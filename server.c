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

#define BUFSIZE 1024
#define MAXERRS 16
#define N_BACKLOG 64
#define MAXFDS 16 * 1024

#define DOCUMENTROOTPATH "/usr/src/myapp"

extern char **environ; /* the environment */


off_t offsets[MAXFDS] = {0};
int active[MAXFDS] = {0};
int remain_data[MAXFDS] = {0};
int fds[MAXFDS] = {0};

void error(char *msg) {
  perror(msg);
  exit(1);
}

void epoll_child_del(int epollfd, int childfd, FILE *stream) {
    if (epoll_ctl(epollfd, EPOLL_CTL_DEL, childfd, NULL) < 0) {
        error("epoll_ctl EPOLL_CTL_DEL");
    }
    fclose(stream);
    close(childfd);
}

void make_socket_non_blocking(int sockfd) {
  int flags = fcntl(sockfd, F_GETFL, 0);
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

void on_peer_connected(int parentfd, int epollfd) {
    struct sockaddr_in peer_addr;
    socklen_t peer_addr_len = sizeof(peer_addr);
    int newsockfd = accept(parentfd, (struct sockaddr*)&peer_addr,
                           &peer_addr_len);
    if (newsockfd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            printf("accept returned EAGAIN or EWOULDBLOCK\n");
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
        event.events = EPOLLOUT | EPOLLIN | EPOLLET;
      
        if (epoll_ctl(epollfd, EPOLL_CTL_ADD, newsockfd, &event) < 0) {
            error("epoll_ctl EPOLL_CTL_ADD");
        }
        
        offsets[newsockfd] = 0;
        active[newsockfd] = 0;
    }
}

int listen_inet_socket(int port) {
    struct sockaddr_in serveraddr;
    int parentfd = socket(AF_INET, SOCK_STREAM, 0);
    if (parentfd < 0) {
        error("ERROR opening socket");
    }    

    int optval = 1;
    setsockopt(parentfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval , sizeof(int));

    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)port);
  
    if (bind(parentfd, (struct sockaddr *) &serveraddr, sizeof(serveraddr)) < 0) {
        error("ERROR on binding");
    }

    if (listen(parentfd, N_BACKLOG) < 0) {
        error("ERROR on listen");
    }
    
    return parentfd;
}

typedef struct _Request {
    char method[BUFSIZE];
    char uri[BUFSIZE];
    char version[BUFSIZE];
    int alive_conn;
} Request;



int handle_http_request(FILE * stream, Request *request) {
    char buf[BUFSIZE];
    if (!fgets(buf, BUFSIZE, stream)) {
        return -1;
    }
    
    printf("%s", buf);

    if (sscanf(buf, "%s %s %s\n", request->method, request->uri, request->version) != 3) {
        return -1;
    }

    fgets(buf, BUFSIZE, stream);
    printf("%s", buf);
    while(strcmp(buf, "\r\n") && fgets(buf, BUFSIZE, stream)) {
        request->alive_conn = strstr(buf, "Connection: keep-alive") ? 1 : 0;
        printf("%s", buf); 
    }
    
    return 0;
}

int write_file_http(int epollfd, int childfd, int size, char *filename) {
    if (fds[childfd] == 0) {
    
        remain_data[childfd] = size;
                        
        int fd = open(filename, O_RDONLY);
        if (fd < 0) {
            return -1;
        }
        
        fds[childfd] = fd;
    }
    
    int sent_bytes = 0;
    
    while (((sent_bytes = sendfile(childfd, fds[childfd], &offsets[childfd], BUFSIZ)) > 0) 
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
        return -1;                  
    }

    offsets[childfd] = 0;
    active[childfd] = 0;
    close(fds[childfd]);
    fds[childfd] = 0;
    
    return 0;
}

int configure_epoll(struct epoll_event **events, int parentfd) {
    int epollfd = epoll_create1(0);
    if (epollfd < 0) {
        error("epoll_create1");
    }
    
    struct epoll_event accept_event;
    accept_event.data.fd = parentfd;
    accept_event.events = EPOLLIN | EPOLLET;
    
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, parentfd, &accept_event) < 0) {
        error("epoll_ctl EPOLL_CTL_ADD");
    }

    *events = calloc(MAXFDS, sizeof(struct epoll_event));
    if (*events == NULL) {
        error("Unable to allocate memory for epoll_events");
    }
    
    return epollfd;
}

int main(int argc, char **argv) {
    Config * config = parse_config("/etc/httpd.conf");
    FILE *stream;
    char filename[BUFSIZE] = {'\0'};
    char cgiargs[BUFSIZE] = {'\0'};
    char *p;
    struct stat sbuf;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }
    
    int portno = atoi(argv[1]);
    int parentfd = listen_inet_socket(portno);
    
    make_socket_non_blocking(parentfd);
    
    struct epoll_event* events;
    int epollfd = configure_epoll(&events, parentfd);
    int childfd;
    
    printf("Start server on %d port\n", portno);
    while (1) {
        int nready = epoll_wait(epollfd, events, MAXFDS, -1);
        for (int i = 0; i < nready; i++) {
            
            if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP)) {
                error("epoll_wait returned EPOLLERR");
            }
            
            if (events[i].data.fd == parentfd) {
                
                on_peer_connected(parentfd, epollfd);
            } else if ((events[i].events & EPOLLOUT) || (events[i].events & EPOLLIN)) {          
                
                int childfd = events[i].data.fd;
                if ((stream = fdopen(childfd, "r+")) == NULL) {
                    error("ERROR on fdopen");
                }

                if (active[childfd] == 1 && 
                        write_file_http(epollfd, childfd, sbuf.st_size, filename) != 0) {  
                    continue;
                }
                    
                Request request;
                if (handle_http_request(stream, &request) != 0) {
                    epoll_child_del(epollfd, childfd, stream);
                    continue;                    
                }
                

                if (strcasecmp(request.method, "GET") && strcasecmp(request.method, "HEAD")) {
                    cerror(stream, request.method, 405, "Not Implemented", 
                    "Server does not implement this method");
                    epoll_child_del(epollfd, childfd, stream);
                    continue;
                }
                
                printf("URI:%s\n", request.uri);

                if (strstr(request.uri, "cgi-bin")) {
                    cerror(stream, request.method, 405, "Not Implemented", 
                     "Server does not implement this method");
                    epoll_child_del(epollfd, childfd, stream);
                    continue;
                }
                
                strtok (request.uri,"?");
                strcpy(cgiargs, "");
                strcpy(filename, ".");
                strcat(filename, request.uri);
                
                int is_index = 0;
                if (request.uri[strlen(request.uri) - 1] == '/') {
                    strcat(filename, "index.html");
                    is_index = 1;
                }
                
                urldecode2(filename, filename);
                
                char rlfilename[BUFSIZE];
                strcpy(rlfilename, config->document_root);
                strcat(rlfilename, &filename[1]);
                
                char abs_path[PATH_MAX];                               
                if (!realpath(rlfilename, abs_path) || 
                        strncmp(config->document_root, 
                                abs_path, strlen(config->document_root)) || 
                        stat(rlfilename, &sbuf) < 0) {     
                   cerror(stream, rlfilename, is_index ? 403 : 404, "Not found", 
                        "Server couldn't find this file");
                    epoll_child_del(epollfd, childfd, stream);
                    continue;  
                }

                if (!strcasecmp(request.method, "GET")) {
                    set_headers(stream, 200, "OK", sbuf.st_size, 
                                    get_mime_type(filename), 
                                    request.alive_conn);
                    fflush(stream);
                    if (write_file_http(epollfd, childfd, sbuf.st_size, filename) != 0) {
                        continue;
                    } 
                } else {
                    set_headers(stream, 200, "OK", sbuf.st_size, 
                                    get_mime_type(filename), 
                                    request.alive_conn);
                    fflush(stream);                    
                }
                epoll_child_del(epollfd, childfd, stream);
            } else {
                error("check event type epoll\n");   
            }
        }
    }
}
