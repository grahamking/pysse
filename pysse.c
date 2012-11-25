
/*
 * Copyright 2012 Graham King <graham@gkgk.org>
 *
 * What should license be: LGPL? BSD?
 * It's a library.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>

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

struct client {
    int fd;
    struct client *next;
};

// Connected clients
struct client *head;

// The message we're writing to clients, and it's length
char *outm;
int outm_len;

// Add a socket file descriptor to client list
void client_add(int fd) {

    struct client *new;
    struct client *curr = head;

    new = malloc(sizeof(struct client));
    new->fd = fd;
    new->next  = NULL;

    if (head == NULL) {
        head = new;

    } else {

        while( curr->next != NULL ) {
            curr = curr->next;
        }
        curr->next = new;
    }
}

// Remove a socket file descriptor from client list
// Returns 0 if removed, -1 if not found
int client_remove(int fd) {

    struct client *prev = head;
    struct client *curr;

    // 0 - empty list
    if (head == NULL) {
#ifdef DEBUG
        printf("Attempt to remove fd %d from empty list\n", fd);
#endif
        return -1;
    }

    // 1 - match the head
    if (head->fd == fd) {
        curr = head->next;
        free(head);
        head = curr;
        return 0;
    }

    // n - somewhere in list
    curr = head->next;
    while(curr != NULL && curr->fd != fd) {
        prev = curr;
        curr = curr->next;
    }

    if (curr == NULL) {
#ifdef DEBUG
        printf("fd %d not found in list\n", fd);
#endif
        return -1;
    }

    struct client *next = curr->next;
    free(curr);
    prev->next = next;

    return 0;
}

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

    if ( write(connfd, HEAD_TMPL, strlen(HEAD_TMPL)) == -1 ) {
        perror("write_headers: write error");
        return;
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

    //int flag = 1;
    //setsockopt(connfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(int));

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

    // Assume we can write to socket immediately. Is that a safe assumption?
    write_headers(connfd);

    client_add(connfd);
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

// Set epoll events for the socket. Used to add or remove EPOLLOUT.
void set_epoll(int efd, int connfd, uint32_t events) {

    struct epoll_event ev;
    memset(&ev, 0, sizeof(struct epoll_event));

    ev.events = events;
    ev.data.fd = connfd;
    if (epoll_ctl(efd, EPOLL_CTL_MOD, connfd, &ev) == -1) {
        error(EXIT_FAILURE, errno, "set_epoll: Error %d.", errno);
    }
}

// Read from the pipe and write to all connected sockets
void fanfrom(int efd, int pipefd) {
    char *buf;
    buf = calloc(1, 1024);  // calloc because it sets contents to zero

    int num_read = read(pipefd, buf, 1024);

    if (num_read == -1) {
        perror("fanfrom: Error reading from pipefd");
        return;
    } else if (num_read == 0) {
        // EOF on pipe means parent has quit or wants us to stop
        error(EXIT_FAILURE, errno, "Pipe closed. Quit.");
    }

    outm = buf;
    outm_len = num_read;
    printf("Faning out: %s\n", outm);

    struct client *curr = head;
    while (curr != NULL) {

        set_epoll(efd, curr->fd, EPOLLIN | EPOLLOUT);
        curr = curr->next;
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
            printf("pipefd. Calling fanfrom.\n");
            fanfrom(efd, connfd);

        } else {
            printf("EPOLLIN different fd\n");
            consume(connfd);
        }

    } else if (events & EPOLLOUT) {
#ifdef DEBUG
        printf("EPOLLOUT %d\n", connfd);
#endif

        // Write the outgoing message to this socket
        if ( write(connfd, outm, outm_len) == -1 ) {
            perror("Error write outm to client socket");
        }
        printf("Wrote to %d\n", connfd);

        // Remove that socket from EPOLLOUT list, we are done writing to it
        set_epoll(efd, connfd, EPOLLIN);

    } else if (events & EPOLLHUP) {
#ifdef DEBUG
        printf("EPOLLHUP %d\n", connfd);
#endif
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

#ifdef DEBUG
        sleep(1);   // Slow things down so we can see what's going on
#endif

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

// Check various things are working correctly
int self_test() {
    printf("Self test start\n");

    client_add(1);
    client_add(2);
    client_add(3);

    if (head->next->next->fd != 3) {
        printf("client_add error\n");
        return -1;
    }

    client_remove(2);
    if (head->next->fd != 3) {
        printf("client_remove error\n");
        return -1;
    }
    client_remove(1);
    if (head->fd != 3) {
        printf("client_remove head error\n");
        return -1;
    }

    printf("Self test success\n");
    return 0;
}

char TEST_ADDR[] = "127.0.0.1";
int TEST_PORT = 1234;

int main(int argc, char **argv) {

    if (self_test() == -1) {
        return -1;
    }

    printf("Starting test server on %s. Input is from stdin.\n", TEST_ADDR);
    int fd;
    fd = start(TEST_ADDR, TEST_PORT);

    char buf[256];
    int num_read;
    while ( (num_read = read(STDIN_FILENO, &buf, 256)) != EOF) {
        write(fd, &buf, num_read);
    }

    return 0;
}
