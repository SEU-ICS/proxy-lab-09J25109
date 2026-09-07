#include "csapp.h"
#include <stdio.h>
#include <string.h>

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

void parse_uri(char *uri, char *hostname, char *port, char *path)
{
    char *host_start, *path_start;

    host_start = uri + (!strncasecmp(uri, "http://", 7))*7;

    path_start = strchr(host_start, '/');
    if(path_start != NULL)
        strcpy(path, path_start);
    else
        strcpy(path, "/");
    
    size_t host_len = strcspn(host_start, ":/");
    memcpy(hostname, host_start, host_len);
    hostname[host_len] = '\0';

    const char *p = host_start + host_len;
    if(*p == ':')
    {
        size_t port_len = strcspn(p + 1, "/");
        memcpy(port, p + 1, port_len);
        port[port_len] = '\0';
    }
    else    strcpy(port, "80");
}

void build_headers(rio_t *client_rio, char *request, const char *hostname, const char *port, const char *path)
{
    char buf[MAXLINE];
    char other_headers[MAXLINE * 10] = "";
    char host_header[MAXLINE] = "";

    sprintf(request, "GET %s HTTP/1.0\r\n", path);

    while(Rio_readlineb(client_rio, buf, MAXLINE) > 0) {
        if(!strcmp(buf, "\r\n"))
            break;
        if(!strncasecmp(buf, "Host:", 5))
            strcpy(host_header, buf);
        else    if(!strncasecmp(buf, "User-Agent:", 11)) {}
        else    if(!strncasecmp(buf, "Connection:", 11)) {}
        else    if(!strncasecmp(buf, "Proxy-Connection:", 17)) {}
        else    strcat(other_headers, buf);
    }

    if(strlen(host_header) == 0)
    {
        if(!strcmp(port, "80"))
            sprintf(host_header, "Host: %s\r\n", hostname);
        else
            sprintf(host_header, "Host: %s:%s\r\n", hostname, port);
    }

    strcat(request, host_header);
    strcat(request, user_agent_hdr);
    strcat(request, "Connection: close\r\n");
    strcat(request, "Proxy-Connection: close\r\n");
    strcat(request, other_headers);
    strcat(request, "\r\n");
}

void doit(int connfd)
{
    rio_t rio, server_rio;
    char buf[MAXLINE];
    char method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char hostname[MAXLINE], port[MAXLINE], path[MAXLINE];

    Rio_readinitb(&rio, connfd);
    if(!Rio_readlineb(&rio, buf, MAXLINE))  return;
    sscanf(buf, "%s %s %s", method, uri, version);
    if(strcasecmp(method, "GET"))
    {
        printf("Unsupported method: %s\n", method);
        return;
    }
    parse_uri(uri, hostname, port, path);


    int serverfd = Open_clientfd(hostname, port);
    if(serverfd < 0)    return;
    char request[MAXLINE * 10];
    build_headers(&rio, request, hostname, port, path);
    printf("Forward request:\n%s\n", request);
    Rio_writen(serverfd, request, strlen(request));

    Rio_readinitb(&server_rio, serverfd);
    ssize_t n;
    while((n = Rio_readnb(&server_rio, buf, MAXLINE)) > 0)
        Rio_writen(connfd, buf, n);

    Close(serverfd);
}

void *thread(void *vargp)
{
    int connfd = *((int *)vargp);
    Pthread_detach(pthread_self());
    Free(vargp);
    doit(connfd);
    Close(connfd);
    return NULL;
}

int main(int argc, char **argv)
{
    int listenfd;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;

    if(argc != 2)
    {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    listenfd = Open_listenfd(argv[1]);

    while(1)
    {
        pthread_t tid;

        clientlen = sizeof(clientaddr);
        int *connfdp = Malloc(sizeof(int));
        *connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Pthread_create(&tid, NULL, thread, connfdp);
    }
    printf("%s", user_agent_hdr);
    return 0;
}
