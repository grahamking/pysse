
/*
 * Copyright 2012 Graham King <graham@gkgk.org>
 *
 * What should license be: LGPL? BSD?
 * It's a library.
 */

#include <Python.h>

#include <errno.h>
#include <error.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/epoll.h>

#include <sys/prctl.h>

#define DEBUG     // Comment in for verbose output

#define HEAD_TMPL "HTTP/1.1 200 OK\nCache-Control: no-cache\nContent-Type: text/event-stream\n\n"

// Number of connected server-sent events sockets
int num_clients = 0;

// File descriptors array, for connected client sockets
int clients[100];

/* Convert domain name to IP address, if needed */
const char *as_numeric(const char *address) {

    if ('0' <= address[0] && address[0] <= '9') {
        // Already numeric
        return address;
    }

    struct addrinfo hints;
    struct addrinfo *result;

    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_UNSPEC;    /* Allow IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;    /* For wildcard IP address */
    hints.ai_protocol = 0;          /* Any protocol */

    int err = getaddrinfo(address, NULL, &hints, &result);
    if (err < 0) {
        printf("getaddrinfo: %s\n", gai_strerror(err));
        error(EXIT_FAILURE, 0, "Error converting domain name to IP address\n");
    }

    // Result can be several addrinfo records, we use the first
    struct sockaddr_in* saddr = (struct sockaddr_in*)result->ai_addr;
    const char *ip_address = inet_ntoa(saddr->sin_addr);

    freeaddrinfo(result);

    return ip_address;
}

/* Open the socket and listen on it. Returns the sockets fd. */
int start_sock(const char *address, int port) {

    struct in_addr iaddr;
    struct sockaddr_in saddr;

    int sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sockfd == -1) {
        error(EXIT_FAILURE, errno, "Error %d creating socket", errno);
    }

    int optval = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) == -1) {
        error(EXIT_FAILURE, errno, "Error %d setting SO_REUSEADDR on socket", errno);
    }

    address = as_numeric(address);
    printf("Listening on: %s:%d\n", address, port);

    memset(&iaddr, 0, sizeof(struct in_addr));
    int err = inet_pton(AF_INET, address, &iaddr);
    if (err != 1) {
        error(EXIT_FAILURE, errno, "Error %d converting address to network format", errno);
    }

    memset(&saddr, 0, sizeof(struct sockaddr_in));
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(port);
    saddr.sin_addr = iaddr;

    err = bind(sockfd, (struct sockaddr *) &saddr, sizeof(struct sockaddr_in));
    if (err == -1) {
        error(EXIT_FAILURE, errno, "Error %d binding socket", errno);
    }

    err = listen(sockfd, SOMAXCONN);
    if (err == -1) {
        perror("start_sock: Error listening on sockfd");
    }

    return sockfd;
}

/* Create epoll fd and add sockfd to it. Returns epoll fd. */
int start_epoll(int sockfd, int pipefd) {

    int efd = epoll_create(1);
    if (efd == -1) {
        error(EXIT_FAILURE, errno, "Error %d creating epoll descriptor", errno);
    }

    struct epoll_event ev1, ev2;
    memset(&ev1, 0, sizeof(struct epoll_event));
    memset(&ev2, 0, sizeof(struct epoll_event));

    ev1.events = EPOLLIN;
    ev1.data.fd = sockfd;

    // Add the socket, to accept new connections
    int err = epoll_ctl(efd, EPOLL_CTL_ADD, sockfd, &ev1);
    if (err == -1) {
        error(EXIT_FAILURE, errno, "Error %d adding sockdfd to epoll descriptor", errno);
    }

    ev2.events = EPOLLIN;
    ev2.data.fd = pipefd;

    // Add the pipe, to get writes from caller
    err = epoll_ctl(efd, EPOLL_CTL_ADD, pipefd, &ev2);
    if (err == -1) {
        error(EXIT_FAILURE, errno, "Error %d adding pipefd to epoll descriptor", errno);
    }

    return efd;
}

// Write the HTTP response headers
void write_headers(int connfd) {
    int sent = write(connfd, HEAD_TMPL, strlen(HEAD_TMPL));

    if (sent == -1) {
        perror("write_headers");
    }

#ifdef DEBUG
    printf("Wrote headers to %d\n", connfd);
#endif
}

/* Accept a new connection on sockfd, and add it to epoll.
 *
 * We re-used the epoll_event to save allocating a new one each time on
 * the stack. I _think_ that's a good idea.
 *
 * Returns -1 if error.
 */
int acceptnew(int sockfd, int efd, struct epoll_event *evp) {

    int connfd = accept4(sockfd, NULL, NULL, SOCK_NONBLOCK);
    if (connfd == -1) {
        if (errno == EAGAIN) {
            // If we were multi-process, we'd get this error if another
            // worker process got there before us - no problem
            return 0;
        } else {
            perror("acceptnew: Error 'accept' on socket");
            return -1;
        }
    }

    /*
    if (connfd >= offsetsz) {
        grow_offset();
    }
    */

#ifdef DEBUG
    printf("Accepted: %d\n",  connfd);
#endif

    evp->events = EPOLLIN; // | EPOLLOUT; We only need EPOLLOUT when msg to write
    evp->data.fd = connfd;
    if (epoll_ctl(efd, EPOLL_CTL_ADD, connfd, evp) == -1) {
        error(EXIT_FAILURE, errno, "Error %d adding to epoll descriptor", errno);
    }

    write_headers(connfd);

    clients[num_clients++] = connfd;  // clients and num_clients are global

    return 0;
}

// Read from fd and write to stdout - for dev
void consume(int connfd) {

    char buf[1024];
    int num_read = read(connfd, &buf, 1024);

    if (num_read == -1) {
        perror("consume: Error reading from connfd");

    } else if (num_read == 0) { // EOF
        printf("EOF\n");
        close(connfd);

    } else {
        printf("%s\n", buf);
    }

}

// Read from the pipe and write to all connected sockets
void fanfrom(int pipefd) {

    char buf[1024];
    int num_read = read(pipefd, &buf, 1024);

    if (num_read == -1) {
        perror("fanfrom: Error reading from pipefd");
        return;
    } else if (num_read == 0) {
        // EOF on pipe means parent has quit or wants us to stop
        error(EXIT_FAILURE, errno, "Pipe closed. Quit.");
    }

    printf("Faning out: %s\n", buf);

    for (int i=0; i < num_clients; i++) {
        if ( write(clients[i], buf, num_read) == -1 ) {
            perror("fanfrom: Error write to client socket");
        }
    }
}

/* Process a single epoll event */
void do_event(struct epoll_event *evp, int sockfd, int efd, int pipefd) {

    int connfd = evp->data.fd;
    //int done = 0;        // Are we done writing?
    uint32_t events = evp->events;

    if (events & EPOLLIN) {
#ifdef DEBUG
        printf("EPOLLIN %d\n", connfd);
#endif
        if (connfd == sockfd) {
            printf("sockfd\n");
            acceptnew(sockfd, efd, evp);

        } else if (connfd == pipefd) {
            printf("pipefd\n");
            fanfrom(connfd);

        } else {
            printf("EPOLLIN different fd\n");
            consume(connfd);

            // New connections need http response header

        }

    } else if (events & EPOLLOUT) {
#ifdef DEBUG
        printf("EPOLLOUT %d\n", connfd);
#endif

        /*
        done = swrite_sendfile(connfd, datafd, datasz);

        if (done == 1) {
            shut(connfd, efd);
        }
        */

    } else if (events & EPOLLHUP) {
#ifdef DEBUG
        printf("EPOLLHUP %d\n", connfd);
#endif
        //sclose(connfd);
    }
}

// Listen for new connection or write on pipe fd, or capacity to writeon
// a connected fd if we're outputting a message. Does not return.
void main_loop(int efd, int sockfd, int pipefd) {

    int i;
    int num_ready;
    struct epoll_event events[100];

    printf("main_loop. sockfd: %d, pipefd: %d\n", sockfd, pipefd);
    while (1) {

        num_ready = epoll_wait(efd, events, 100, -1);
        if (num_ready == -1) {
            error(EXIT_FAILURE, errno, "Error %d on epoll_wait", errno);
        }

        for (i = 0; i < num_ready; i++) {
            do_event(&events[i], sockfd, efd, pipefd);
        }

        sleep(1);   // for development
    }
}

// Start the server-sent events server on given address
int start(const char *address, int port) {

    int pipefds[2];
    if ( pipe(pipefds) == -1 ) {
        error(EXIT_FAILURE, errno, "Error %d creating pipe", errno);
    }

    // Fork so caller can continue, child process becomes a server

    int fork_fd = fork();
    if (fork_fd != 0) {
        // We're the parent - close read end and return write end of the pipe
        close(pipefds[0]);
        return pipefds[1];
    }

    // We're the child - we never return

    // Ask to be stopped when parent process stops
    prctl(PR_SET_PDEATHSIG, SIGHUP);

    close(pipefds[1]);  // Close write end of the pipe - we only read

    int sockfd = start_sock(address, port);

    int efd = start_epoll(sockfd, pipefds[0]);

    main_loop(efd, sockfd, pipefds[0]);

    if (close(pipefds[0]) == -1) {
        error(EXIT_FAILURE, errno, "Error %d closing pipe fd", errno);
    }
    if (close(sockfd) == -1) {
        error(EXIT_FAILURE, errno, "Error %d closing socket fd", errno);
    }

    return 0;
}

/*
 * Python extension wrapper
 */

static PyObject *pysse_start(PyObject *self, PyObject *args) {

    const char *address;
    int port;

    printf("Start\n");

    if (!PyArg_ParseTuple(args, "si", &address, &port)) {
        return NULL;
    }

    int pipefd = start(address, port);

    return Py_BuildValue("i", pipefd);
}

static PyMethodDef HelloMethods[] = {
    {"start", pysse_start, METH_VARARGS, "Start server-sent event server on given port"},
    {NULL, NULL, 0, NULL}
};

PyMODINIT_FUNC
initpysse(void) {
    (void) Py_InitModule("pysse", HelloMethods);
}
