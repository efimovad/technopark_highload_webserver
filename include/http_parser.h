#include <ctype.h>
#include <string.h>
#include <stdio.h>

#include "server_errors.h"

#define BUFSIZE 1024

typedef struct {
    char method[BUFSIZE];
    char uri[BUFSIZE];
    char version[BUFSIZE];
    char filename[BUFSIZE];
    int alive_conn;
} Request;

char *get_mime_type(char *filename);
void urldecode2(char *dst, const char *src);
int parse_request(FILE * stream, Request * request); 
