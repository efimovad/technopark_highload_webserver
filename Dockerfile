FROM gcc:7.4 
COPY . /usr/src/myapp 
WORKDIR /usr/src/myapp
RUN gcc -o myapp config.h config.c server.c
CMD ["./myapp", "80"]
