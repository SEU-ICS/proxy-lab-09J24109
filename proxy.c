#include "csapp.h"
#include <pthread.h>
#include <string.h>

/* Recommended max cache and object sizes */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

/* You won't lose style points for including this long line in your code */
static const char *user_agent_hdr = "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";

void *thread(void *vargp);
void doit(int fd);
int parse_uri(char *uri, char *host, char *port, char *path);
void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg);

typedef struct cache_block {
    char *url;
    char *data;
    size_t size;
    struct cache_block *prev;
    struct cache_block *next;
} cache_block_t;

static cache_block_t *cache_head = NULL;
static cache_block_t *cache_tail = NULL;
static size_t cache_current_size = 0;
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

void cache_init(void);
void cache_put(char *url, char *data, size_t size);
char *cache_get(char *url, size_t *size);
void cache_evict(void);
void cache_free_block(cache_block_t *block);

int main(int argc, char **argv) {
    int listenfd, *connfdp;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    Signal(SIGPIPE, SIG_IGN);
    listenfd = Open_listenfd(argv[1]);
    cache_init();

    while (1) {
        clientlen = sizeof(clientaddr);
        connfdp = Malloc(sizeof(int));
        *connfdp = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Pthread_create(&tid, NULL, thread, connfdp);
    }
    return 0;
}

void *thread(void *vargp) {
    int connfd = *((int *)vargp);
    Pthread_detach(pthread_self());
    Free(vargp);
    doit(connfd);
    Close(connfd);
    return NULL;
}

void doit(int fd) {
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
    char uri_copy[MAXLINE];
    char host[MAXLINE], port[MAXLINE], path[MAXLINE];
    rio_t rio_client;
    int serverfd;
    size_t n;

    Rio_readinitb(&rio_client, fd);
    if (Rio_readlineb(&rio_client, buf, MAXLINE) <= 0)
        return;
    sscanf(buf, "%s %s %s", method, uri, version);

    if (strcasecmp(method, "GET") != 0) {
        clienterror(fd, method, "501", "Not Implemented",
                    "Proxy only supports GET method");
        return;
    }

    strcpy(uri_copy, uri);

    if (parse_uri(uri, host, port, path) < 0) {
        clienterror(fd, uri, "400", "Bad Request",
                    "Proxy cannot parse the URI");
        return;
    }

    pthread_mutex_lock(&cache_mutex);
    char *cache_data = cache_get(uri_copy, &n);
    pthread_mutex_unlock(&cache_mutex);
    if (cache_data) {
        if (rio_writen(fd, cache_data, n) < 0) {
            free(cache_data);
            return;
        }
        free(cache_data);
        return;
    }

    serverfd = open_clientfd(host, port);
    if (serverfd < 0) {
        clienterror(fd, host, "503", "Service Unavailable",
                    "Cannot connect to remote server");
        return;
    }

    char request[MAXLINE * 4];
    sprintf(request, "GET %s HTTP/1.0\r\n", path);
    sprintf(request + strlen(request), "Host: %s:%s\r\n", host, port);
    strcat(request, user_agent_hdr);
    strcat(request, "Connection: close\r\n\r\n");

    if (rio_writen(serverfd, request, strlen(request)) < 0) {
        Close(serverfd);
        return;
    }

    char *response = NULL;
    size_t total = 0;
    char buf2[MAXBUF];
    ssize_t bytes;
    while ((bytes = read(serverfd, buf2, MAXBUF)) > 0) {
        if (rio_writen(fd, buf2, bytes) < 0)
            break;
        if (total + bytes <= MAX_OBJECT_SIZE) {
            char *tmp = realloc(response, total + bytes);
            if (tmp) {
                response = tmp;
                memcpy(response + total, buf2, bytes);
                total += bytes;
            } else {
                free(response);
                response = NULL;
                total = 0;
                break;
            }
        } else {
            free(response);
            response = NULL;
            total = 0;
            break;
        }
    }

    if (response && total > 0) {
        pthread_mutex_lock(&cache_mutex);
        cache_put(uri_copy, response, total);
        pthread_mutex_unlock(&cache_mutex);
        free(response);
    }

    Close(serverfd);
}

int parse_uri(char *uri, char *host, char *port, char *path) {
    char *host_start, *path_start, *colon;

    if (strncasecmp(uri, "http://", 7) != 0)
        return -1;
    host_start = uri + 7;

    path_start = strchr(host_start, '/');
    if (path_start) {
        strcpy(path, path_start);
        *path_start = '\0';
    } else {
        strcpy(path, "/");
    }

    colon = strchr(host_start, ':');
    if (colon) {
        *colon = '\0';
        strcpy(host, host_start);
        strcpy(port, colon + 1);
    } else {
        strcpy(host, host_start);
        strcpy(port, "80");
    }
    return 0;
}

void clienterror(int fd, char *cause, char *errnum, char *shortmsg, char *longmsg) {
    char buf[MAXLINE];
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n\r\n");
    rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<html><title>Proxy Error</title>");
    rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<body><h1>%s: %s</h1>", errnum, shortmsg);
    rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "<p>%s: %s</p>", longmsg, cause);
    rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "</body></html>");
    rio_writen(fd, buf, strlen(buf));
}

void cache_init(void) {
    cache_head = cache_tail = NULL;
    cache_current_size = 0;
}

void cache_free_block(cache_block_t *block) {
    if (block) {
        free(block->url);
        free(block->data);
        free(block);
    }
}

void cache_evict(void) {
    if (!cache_tail) return;
    cache_block_t *victim = cache_tail;
    if (victim->prev) {
        victim->prev->next = NULL;
        cache_tail = victim->prev;
    } else {
        cache_head = cache_tail = NULL;
    }
    cache_current_size -= victim->size;
    cache_free_block(victim);
}

void cache_put(char *url, char *data, size_t size) {
    if (size > MAX_OBJECT_SIZE) return;

    cache_block_t *p = cache_head;
    while (p) {
        if (strcmp(p->url, url) == 0) {
            if (p->prev) p->prev->next = p->next;
            else cache_head = p->next;
            if (p->next) p->next->prev = p->prev;
            else cache_tail = p->prev;
            cache_current_size -= p->size;
            cache_free_block(p);
            break;
        }
        p = p->next;
    }

    while (cache_current_size + size > MAX_CACHE_SIZE)
        cache_evict();

    if (cache_current_size + size > MAX_CACHE_SIZE)
        return;

    cache_block_t *new_block = malloc(sizeof(cache_block_t));
    if (!new_block) return;
    new_block->url = strdup(url);
    new_block->data = malloc(size);
    if (!new_block->url || !new_block->data) {
        free(new_block->url);
        free(new_block->data);
        free(new_block);
        return;
    }
    memcpy(new_block->data, data, size);
    new_block->size = size;
    new_block->prev = NULL;
    new_block->next = cache_head;

    if (cache_head) cache_head->prev = new_block;
    else cache_tail = new_block;
    cache_head = new_block;
    cache_current_size += size;
}

char *cache_get(char *url, size_t *size) {
    cache_block_t *p = cache_head;
    while (p) {
        if (strcmp(p->url, url) == 0) {
            if (p != cache_head) {
                if (p->prev) p->prev->next = p->next;
                if (p->next) p->next->prev = p->prev;
                else cache_tail = p->prev;
                p->prev = NULL;
                p->next = cache_head;
                if (cache_head) cache_head->prev = p;
                cache_head = p;
                if (!cache_tail) cache_tail = p;
            }
            *size = p->size;
            char *copy = malloc(p->size);
            if (copy) memcpy(copy, p->data, p->size);
            return copy;
        }
        p = p->next;
    }
    return NULL;
}
