/*
 * proxy.c
 *
 * Proxy handles requests from clients.
 *
 * If the request has been cached down in the proxy's memory, it'll return
 * the cached web object content without querying the real server.
 *
 * It the request's not been cached yet, the proxy will send the request to
 * the real server, after making necessary changes to the headers. After
 * getting response, the response will go to the client.
 *
 * Meanwhile, if the response object is small enough to be held in cache,
 * it'll be cached.
 *
 * Author: Jinyi Li
 */
#include "csapp.h"
#include "cache.h"
#include <stdbool.h>

//#define DEBUG

#ifdef DEBUG
/* When debugging is enabled, the underlying functions get called */
#define dbg_printf(...) printf(__VA_ARGS__)
#else
/* When debugging is disabled, no code gets generated */
#define dbg_printf(...)
#endif

/* Max web object size */
#define MAX_OBJECT_SIZE (100*1024)
/* Max text line length */
#define MAXLINE 8192
/* Length of host address */
#define HOSTLEN 256
/* Length of port number */
#define SERVLEN 8

/* Information about a connected client. */
typedef struct {
    struct sockaddr_in addr;    // Socket address
    socklen_t addrlen;          // Socket address length
    int connfd;                 // Client connection file descriptor
    char host[HOSTLEN];         // Client host
    char serv[SERVLEN];         // Client service (port)
} client_info;

/* User-Agent header string. */
static const char *header_user_agent = "User-Agent: Mozilla/5.0"
                                       " (X11; Linux x86_64; rv:3.10.0)"
                                       " Gecko/20181101 Firefox/61.0.1\r\n";
/* Connection header string. */
static const char *header_connection = "Connection: close\r\n";
/* Proxy connection header string. */
static const char *header_proxy_connection = "Proxy-Connection: close\r\n";
/* Semaphore that protects the shared cache. */
sem_t mutex;


/*
 * Process the client request in buf by parsing it into method, host,
 * port, resource, version. It will take pointers to save the parsing result.
 *
 * It will return 0 if succeeds, or 1 if fails.
 */
int process_request(char *buf, char *socket_address, char *host, char *port,
                    char *uri, char *method, char *resource) {
    char version;

    // Parse request into: method, URI, version.
    if (sscanf(buf, "%s %s HTTP/1.%c", method, uri, &version) != 3
        || (version != '0' && version != '1')) {
        return 1;
    }

    // Parse URI into: (protocol,) socket address, resource.
    char request_protocol[MAXLINE];
    if (strstr(uri, "://")) {
        // e.g.: "http://www.hi.com:1234/data1.txt"
        sscanf(uri, "%[^:]://%[^/]%s", request_protocol, socket_address,
               resource);
    } else if (strstr(uri, "/")) {
        // e.g.: "www.hi.com:1234/data1.txt"
        sscanf(uri, "%[^/]%s", socket_address, resource);
    } else {
        // Invalid URI.
        return 1;
    }

    // Get host and port.
    if (strstr(socket_address, ":")) {
        sscanf(socket_address, "%[^:]:%[0-9]", host, port);
    } else {
        host = socket_address;
        port = "80";
    }

    // Reformat request.
    strcpy(buf, method);
    strcat(buf, " ");
    strcat(buf, resource);
    strcat(buf, " ");
    strcat(buf, "HTTP/1.0");
    strcat(buf, "\r\n");

    return 0;
}

/*
 * Process request headers line by line. Replace User-Agent, Connection, and
 * Proxy-Connection headers with prepared values, and add Host header if not
 * exists in the original request.
 *
 * Return 0 if succeeds, or 1 if fails.
 */
int process_headers(rio_t rio, int proxyfd, char *socket_address, char *buf) {
    bool has_host, has_useragent, has_connection, has_proxyconnection;
    ssize_t rc;

    do {
        // Read in a new header.
        if ((rc = rio_readlineb(&rio, buf, MAXLINE)) < 0) {
            return 1;
        }

        // Replace some headers with specific values.
        if (strncmp(buf, "Host:", sizeof("Host:"))) {
            // If having Host header, keep it.
            has_host = true;
        } else if (strstr(buf, "User-Agent:")) {
            has_useragent = true;
            strcpy(buf, header_user_agent);
        } else if (strstr(buf, "Proxy-Connection:")) {
            has_proxyconnection = true;
            strcpy(buf, header_proxy_connection);
        } else if (strstr(buf, "Connection:")) {
            has_connection = true;
            strcpy(buf, header_connection);
        }

        // Write this header to server.
        if ((size_t) rio_writen(proxyfd, buf, strlen(buf)) != strlen(buf)) {
            return 1;
        }
        dbg_printf("%s", buf);
    } while (strncmp(buf, "\r\n", sizeof("\r\n")));

    // Add some headers if not exists in the original request.
    if (!has_host) {
        strcpy(buf, "Host: ");
        strcpy(buf, socket_address);
        if ((size_t) rio_writen(proxyfd, buf, strlen(buf)) != strlen(buf)) {
            return 1;
        }
    }
    if (!has_useragent) {
        strcpy(buf, header_user_agent);
        if ((size_t) rio_writen(proxyfd, buf, strlen(buf)) != strlen(buf)) {
            return 1;
        }
    }
    if (!has_connection) {
        strcpy(buf, header_connection);
        if ((size_t) rio_writen(proxyfd, buf, strlen(buf)) != strlen(buf)) {
            return 1;
        }
    }
    if (!has_proxyconnection) {
        strcpy(buf, header_proxy_connection);
        if ((size_t) rio_writen(proxyfd, buf, strlen(buf)) != strlen(buf)) {
            return 1;
        }
    }
    return 0;
}

/*
 * Append content in buf to 'entry_obj' with a current length of obj_len.
 *
 * Return 0 if succeeds, or 1 if fails.
 */
int append_to_cache_obj(char *entry_obj, int *obj_len, int size, char *buf) {
    if ((*obj_len + size) > MAX_OBJECT_SIZE) {
        return 1;
    }
    // Update pointer position where I should continue writing.
    void *write_pos = (void *) (entry_obj + *obj_len);
    memcpy(write_pos, buf, size);
    // Update current response object size.
    *obj_len += size;
    return 0;
}

/*
 * Process response from server line by line.
 *
 * Return 0 if succeeds, or 1 if fails.
 */
int process_response(rio_t pxyrio, int pxyfd, int clientfd, char *pxybuf,
                     char *entry_obj, int *obj_len) {
    // Initialize reading from proxy-server socket.
    Rio_readinitb(&pxyrio, pxyfd);
    ssize_t size;

    // Read from server, write to client.
    bool is_valid_obj = true;
    while ((size = rio_readlineb(&pxyrio, pxybuf, MAXLINE)) > 0) {
        if ((size_t) rio_writen(clientfd, pxybuf, size) != size) {
            return 1;
        }

        // Write to cache object.
        if (is_valid_obj) {
            if (append_to_cache_obj(entry_obj, obj_len, size, pxybuf)) {
                is_valid_obj = false;
            }
        }
    }
    // If failed rio_writen or invalid cache obj, all need to return 1
    // to free all malloc-ed resource and close the proxy-client socket.
    return is_valid_obj ? 0 : 1;
}

/*
 * Free malloc-ed resource and close proxy-client socket.
 */
void free_resource(char *url, char *obj, int proxyfd) {
    Free(url);
    Free(obj);
    Close(proxyfd);
}

/*
 * When no cached data, serve this request to the corresponding server.
 */
void serve_request(char *host, char *port, char *uri, char *socket_address,
                   char *buf, rio_t rio, int connfd) {
    int proxyfd;

    // Initialize for cache entry's url and response object.
    char *entry_url = Malloc(sizeof(char) * (strlen(uri) + 1));
    strcpy(entry_url, uri);
    char *entry_obj = Malloc(MAX_OBJECT_SIZE);
    int obj_len = 0;

    // Open a proxy-server connection.
    if ((proxyfd = open_clientfd(host, port)) < 0) {
        Free(entry_url);
        Free(entry_obj);
        return;
    }
    // Send request.
    if ((size_t) rio_writen(proxyfd, buf, strlen(buf)) != strlen(buf)) {
        free_resource(entry_url, entry_obj, proxyfd);
        return;
    }
    // Send headers.
    if (process_headers(rio, proxyfd, socket_address, buf)) {
        free_resource(entry_url, entry_obj, proxyfd);
        return;
    }
    // Send response: read from server, write to client.
    rio_t pxyrio;
    char pxybuf[MAXLINE];
    int response_res = process_response(pxyrio, proxyfd, connfd, pxybuf,
                                        entry_obj, &obj_len);
    if (response_res == 1) {
        free_resource(entry_url, entry_obj, proxyfd);
        return;
    } else {
        // Save the valid-size response object to cache.
        entry *new_entry = create_entry(entry_url, entry_obj, obj_len);

        // Add lock to protect the cache.
        P(&mutex);
        put_new_entry(new_entry);
        V(&mutex);

        Close(proxyfd);
    }
}

/*
 * When having cached data, serve the client with cached entry.
 */
void serve_cache(entry *cache_entry, int connfd, char *buf, rio_t rio) {
    // Send response: read from server, write to client.
    if ((size_t) rio_writen(connfd, cache_entry->response,
                            cache_entry->obj_len) != cache_entry->obj_len) {
        return;
    }
}

/*
 * Serve the client with a HTTP response.
 *
 * If error occurs during reading or writing, the function will return.
 */
void serve(int connfd) {
    char buf[MAXLINE], socket_address[MAXLINE], host[MAXLINE], port[MAXLINE],
            uri[MAXLINE], method[MAXLINE], resource[MAXLINE];

    // Initialize client-proxy file descriptor.
    rio_t rio;
    Rio_readinitb(&rio, connfd);

    // Process request line.
    ssize_t rc;
    if ((rc = rio_readlineb(&rio, buf, MAXLINE)) < 0) {
        return;
    }
    dbg_printf("Request: %s\n", buf);
    if (process_request(buf, socket_address, host,
                        port, uri, method, resource)) {
        return;
    }

    entry *cache_entry = read_entry(uri);
    if (cache_entry) {                          // Serve cached response
        serve_cache(cache_entry, connfd, buf, rio);
    } else {                                    // Serve requested response.
        serve_request(host, port, uri, socket_address, buf, rio, connfd);
    }
}

/*
 * A thread handler to detach, serve, and free the thread to process a request.
 */
void *thread(void *thread_arg) {
    pthread_detach(pthread_self());
    int connfd = *((int *) thread_arg);
    Free(thread_arg);
    serve(connfd);
    Close(connfd);
    return NULL;
}


/* Main routine of the proxy server. */
int main(int argc, char **argv) {
    // Block SIGPIPE signal.
    sigset_t mask, prev;
    Sigemptyset(&mask);
    Sigaddset(&mask, SIGPIPE);
    Sigprocmask(SIG_BLOCK, &mask, &prev);

    int listenfd, *connfdp;
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    // Get port number from the command line argument.
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }

    // Create server socket - listen to port and accept client sockets.
    char *proxyport = argv[1];

    // Initiate the cache.
    init_cache();
    // Initiate the Semaphore.
    Sem_init(&mutex, 0, 1);

    listenfd = Open_listenfd(proxyport);
    while (1) {
        // Save client info on stack.
        clientlen = sizeof(struct sockaddr_storage);
        connfdp = malloc(sizeof(int));
        if (!connfdp) {
            continue;
        }
        *connfdp = Accept(listenfd, (SA * ) & clientaddr, &clientlen);

        // Create a thread to handle this request.
        Pthread_create(&tid, NULL, thread, connfdp);
    }
}
