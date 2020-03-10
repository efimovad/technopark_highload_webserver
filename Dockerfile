FROM gcc:7.4 
COPY . /usr/src/myapp 
WORKDIR /usr/src/myapp
RUN gcc -O4 -o myapp config.h config.c server.c -lpthread
CMD ["./myapp", "80"]
