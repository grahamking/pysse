
import socket
import select
import os
import sys

HEAD_TMPL = "HTTP/1.1 200 OK\nCache-Control: no-cache\nContent-Type: text/event-stream\nConnection: keep-alive\nAccess-Control-Allow-Origin: *\n\n"


def start_sock():

    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setblocking(0)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind(("127.0.0.1", 1234))
    sock.listen(5)
    return sock


def start_epoll(sockfd):

    ep = select.epoll()
    ep.register(sockfd, select.EPOLLIN)
    return ep


def acceptnew(sock, ep):

    conn, _ = sock.accept()
    conn.setblocking(0)
    ep.register(conn.fileno(), select.EPOLLIN)

    conn.send(HEAD_TMPL)

    return conn


def main_loop(sock, ep):

    sockfd = sock.fileno()
    conns = []

    while 1:

        events = ep.poll(maxevents=100)

        for event in events:
            print(event)
            efd, etype = event
            if etype == select.EPOLLIN:

                if efd == sockfd:
                    conns.append(acceptnew(sock, ep))
                else:
                    print("Other fd: %d" % efd)


def main():

    sock = start_sock()
    sockfd = sock.fileno()
    ep = start_epoll(sockfd)
    main_loop(sock, ep)

    return 0


if __name__ == "__main__":
    sys.exit(main())
