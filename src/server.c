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

#include "config.h"
#include "http_parser.h"

#define BUFSIZE 1024
#define MAXERRS 16
#define N_BACKLOG 100
#define MAXFDS 64 * 1024

#define DOCUMENTROOTPATH "/usr/src/myapp"
#define CONFIGROOTPATH "/etc/httpd.conf"

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
  
    fprintf(stream, "<html><title>Tiny Error</title>");
    fprintf(stream, "<body bgcolor=""ffffff"">\n");
    fprintf(stream, "%d: %s\n", code, shortmsg);
    fprintf(stream, "<p>%s: %s\n", longmsg, cause);
    fprintf(stream, "<hr><em>Web server</em>\n\r\n");
    fflush(stream);
}

off_t offsets[MAXFDS] = {0};
int active[MAXFDS] = {0};
int remain_data[MAXFDS] = {0};
int filedesc[MAXFDS] = {0};

void modify_child_epollfd(int epollfd, int childfd) {
    struct epoll_event event = {0};
    event.data.fd = childfd;
    event.events = EPOLLOUT | EPOLLIN | EPOLLET;
    
    if (epoll_ctl(epollfd, EPOLL_CTL_MOD, childfd, &event) < 0) {
        error("epoll_ctl EPOLL_CTL_MOD");
    }
}

void clean_child_epollfd(int epollfd, int childfd, FILE* stream) {
    if (epoll_ctl(epollfd, EPOLL_CTL_DEL, childfd, NULL) < 0) {
        error("epoll_ctl EPOLL_CTL_DEL");
    }  
    fclose(stream);
    close(childfd);
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

void on_peer_connected(int parentfd, int epollfd, off_t *offsets, int *active) {
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

void on_peer_ready(int epollfd, struct epoll_event event) {
    Config * config = parse_config(CONFIGROOTPATH);
    int childfd = event.data.fd;
    int sent_bytes = 0;
    struct stat sbuf;
    
    FILE * stream;   
    if ((stream = fdopen(childfd, "r+")) == NULL)
        error("ERROR on fdopen");
    
    if (active[childfd] == 1) {
        goto writefile;
    }
    
    Request request;
    int error = parse_request(stream, &request);
    if (error != OK) {
        if (error == FAIL) {
            cerror(stream, request.method, 405, "Not Implemented", 
                "Server does not implement this method");
            clean_child_epollfd(epollfd, childfd, stream);
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
                    
        remain_data[childfd] = sbuf.st_size;
        
        int fd = open(filename, O_RDONLY);
        if (fd < 0) {
            clean_child_epollfd(epollfd, childfd, stream);
            return;
        }
        
        filedesc[childfd] = fd;                    
                         
writefile:          
        sent_bytes = 0;
        while (((sent_bytes = sendfile(childfd, filedesc[childfd], &offsets[childfd], BUFSIZ)) > 0) 
                  && (remain_data[childfd] > 0)) {
            remain_data[childfd] -= sent_bytes;
        }
        
        if (sent_bytes != 0 && errno == EAGAIN) {
            modify_child_epollfd(epollfd, childfd);
            active[childfd] = 1;
            return;                  
        }
        
        offsets[childfd] = 0;
        active[childfd] = 0;
        close(filedesc[childfd]);
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
    accept_event.events = EPOLLIN;
    
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, parentfd, &accept_event) < 0) {
        error("epoll_ctl EPOLL_CTL_ADD");
    }
    
    return epollfd;
}

void* worker(void* dat) {

    int parentfd = *((int*) dat);
    int epollfd = configure_epoll(parentfd);
    struct epoll_event events[MAXFDS];
    
    while (1) {
        int nready = epoll_wait(epollfd, events, MAXFDS, -1);
        
        for (int i = 0; i < nready; i++) {
            
            if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP)) {
                error("epoll_wait returned EPOLLERR");
            }
            
            if (events[i].data.fd == parentfd) {              
                
                on_peer_connected(parentfd, epollfd, offsets, active);
                
            } else if ((events[i].events & EPOLLOUT) || (events[i].events & EPOLLIN)) {
                
                on_peer_ready(epollfd, events[i]);
                
            } else {
                error("check event type epoll\n");   
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

    int parentfd = socket(AF_INET, SOCK_STREAM, 0);
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

    if (listen(parentfd, SOMAXCONN) < 0)
        error("ERROR on listen");
    
    make_socket_non_blocking(parentfd);
    
    worker(&parentfd);
}
