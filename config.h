#include <string.h>
#include <stdio.h>
#include <stdlib.h>

struct {
    int cpu_limit;
    int thread_limit;
    char document_root[100];
} typedef Config;


Config * parse_config(char *filename);
