#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    int cpu_limit;
    int thread_limit;
    char document_root[100];
} Config;


Config * parse_config(char *filename);
