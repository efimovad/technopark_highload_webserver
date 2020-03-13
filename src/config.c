#include "config.h"

Config * parse_config(char *filename) {
    FILE *fp;
    char str[128];
    if ((fp=fopen(filename, "r") )==NULL) {
        printf("Cannot open file.\n");
        exit (1);
    }
    
    Config * my_config = (Config *)malloc(sizeof(Config));
    
    int get_cpu_limit = 0;
    int get_thread_limit = 0;
    int get_document_root = 0;
    
    while(!feof(fp)) {
        if (fgets(str, 126, fp)) {
            char *ptr = strtok(str, " \n");
	        while (ptr != NULL)
	        {
	            if (get_cpu_limit) {
	                my_config->cpu_limit = atoi(ptr);
	                get_cpu_limit = 0;
	                continue;
	            } else if (get_thread_limit) {
	                my_config->thread_limit = atoi(ptr);
	                get_thread_limit = 0;
	                continue;
	            } else if (get_document_root) {
	                strcpy(my_config->document_root, ptr);
	                get_document_root = 0;
	                continue;
	            } else if (!strncmp(ptr, "cpu_limit", 9)) {
	                get_cpu_limit = 1;
	            } else if (!strncmp(ptr, "thread_limit", 12)) {
	                get_thread_limit = 1;
	            } else if (!strncmp(ptr, "document_root", 13)) {
	                get_document_root = 1;
	            }
		        ptr = strtok(NULL, " \n");
	        }
        }
    }
    
    fclose(fp);
    return my_config;
}


