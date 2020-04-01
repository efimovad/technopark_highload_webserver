#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <limits.h>
#include <ctype.h>
#include <errno.h>
#include <sys/sendfile.h>
#include <pthread.h>

#include <sys/syscall.h>


#include "config.h"
#include "http_parser.h"

#define BUFSIZE 1024
#define MAXERRS 16
#define MAXFDS 1024

#define CONFIGROOTPATH "/etc/httpd.conf"

typedef enum {INIT, PROC, CONN, VERIFY, TERM} PeerState;

typedef struct {
    PeerState state;
    int remain_data;
    int fd;
    FILE * stream;
    off_t offset;
} Peer;

Peer peers[MAXFDS];

pthread_mutex_t mutex_conn;

void error(char *msg) {
  perror(msg);
  exit(1);
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
  
    fprintf(stream, "<html><title>Error</title>");
    fprintf(stream, "<body bgcolor=""ffffff"">\n");
    fprintf(stream, "%d: %s\n", code, shortmsg);
    fprintf(stream, "<p>%s: %s\n", longmsg, cause);
    fprintf(stream, "<hr><em>Web server</em>\n\r\n");
    fflush(stream);
}

void add_child_epollfd(int epollfd, int childfd) {
    struct epoll_event event = {0};
    event.data.fd = childfd;
    event.events = EPOLLIN | EPOLLOUT | EPOLLET | EPOLLONESHOT;
  
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, childfd, &event) < 0) {
        error("epoll_ctl EPOLL_CTL_ADD");
    }
}

void modify_child_epollfd(int epollfd, int childfd, uint32_t flags) {
    pthread_mutex_lock(&mutex_conn);
    struct epoll_event event = {0};
    event.data.fd = childfd;
    event.events = flags;
 
    if (epoll_ctl(epollfd, EPOLL_CTL_MOD, childfd, &event) < 0) {
        error("epoll_ctl EPOLL_CTL_MOD");
    }
    pthread_mutex_unlock(&mutex_conn);
}

void clean_child_epollfd(int epollfd, int childfd, FILE* stream) {
    pthread_mutex_lock(&mutex_conn);
    if (epoll_ctl(epollfd, EPOLL_CTL_DEL, childfd, NULL) < 0) {
        error("epoll_ctl EPOLL_CTL_DEL");
    }
    fclose(stream);  
    close(childfd);
    pthread_mutex_unlock(&mutex_conn);
}

int verify_file(char *uri, char * document_root, char *filename, struct stat *sbuf) {
    strtok (uri,"?");
    strcpy(filename, ".");
    strcat(filename, uri);
    
    if (uri[strlen(uri)-1] == '/') {
        strcat(filename, "index.html");
    }
    
    urldecode2(filename, filename);
    
    char rlfilename[BUFSIZE];
    strcpy(rlfilename, document_root);
    strcat(rlfilename, &filename[1]);
    
    char abs_path[PATH_MAX];
    char *res = realpath(rlfilename, abs_path);
    
    if (!res || 
        strncmp(document_root, abs_path, strlen(document_root)) || 
        stat(rlfilename, sbuf) < 0) {
        return FAIL;    
    }
    
    strcpy(filename, rlfilename);
    
    return OK;
}

void on_peer_connected(int parentfd, int epollfd) {
    struct sockaddr_in peer_addr;
    socklen_t peer_addr_len = sizeof(peer_addr);
    
    pthread_mutex_lock(&mutex_conn);
    int newsockfd = accept(parentfd, (struct sockaddr*)&peer_addr,
                           &peer_addr_len);
    pthread_mutex_unlock(&mutex_conn);
                           
    if (newsockfd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            error("accept");
        }
    } else {
        make_socket_non_blocking(newsockfd);
        if (newsockfd >= MAXFDS) {
            return;
        }
        add_child_epollfd(epollfd, newsockfd);
        peers[newsockfd].offset = 0;
        peers[newsockfd].state = INIT;
    }
}


FILE * get_stream(int childfd) {
    if (peers[childfd].state == VERIFY) {
        return peers[childfd].stream;
    } 
        
    return fdopen(childfd, "r+");
}

int send_file_to_peer(int epollfd, int childfd, FILE * stream, char * filename, struct stat sbuf) {
    if (filename != NULL || peers[childfd].state != PROC) {
        peers[childfd].remain_data = sbuf.st_size;
        
        int fd = open(filename, O_RDONLY);
        if (fd < 0) {
            clean_child_epollfd(epollfd, childfd, stream);
            return 1;
        }
        
        peers[childfd].fd = fd;                    
    }
              
    int sent_bytes = 0; 
    while (((sent_bytes = sendfile(childfd, peers[childfd].fd, &peers[childfd].offset, BUFSIZ)) > 0) 
              && (peers[childfd].remain_data > 0)) {
        peers[childfd].remain_data -= sent_bytes;
    }

    if (sent_bytes != 0 && errno == EAGAIN) {
        uint32_t flags = EPOLLOUT | EPOLLET | EPOLLONESHOT | EPOLLPRI;
        modify_child_epollfd(epollfd, childfd, flags);
        peers[childfd].state = PROC;
        return 1;                  
    }
    
    peers[childfd].offset = 0;
    peers[childfd].state = INIT;
    close(peers[childfd].fd);
    return 0;
}

void on_peer_ready(int epollfd, struct epoll_event event, Config *config) {
    int childfd = event.data.fd;
    struct stat sbuf;
    
    //printf("[%ld] %d ON PEER READY %d\n", syscall(__NR_gettid), getpid(), childfd);
    
    FILE * stream = get_stream(childfd);
    if (stream == NULL) error("fdopen");
    
    
    if (peers[childfd].state == PROC) {
        if (send_file_to_peer(epollfd, childfd, stream, NULL, sbuf)) {
            return;
        }
        clean_child_epollfd(epollfd, childfd, stream);
        return;
    }
    
    Request request;
    int error = parse_request(stream, &request);
    if (error != OK) {
        if (error == FAIL) {
            cerror(stream, request.method, 405, "Not Implemented", 
                "Server does not implement this method");
            clean_child_epollfd(epollfd, childfd, stream);
            return;
        }
        
        if (peers[childfd].state != VERIFY) {
            uint32_t flags = EPOLLIN | EPOLLET | EPOLLONESHOT | EPOLLPRI;
            modify_child_epollfd(epollfd, childfd, flags);
            
            peers[childfd].stream = stream;
            peers[childfd].state = VERIFY;
            //printf("%d RETURN AFTER PARSE REQ %d %d\n", getpid(), childfd, active[childfd]);
        } else { 
            clean_child_epollfd(epollfd, childfd, stream); 
            peers[childfd].state = INIT;
        }
        return;
    }
    
    char filename[BUFSIZE];
    if (verify_file(request.uri, config->document_root, filename, &sbuf) != OK) {   
       cerror(stream, filename, strstr(filename, "index.html") ? 403 : 404, 
                    "Not found", "Server couldn't find this file");
       clean_child_epollfd(epollfd, childfd, stream);
       return;
    }

    if (!strcasecmp(request.method, "GET")) {
        set_headers(stream, 200, "OK", sbuf.st_size, get_mime_type(filename), 
                            request.alive_conn);
        fflush(stream);
       
        if (send_file_to_peer(epollfd, childfd, stream, filename, sbuf)) {
            return;
        }
    } else {
        set_headers(stream, 200, "OK", sbuf.st_size, get_mime_type(filename), 
                            request.alive_conn);
        fflush(stream);                    
    }
            
    clean_child_epollfd(epollfd, childfd, stream);
}

int configure_epoll(int parentfd) {
    int epollfd = epoll_create1(0);
    if (epollfd < 0) {
        error("epoll_create1");
    }
    
    struct epoll_event accept_event;
    accept_event.data.fd = parentfd;
    accept_event.events = EPOLLIN | EPOLLEXCLUSIVE;
    
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, parentfd, &accept_event) < 0) {
        error("epoll_ctl EPOLL_CTL_ADD");
    }
    
    return epollfd;
}

int parentfd;

void* worker(void* dat) {
    Config * config = parse_config(CONFIGROOTPATH);
    int epollfd = *((int*) dat);
    struct epoll_event events[MAXFDS];
    
    
    while (1) {
        //printf("[%ld] wait\n",  syscall(__NR_gettid));
        int nready = epoll_wait(epollfd, events, MAXFDS, -1);
       
        for (int i = 0; i < nready; i++) {
                        
            if (events[i].data.fd == parentfd) {              
                
                on_peer_connected(parentfd, epollfd);    
                
            } else if ((events[i].events & EPOLLOUT) || (events[i].events & EPOLLIN)) {
                               
                on_peer_ready(epollfd, events[i], config);
            }
        }
    } 
    
    return NULL;
}

int main(int argc, char **argv) { 
    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }
    int portno = atoi(argv[1]);

    parentfd = socket(AF_INET, SOCK_STREAM, 0);
    if (parentfd < 0) 
        error("ERROR opening socket");

    int optval = 1;
    setsockopt(parentfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval , sizeof(int));

    struct sockaddr_in serveraddr;
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);
  
    if (bind(parentfd, (struct sockaddr *) &serveraddr, sizeof(serveraddr)) < 0) 
        error("ERROR on binding");

    printf("SOMAXCONN: %d\n", SOMAXCONN);

    if (listen(parentfd, 1024) < 0)
        error("ERROR on listen");
    
    make_socket_non_blocking(parentfd);
   
    int epollfd = configure_epoll(parentfd);
    int pthnum = 2;	
    pthread_t * threads = (pthread_t*)malloc(pthnum * sizeof(pthread_t));	
    	
    for (int i = 0; i < pthnum; i++) {	
        if(pthread_create(&threads[i], NULL, worker, &epollfd)) {	
            error("Error creating thread\n");	
        }	
    }	
    	
    for (int i = 0; i < pthnum; i++) {	
        if(pthread_join(threads[i], NULL)) {	
            error("Error creating thread\n");	
        }	
    }	
    	
    //if (fork()) worker(&epollfd); else worker(&epollfd);
    //fork();
    
    /*int pthnum = 3;
    for (int i = 0; i < pthnum; i++) {
        if (fork() == 0) {
            int epollfd = configure_epoll(parentfd);
            worker(&epollfd);
        }
    }
    
    int epollfd = configure_epoll(parentfd);
    worker(&epollfd);*/
}
