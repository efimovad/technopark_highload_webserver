FROM gcc:7.4 
COPY . /usr/src/myapp 
WORKDIR /usr/src/myapp
RUN make
CMD ["./myapp.out", "80"]
