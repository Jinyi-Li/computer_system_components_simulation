#include "csapp.h"
#include <stdbool.h>

#define DEBUG

#ifdef DEBUG
/* When debugging is enabled, the underlying functions get called */
#define dbg_printf(...) printf(__VA_ARGS__)
#else
/* When debugging is disabled, no code gets generated */
#define dbg_printf(...)
#endif

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


/*
 * Process the client request in buf by parsing it into method, host,
 * port, resource, version. It will take pointers to save the parsing result.
 *
 * It will return 0 if succeeds, or 1 if fails.
 */
int process_request(char *buf, char *socket_address, char *host, char *port,
                    char *method, char *resource) {
    char uri[MAXLINE], version;

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
    has_connection = false;
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
 * Process response from server line by line.
 *
 * Return 0 if succeeds, or 1 if fails.
 */
int process_response(rio_t pxyrio, int pxyfd, int clientfd, char *pxybuf) {
    // Initialize reading from proxy-server socket.
    Rio_readinitb(&pxyrio, pxyfd);
    dbg_printf("Response: \n");
    ssize_t size;

    // Read from server, write to client.
    while ((size = rio_readlineb(&pxyrio, pxybuf, MAXLINE)) > 0) {
        if ((size_t) rio_writen(clientfd, pxybuf, size) != size) {
            return 1;
        }
        dbg_printf("%s", pxybuf);
    }
    return 0;
}

/*
 * Serve the client with a HTTP response.
 *
 * If error occurs during reading or writing, the function will return.
 */
void serve(client_info *client) {
    int proxyfd;
    char buf[MAXLINE], socket_address[MAXLINE], host[MAXLINE], port[MAXLINE],
            method[MAXLINE],
            resource[MAXLINE];

    // Get some extra info about the client (hostname/port)
    Getnameinfo((SA * ) & client->addr, client->addrlen,
                client->host, sizeof(client->host),
                client->serv, sizeof(client->serv),
                0);
    dbg_printf("Accepted connection from %s:%s\n", client->host, client->serv);

    // Initialize client-proxy file descriptor.
    rio_t rio;
    Rio_readinitb(&rio, client->connfd);

    // Process request: step 1, read from client, validate.
    ssize_t rc;
    if ((rc = rio_readlineb(&rio, buf, MAXLINE)) < 0) {
        return;
    }
    if (process_request(buf, socket_address, host, port, method, resource)) {
        return;
    }

    // Open a proxy-server connection.
    dbg_printf("***host: %s port: %s\n", host, port);
    if ((proxyfd = open_clientfd(host, port)) < 0) {
        return;
    }

    // Process request: step 2, write to server.
    if ((size_t) rio_writen(proxyfd, buf, strlen(buf)) != strlen(buf)) {
        return;
    }
    dbg_printf("%s", buf);

    // Process headers: read from client, write to server.
    if (process_headers(rio, proxyfd, socket_address, buf)) {
        return;
    }

    // Process response: read from server, write to client.
    rio_t pxyrio;
    char pxybuf[MAXLINE];
    if (process_response(pxyrio, proxyfd, client->connfd, pxybuf)) {
        return;
    }

    // Close proxy-server socket.
    int close_rc;
    if ((close_rc = close(proxyfd)) < 0) {
        return;
    }
}


/* Main routine of the proxy server. */
int main(int argc, char **argv) {
    // Block SIGPIPE signal.
    sigset_t mask, prev;
    Sigemptyset(&mask);
    Sigaddset(&mask, SIGPIPE);
    Sigprocmask(SIG_BLOCK, &mask, &prev);

    int listenfd;

    // Get port number from the command line argument.
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        exit(1);
    }

    // Create server socket - listen to port and accept client sockets.
    char *clientport = argv[1];
    listenfd = Open_listenfd(clientport);
    while (1) {
        // Save client info on stack.
        client_info client_data;
        client_info *client = &client_data;

        // Initialize client address.
        client->addrlen = sizeof(client->addr);

        // Get accepted client socket's file descriptor.
        client->connfd = Accept(listenfd,
                                (SA * ) & client->addr, &client->addrlen);

        // Connect with a client and serve.
        serve(client);

        // Close connection and release the port for new requests to come.
        Close(client->connfd);
    }
    // Never reach here.
    Sigprocmask(SIG_SETMASK, &prev, NULL);
}
