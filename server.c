#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <limits.h>
#include <ctype.h>
#include <errno.h>
#include "config.h"
#include <sys/sendfile.h>

#include <pthread.h>

#define BUFSIZE 1024
#define MAXERRS 16
#define N_BACKLOG 100
#define MAXFDS 64 * 1024

#define DOCUMENTROOTPATH "/usr/src/myapp"

extern char **environ; /* the environment */

pthread_mutex_t lock;

void error(char *msg) {
  perror(msg);
  exit(1);
}

void make_socket_non_blocking(int sockfd) {
  int flags = fcntl(sockfd, F_GETFL, 0);
  //printf("SOCKFD %d\n", sockfd);
  if (flags == -1) {
    error("fcntl F_GETFL");
  }

  if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
    error("fcntl F_SETFL O_NONBLOCK");
  }
}

char* get_UTC() {
    char *buf = (char*)calloc(1000, sizeof(char));
    time_t now = time(0);
    struct tm tm = *gmtime(&now);
    strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S %Z", &tm);
    return buf;
}

char *get_mime_type(char *filename) {
    if (strstr(filename, ".html"))
        return "text/html";
    else if (strstr(filename, ".gif"))
        return "image/gif";
    else if (strstr(filename, ".jpg"))
        return "image/jpeg";
    else if (strstr(filename, ".css"))
        return "text/css";
    else if (strstr(filename, ".js"))
        return "text/javascript";
    else if (strstr(filename, ".jpeg"))
        return "image/jpeg";
    else if (strstr(filename, ".png"))
        return "image/png";
    else if (strstr(filename, ".swf"))
        return "application/x-shockwave-flash";
    else 
        return "text/plain";
}


void set_headers(FILE * stream, int code, char * status, off_t length, 
                    char * mime_type, int live_conn) {
	char * date = get_UTC();
    fprintf(stream, "HTTP/1.1 %d %s\r\n", code, status);
    fprintf(stream, "Content-Length: %jd\r\n", length);
    //printf("Content-Length: %jd\r\n", length);
    fprintf(stream, "Content-Type: %s\r\n", mime_type);
    fprintf(stream, "Date: %s\r\n", date);
    free(date);
    fprintf(stream, "Server: Web Server\r\n");
    fprintf(stream, "Connection: %s\r\n", live_conn ? "keep-alive" : "close");
    fprintf(stream, "\r\n");
}

void cerror(FILE *stream, char *cause, int code, 
	    char *shortmsg, char *longmsg) {
    set_headers(stream, code, shortmsg, sizeof(longmsg), "text/html", 0);
  
    fprintf(stream, "<html><title>Tiny Error</title>");
    fprintf(stream, "<body bgcolor=""ffffff"">\n");
    fprintf(stream, "%d: %s\n", code, shortmsg);
    fprintf(stream, "<p>%s: %s\n", longmsg, cause);
    fprintf(stream, "<hr><em>Web server</em>\n\r\n");
    fflush(stream);
}


void urldecode2(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a')
                a -= 'a'-'A';
            if (a >= 'A')
                a -= ('A' - 10);
            else
                a -= '0';

            if (b >= 'a')
                b -= 'a'-'A';
            if (b >= 'A')
                b -= ('A' - 10);
            else
                b -= '0';

            *dst++ = 16*a+b;
            src+=3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst++ = '\0';
}

void on_peer_connected(int parentfd, int epollfd, off_t *offsets, int *active) {
    struct sockaddr_in peer_addr;
    socklen_t peer_addr_len = sizeof(peer_addr);
    
    //pthread_mutex_lock(&lock);
    int newsockfd = accept(parentfd, (struct sockaddr*)&peer_addr,
                           &peer_addr_len);
    //pthread_mutex_unlock(&lock);
    
    if (newsockfd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            //printf("accept returned EAGAIN or EWOULDBLOCK\n");
        } else {
            error("accept");
        }
    } else {
        make_socket_non_blocking(newsockfd);
        if (newsockfd >= MAXFDS) {
            error("socket fd >= MAXFDS");
        }

        struct epoll_event event = {0};
        event.data.fd = newsockfd;
        event.events = EPOLLOUT | EPOLLIN | EPOLLET; // EPOLLIN | EPOLLET
      
        if (epoll_ctl(epollfd, EPOLL_CTL_ADD, newsockfd, &event) < 0) {
            error("epoll_ctl EPOLL_CTL_ADD");
        }
        
        offsets[newsockfd] = 0;
        active[newsockfd] = 0;
        
        //printf("GET NEW CONN\n");
    }
}


void* worker(void* dat) {
    off_t offsets[MAXFDS] = {0};
    int active[MAXFDS] = {0};
    int remain_data[MAXFDS] = {0};
    int filedesc[MAXFDS] = {0};

    int sent_bytes;
    int childfd;  
    
        /* variables for connection I/O */
    FILE *stream;          /* stream version of childfd */
    char buf[BUFSIZE];     /* message buffer */
    char method[BUFSIZE];  /* request method */
    char uri[BUFSIZE];     /* request uri */
    char version[BUFSIZE]; /* request method */
    char filename[BUFSIZE];/* path derived from uri */
    char cgiargs[BUFSIZE]; /* cgi argument list */
    char *p;               /* temporary pointer */
    struct stat sbuf;      /* file status */
    int keepa_live_conn = 0;
   
    int parentfd = *((int*) dat);
    
    Config * config = parse_config("/etc/httpd.conf");
    
    int epollfd = epoll_create1(0);
    if (epollfd < 0) {
        error("epoll_create1");
    }
    
    struct epoll_event accept_event;
    accept_event.data.fd = parentfd;
    accept_event.events = EPOLLIN;
    
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, parentfd, &accept_event) < 0) {
        error("epoll_ctl EPOLL_CTL_ADD");
    }

    struct 
    epoll_event* events = calloc(MAXFDS, sizeof(struct epoll_event));
    if (events == NULL) {
        error("Unable to allocate memory for epoll_events");
    }
    
    int cnt=0;
    int cnt_all = 0;
    
    while (1) {
        int nready = epoll_wait(epollfd, events, MAXFDS, -1);
        //printf("nready: %d\n", nready);
        for (int i = 0; i < nready; i++) {
            //printf("event: %d nready: %d\n", events[i].events, nready);
            if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP)) {
                error("epoll_wait returned EPOLLERR");
            }
            
            if (events[i].data.fd == parentfd) {              
                on_peer_connected(parentfd, epollfd, offsets, active);
                
            } else if ((events[i].events & EPOLLOUT) || (events[i].events & EPOLLIN)) {
                pthread_mutex_lock(&lock);
                //printf("EPOLLOUT EPOLLIN\n");
                
                int childfd = events[i].data.fd;
                    
                if ((stream = fdopen(childfd, "r+")) == NULL)
                    error("ERROR on fdopen");

                //printf("BEFORE ACTIVE\n");
                if (active[childfd] == 1){
                    //printf("GOTO\n");
                    goto writefile;
                }
                    
                if (!fgets(buf, BUFSIZE, stream)) {
                    pthread_mutex_unlock(&lock);
                    continue;
                }
                
                //printf("%s", buf);
                
                int method_len = sscanf(buf, "%s %s %s\n", method, uri, version);
                if (method_len != 3) {
                    cnt--;
                    if (epoll_ctl(epollfd, EPOLL_CTL_DEL, childfd, NULL) < 0) {
                        error("epoll_ctl EPOLL_CTL_DEL");
                    }
                    fclose(stream);
                    close(childfd);
                    pthread_mutex_unlock(&lock);
                    continue;
                }
                fgets(buf, BUFSIZE, stream);
                //printf("%s", buf);
                while(strcmp(buf, "\r\n") && fgets(buf, BUFSIZE, stream)) {
                    keepa_live_conn = strstr(buf, "Connection: keep-alive") ? 1 : 0; 
                    //printf("%s", buf); 
                }

                if (strcasecmp(method, "GET") && strcasecmp(method, "HEAD")) {
                    cerror(stream, method, 405, "Not Implemented", 
                     "Server does not implement this method");
                     cnt--;
                    if (epoll_ctl(epollfd, EPOLL_CTL_DEL, childfd, NULL) < 0) {
                        error("epoll_ctl EPOLL_CTL_DEL");
                    }
                    fclose(stream);
                    close(childfd);
                    pthread_mutex_unlock(&lock);
                    continue;
                }
           
                
                int is_index = 0;
                if (!strstr(uri, "cgi-bin")) {
                    strtok (uri,"?");
                    strcpy(cgiargs, "");
                    strcpy(filename, ".");
                    strcat(filename, uri);
                    if (uri[strlen(uri)-1] == '/') {
                        strcat(filename, "index.html");
                        is_index = 1;
                    }    
                } else {
                    cerror(stream, method, 405, "Not Implemented", 
                     "Server does not implement this method");
                     cnt--;
                    if (epoll_ctl(epollfd, EPOLL_CTL_DEL, childfd, NULL) < 0) {
                        error("epoll_ctl EPOLL_CTL_DEL");
                    }
                    fclose(stream);
                    close(childfd);
                    pthread_mutex_unlock(&lock);
                    continue;
                }
                
                urldecode2(filename, filename);
                
                char rlfilename[BUFSIZE];
                strcpy(rlfilename, config->document_root);
                strcat(rlfilename, &filename[1]);
                
                char abs_path[PATH_MAX];
                char *res = realpath(rlfilename, abs_path);
                
                
                if (!res || 
                    strncmp(config->document_root, abs_path, strlen(config->document_root)) || 
                    stat(rlfilename, &sbuf) < 0) {     
                   cerror(stream, rlfilename, is_index ? 403 : 404, "Not found", 
                        "Server couldn't find this file");
                    if (epoll_ctl(epollfd, EPOLL_CTL_DEL, childfd, NULL) < 0) {
                        error("epoll_ctl EPOLL_CTL_DEL");
                    }    
                    fclose(stream);
                    close(childfd);
                    pthread_mutex_unlock(&lock);
                    continue;   
                }

                if (!strcasecmp(method, "GET")) {
                    set_headers(stream, 200, "OK", sbuf.st_size, get_mime_type(filename), 
                                        keepa_live_conn);
                    fflush(stream);
                                
                    
                    remain_data[childfd] = sbuf.st_size;
                    
                    int flag = 0;
                    int fd = open(rlfilename, O_RDONLY);
                    if (fd < 0) {
                        goto cleanup;
                    }
                    
                    filedesc[childfd] = fd;                    
                                     
writefile:          
                    sent_bytes = 0;
                    while (((sent_bytes = sendfile(childfd, filedesc[childfd], &offsets[childfd], BUFSIZ)) > 0) 
                              && (remain_data[childfd] > 0)) {
                        remain_data[childfd] -= sent_bytes;
                    }
                    
                    if (sent_bytes != 0 && errno == EAGAIN) {
                        struct epoll_event event = {0};
                        event.data.fd = childfd;
                        event.events = EPOLLOUT | EPOLLIN | EPOLLET;
                        
                        if (epoll_ctl(epollfd, EPOLL_CTL_MOD, childfd, &event) < 0) {
                            error("epoll_ctl EPOLL_CTL_MOD");
                        }
                        active[childfd] = 1;
                        pthread_mutex_unlock(&lock);
                        continue;                  
                    }
                    
                    offsets[childfd] = 0;
                    active[childfd] = 0;
                    close(filedesc[childfd]);
                } else {
                    set_headers(stream, 200, "OK", sbuf.st_size, get_mime_type(filename), 
                                        keepa_live_conn);
                    fflush(stream);                    
                }
 
cleanup:               
                if (epoll_ctl(epollfd, EPOLL_CTL_DEL, childfd, NULL) < 0) {
                    error("epoll_ctl EPOLL_CTL_DEL");
                }
                
                fclose(stream);
                close(childfd);
                pthread_mutex_unlock(&lock);
            } else {
                error("check event type epoll\n");   
            }
        }
    } 
    
    return NULL;
}

int main(int argc, char **argv) {

    //Config * config = parse_config("/etc/httpd.conf");

    struct sockaddr_in serveraddr; /* server's addr */ 
    int portno;            /* port to listen on */
    int parentfd;          /* parent socket */

    /* check command line args */
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[1]);

    /* open socket descriptor */
    parentfd = socket(AF_INET, SOCK_STREAM, 0);
    if (parentfd < 0) 
        error("ERROR opening socket");

    /* allows us to restart server immediately */
    int optval = 1;
    setsockopt(parentfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval , sizeof(int));

    /* bind port to socket */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);
  
    if (bind(parentfd, (struct sockaddr *) &serveraddr, sizeof(serveraddr)) < 0) 
        error("ERROR on binding");

    /* get us ready to accept connection requests */
    //if (listen(parentfd, N_BACKLOG) < 0) /* allow 5 requests to queue up */ 
    if (listen(parentfd, SOMAXCONN) < 0)
    error("ERROR on listen");
    
    make_socket_non_blocking(parentfd);
    
    //int NUM_THREADS = 8;
    
    //pthread_t* thread = (pthread_t*) malloc(sizeof(pthread_t*)*NUM_THREADS);
    
    /*if (pthread_mutex_init(&lock, NULL) != 0) {
        printf("\n mutex init failed\n");
        return 1;
    }
    
    for (int i = 0; i < NUM_THREADS; ++i) {
        if (pthread_create(&thread[i], NULL, worker, &parentfd)) {
            error("create pthread");
        }
    }
    
    
    for (int i = 0; i < NUM_THREADS; ++i) {
        pthread_join(thread[i], NULL) ;
    }*/ 
    
    worker(&parentfd);
    
    /*int epollfd = epoll_create1(0);
    if (epollfd < 0) {
        error("epoll_create1");
    }
    
    struct epoll_event accept_event;
    accept_event.data.fd = parentfd;
    accept_event.events = EPOLLIN;
    
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, parentfd, &accept_event) < 0) {
        error("epoll_ctl EPOLL_CTL_ADD");
    }

    struct 
    epoll_event* events = calloc(MAXFDS, sizeof(struct epoll_event));
    if (events == NULL) {
        error("Unable to allocate memory for epoll_events");
    }
    
    int cnt=0;
    int cnt_all = 0;
    
    while (1) {
        int nready = epoll_wait(epollfd, events, MAXFDS, -1);
        //printf("nready: %d\n", nready);
        for (int i = 0; i < nready; i++) {
            //printf("event: %d nready: %d\n", events[i].events, nready);
            if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP)) {
                error("epoll_wait returned EPOLLERR");
            }
            
            if (events[i].data.fd == parentfd) {
                
                //printf("FIND NEW PEER\n");
                cnt++; cnt_all++;
                //printf("listened %d all %d\n", cnt, cnt_all);
                on_peer_connected(parentfd, epollfd);
                
            } else if ((events[i].events & EPOLLOUT) || (events[i].events & EPOLLIN)) {
                //printf("EPOLLOUT EPOLLIN\n");
                
                int childfd = events[i].data.fd;
                    
                if ((stream = fdopen(childfd, "r+")) == NULL)
                    error("ERROR on fdopen");

                //printf("BEFORE ACTIVE\n");
                if (active[childfd] == 1){
                    //printf("GOTO\n");
                    goto writefile;
                }
                    
                if (!fgets(buf, BUFSIZE, stream)) {
                    continue;
                }
                
                //printf("%s", buf);
                
                int method_len = sscanf(buf, "%s %s %s\n", method, uri, version);
                if (method_len != 3) {
                    cnt--;
                    if (epoll_ctl(epollfd, EPOLL_CTL_DEL, childfd, NULL) < 0) {
                        error("epoll_ctl EPOLL_CTL_DEL");
                    }
                    fclose(stream);
                    close(childfd);
                    continue;
                }
                fgets(buf, BUFSIZE, stream);
                //printf("%s", buf);
                while(strcmp(buf, "\r\n") && fgets(buf, BUFSIZE, stream)) {
                    keepa_live_conn = strstr(buf, "Connection: keep-alive") ? 1 : 0; 
                    //printf("%s", buf); 
                }

                if (strcasecmp(method, "GET") && strcasecmp(method, "HEAD")) {
                    cerror(stream, method, 405, "Not Implemented", 
                     "Server does not implement this method");
                     cnt--;
                    if (epoll_ctl(epollfd, EPOLL_CTL_DEL, childfd, NULL) < 0) {
                        error("epoll_ctl EPOLL_CTL_DEL");
                    }
                    fclose(stream);
                    close(childfd);
                    continue;
                }
           
                
                int is_index = 0;
                if (!strstr(uri, "cgi-bin")) {
                    strtok (uri,"?");
                    strcpy(cgiargs, "");
                    strcpy(filename, ".");
                    strcat(filename, uri);
                    if (uri[strlen(uri)-1] == '/') {
                        strcat(filename, "index.html");
                        is_index = 1;
                    }    
                } else {
                    cerror(stream, method, 405, "Not Implemented", 
                     "Server does not implement this method");
                     cnt--;
                    if (epoll_ctl(epollfd, EPOLL_CTL_DEL, childfd, NULL) < 0) {
                        error("epoll_ctl EPOLL_CTL_DEL");
                    }
                    fclose(stream);
                    close(childfd);
                    continue;
                }
                
                urldecode2(filename, filename);
                
                char rlfilename[BUFSIZE];
                strcpy(rlfilename, config->document_root);
                strcat(rlfilename, &filename[1]);
                
                char abs_path[PATH_MAX];
                char *res = realpath(rlfilename, abs_path);
                
                
                if (!res || 
                    strncmp(config->document_root, abs_path, strlen(config->document_root)) || 
                    stat(rlfilename, &sbuf) < 0) {     
                   cerror(stream, rlfilename, is_index ? 403 : 404, "Not found", 
                        "Server couldn't find this file");
                    if (epoll_ctl(epollfd, EPOLL_CTL_DEL, childfd, NULL) < 0) {
                        error("epoll_ctl EPOLL_CTL_DEL");
                    }    
                    fclose(stream);
                    close(childfd);
                    continue;   
                }

                if (!strcasecmp(method, "GET")) {
                    set_headers(stream, 200, "OK", sbuf.st_size, get_mime_type(filename), 
                                        keepa_live_conn);
                    fflush(stream);
                                
                    
                    remain_data[childfd] = sbuf.st_size;
                    
                    int flag = 0;
                    int fd = open(filename, O_RDONLY);
                    if (fd < 0) {
                        goto cleanup;
                    }
                    
                    filedesc[childfd] = fd;                    
                                     
writefile:          
                    sent_bytes = 0;
                    while (((sent_bytes = sendfile(childfd, filedesc[childfd], &offsets[childfd], BUFSIZ)) > 0) && (remain_data[childfd] > 0))
                    {
                        remain_data[childfd] -= sent_bytes;
                    }
                    
                    if (sent_bytes != 0 && errno == EAGAIN) {
                        struct epoll_event event = {0};
                        event.data.fd = childfd;
                        event.events = EPOLLOUT | EPOLLIN | EPOLLET;
                        
                        if (epoll_ctl(epollfd, EPOLL_CTL_MOD, childfd, &event) < 0) {
                            error("epoll_ctl EPOLL_CTL_MOD");
                        }
                        active[childfd] = 1;
                        continue;                  
                    }
                    
                    offsets[childfd] = 0;
                    active[childfd] = 0;
                    close(filedesc[childfd]);
                    
                    
                } else {
                    set_headers(stream, 200, "OK", sbuf.st_size, get_mime_type(filename), 
                                        keepa_live_conn);
                    fflush(stream);                    
                }
 
cleanup:               
                cnt--;
                if (epoll_ctl(epollfd, EPOLL_CTL_DEL, childfd, NULL) < 0) {
                    error("epoll_ctl EPOLL_CTL_DEL");
                }
                
                fclose(stream);
                close(childfd);
            } else {
                error("check event type epoll\n");   
            }
        }
    }*/
}
