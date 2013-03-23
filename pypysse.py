
import socket
import select
import os
import sys
import redis
import threading
import fcntl

import time

HEAD_TMPL = "HTTP/1.1 200 OK\nCache-Control: no-cache\nContent-Type: text/event-stream\nConnection: keep-alive\nAccess-Control-Allow-Origin: *\n\n"

REDIS_QUEUE = "pysse"


def start_redis_thread(list_name, p_write):
    """
        Start redis in a separate thread, copying input from redis
        onto pipe fd.

        list_name: Redis list to take commands from
        p_write: File descriptor (pipe) for writing redis messages to.
    """

    r = redis.StrictRedis(host="127.0.0.1")

    def inner():
        while 1:
            _, val = r.blpop([list_name])
            os.write(p_write, val)

    t = threading.Thread(target=inner, name='pysse redis')
    t.daemon = 1
    t.start()


class Server(object):
    """EventSource server.
    """

    def __init__(self):

        self.sock = self.start_sock()
        self.ep = self.start_epoll()

        self.p_read = self.pipe_from_redis()

        self.conns = {}                 # Active connections
        self.out_message = None         # Message to send out

    def start_sock(self):

        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.setblocking(0)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind(("127.0.0.1", 1234))
        sock.listen(5)
        return sock

    def start_epoll(self):

        ep = select.epoll()
        ep.register(self.sock.fileno(), select.EPOLLIN)
        return ep

    def pipe_from_redis(self):
        """Create a pipe and connect to a thread which reads from
        a Redis queue.
        """
        p_read, p_write = os.pipe()

        # Set p_read to non blocking
        fl = fcntl.fcntl(p_read, fcntl.F_GETFL)
        fcntl.fcntl(p_read, fcntl.F_SETFL, fl | os.O_NONBLOCK)

        self.ep.register(p_read, select.EPOLLIN)

        start_redis_thread(REDIS_QUEUE, p_write)

        return p_read

    def acceptnew(self):
        """Accept a new client connection.
        """

        conn, _ = self.sock.accept()
        #print("Accepted: %d" % conn.fileno())
        conn.setblocking(0)

        self.ep.register(conn.fileno(), select.EPOLLIN)
        conn.send(HEAD_TMPL)

        self.conns[conn.fileno()] = conn

    def get_redis_command(self):
        """Read command from redis pipe.
        """
        new_command = ""

        msg = os.read(self.p_read, 1024)
        while msg:
            new_command += msg
            try:
                msg = os.read(self.p_read, 1024)
            except OSError: # EAGAIN, we're done reading
                break

        self.out_message =  new_command

        sys.stdout.write(self.out_message)
        sys.stdout.flush()

    def listen_for_out(self):
        """Listen for EPOLLOUT readiness on all the connections
        in conn_fds.
        ep: epoll object.
        """
        for fd in self.conns.keys():
            #print("listen_for_out: %d" % fd)
            try:
                self.ep.modify(fd, select.EPOLLIN | select.EPOLLOUT)
            except IOError:
                print("IOError ep.modify on %d" % fd)

    def event_generator(self):
        """Yield epoll events, forever. Blocks if no events.
        """
        while 1:
            for event in self.ep.poll(maxevents=100):
                yield event
            time.sleep(0.5)

    def close(self, fd):
        """Close a file descriptor, removing it from internal list.
        """
        self.conns[fd].close()
        del self.conns[fd]
        print("Closed %d" % fd)

    def run(self):
        """main_loop. Runs forever"""

        sockfd = self.sock.fileno()

        for event in self.event_generator():
            print("----- {}".format(event))

            efd, etype = event
            if etype & select.EPOLLHUP:
                print("EPOLLHUP: %d" % efd)
                self.close(efd)
                continue    # Ignore all other events

            if etype & select.EPOLLIN:
                print("EPOLLIN: %d" % efd)

                if efd == sockfd:           # New client connection

                    self.acceptnew()

                elif efd == self.p_read:    # New command from Redis

                    self.get_redis_command()
                    self.listen_for_out()

                else:                       # From a client socket

                    try:
                        inp = os.read(efd, 1024)
                    except OSError:
                        print("OSError, closing %d" % efd)
                        self.close(efd)
                        continue

                    if not inp: # EOF
                        self.close(efd)
                        continue

                    else:
                        print(inp)

            if etype & select.EPOLLOUT:
                print("EPOLLOUT: %d" % efd)

                if not self.out_message:
                    continue

                num_wrote = os.write(efd, self.out_message)
                print("Wrote: ---")
                print(self.out_message)
                print("Wrote %d bytes of %d" % (num_wrote, len(self.out_message)))
                self.ep.modify(efd, select.EPOLLIN)

            #print("Open conns: {}".format(self.conns.keys()))


def main():

    server = Server()
    server.run()

    return 0


if __name__ == "__main__":
    sys.exit(main())
