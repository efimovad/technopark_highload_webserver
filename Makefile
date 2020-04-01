TARGET = myapp.out
HDRS_DIR = include

SRCS = \
       src/config.c \
       src/http_parser.c \
       src/server.c

.PHONY: all clean

all: $(SRCS)
	$(CC) -Wall -Wextra -Werror -I $(HDRS_DIR) -o $(TARGET) $(CFLAGS) $(SRCS) -lpthread

test: $(eval override CFLAGS +=-D test=0)
	make all

clean:
	rm -rf $(TARGET)

