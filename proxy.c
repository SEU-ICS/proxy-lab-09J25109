#include "csapp.h"
#include <stdio.h>
#include <string.h>

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

#define CACHE_BLOCKS 32

typedef struct
{
    char uri[MAXLINE];
    char object[MAX_OBJECT_SIZE];
    int size;
    unsigned long lru;
    int valid;
} cache_block;

typedef struct
{
    cache_block blocks[CACHE_BLOCKS];
    int total_size;
    unsigned long clock;
    pthread_rwlock_t lock;
} cache_t;

cache_t cache;

int find_lru_block()
{
    int victim = -1;
    unsigned long min_lru = 4294967295;

    for(int i = 0; i < CACHE_BLOCKS; i++)
    {
        if(cache.blocks[i].valid && cache.blocks[i].lru < min_lru)
        {
            min_lru = cache.blocks[i].lru;
            victim = i;
        }
    }

    return victim;
}

int find_empty_block(void)
{
    for(int i = 0; i < CACHE_BLOCKS; i++)
    {
        if(!cache.blocks[i].valid)
            return i;
    }
    return -1;
}

void cache_init()
{
    cache.total_size = 0;
    cache.clock = 0;

    pthread_rwlock_init(&cache.lock, NULL);

    for(int i = 0; i < CACHE_BLOCKS; i++)
    {
        cache.blocks[i].valid = 0;
        cache.blocks[i].size = 0;
        cache.blocks[i].lru = 0;
    }
}

void cache_write(const char *uri, const char *object, int size)
{
    if(size > MAX_OBJECT_SIZE)
        return;

    pthread_rwlock_wrlock(&cache.lock);

    while (cache.total_size + size > MAX_CACHE_SIZE)
    {
        int victim = find_lru_block();
        if (victim == -1)
            break;

        cache.total_size -= cache.blocks[victim].size;

        cache.blocks[victim].valid = 0;
        cache.blocks[victim].size = 0;
    }

    int index = find_empty_block();

    if(index == -1)
    {
        index = find_lru_block();
        if(index != -1)
            cache.total_size -= cache.blocks[index].size;
    }

    if (index != -1)
    {
        strcpy(cache.blocks[index].uri, uri);
        memcpy(cache.blocks[index].object, object, size);

        cache.blocks[index].size = size;
        cache.blocks[index].lru = ++cache.clock;
        cache.blocks[index].valid = 1;
        cache.total_size += size;
    }

    pthread_rwlock_unlock(&cache.lock);
}

int cache_read(const char *uri, int connfd)
{
    pthread_rwlock_rdlock(&cache.lock);

    for(int i = 0; i < CACHE_BLOCKS; i++)
    {
        if(cache.blocks[i].valid && !strcmp(cache.blocks[i].uri, uri))
        {
            cache.blocks[i].lru = ++cache.clock;
            Rio_writen(connfd, cache.blocks[i].object, cache.blocks[i].size);
            pthread_rwlock_unlock(&cache.lock);
            return 1;
        }
    }

    pthread_rwlock_unlock(&cache.lock);
    return 0;
}

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
    if(strcasecmp(method, "GET") || cache_read(uri, connfd))    return;
    parse_uri(uri, hostname, port, path);


    int serverfd = Open_clientfd(hostname, port);
    if(serverfd < 0)    return;
    char request[MAXLINE * 10];
    build_headers(&rio, request, hostname, port, path);
    printf("Forward request:\n%s\n", request);
    Rio_writen(serverfd, request, strlen(request));

    Rio_readinitb(&server_rio, serverfd);
    ssize_t n;
    
    char object[MAX_OBJECT_SIZE];
    int object_size = 0;
    int cacheable = 1;
    while((n = Rio_readnb(&server_rio, buf, MAXLINE)) > 0)
    {
        Rio_writen(connfd, buf, n);

        if(cacheable)
        {
            if(object_size + n <= MAX_OBJECT_SIZE)
            {
                memcpy(object + object_size, buf, n);
                object_size += n;
            } 
            else
            {
                cacheable = 0;
            }
        }
    }
    if(cacheable)
        cache_write(uri, object, object_size);
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
    int listenfd ;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;

    if(argc != 2)
    {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    cache_init();
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
