# technopark_highload_webserver
Serving static web-server (epoll architecture)

### Docker
You need safe your static files in /var/www/html path and configuration file in /etc/httpd.conf
```sh
$:docker build -t epollserver github.com/efimovad/technopark_highload_webserver.git
$:docker run -p 80:80 -v /etc/httpd.conf:/etc/httpd.conf:ro -v /var/www/html:/var/www/html:ro --name epollservre -t epollserver
```
