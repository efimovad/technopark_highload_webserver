#include "http_parser.h"

#include <errno.h>

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

int parse_request(FILE * stream, Request * request) {
    char buf[BUFSIZE];
    //fgets(buf, BUFSIZE, stream);
    if (!fgets(buf, BUFSIZE, stream)) {
        //printf("WAIT 1\n");
        //if (errno == EAGAIN) printf("EAGAIN\n");    
        return WAIT;
    }
    
    int method_len = sscanf(buf, "%s %s %s\n", request->method, 
                                               request->uri, 
                                               request->version);
    if (method_len != 3) {
        return FAIL;
    }
    
    if (!fgets(buf, BUFSIZE, stream)) {
        //printf("WAIT 2 \n");   
        return WAIT;
    }
    
    while(strcmp(buf, "\r\n") && fgets(buf, BUFSIZE, stream)) {
        request->alive_conn = strstr(buf, "Connection: keep-alive") ? 1 : 0;
    }
    
    if ((strcasecmp(request->method, "GET") && strcasecmp(request->method, "HEAD")) || 
            strstr(request->uri, "cgi-bin")) {
        return FAIL;
    }
    
    return OK;
}

