"""EventSource / Server Sent Events test client.
Creates a bunch of clients and counts the messages they receive.
"""

import os
import sys
import select
import socket
import time

USAGE = "Usage: {} num_clients num_expected_messages"

HEADERS = """
GET / HTTP/1.1
Host: localhost:1234
User-Agent: Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:19.0) Gecko/20100101 Firefox/19.0
Accept: text/event-stream
Accept-Language: en-US,en;q=0.5
Accept-Encoding: gzip, deflate
Referer: http://localhost/pysse.html
Origin: http://localhost
Connection: keep-alive
Pragma: no-cache
Cache-Control: no-cache

"""


def start_clients(ep, host, port, num_clients):
    """Start several EventSource clients."""
    conns = {}
    for num in range(num_clients):

        conn = socket.create_connection((host, port))
        conn.setblocking(0)
        conn.sendall(HEADERS)

        conns[conn.fileno()] = conn
        ep.register(conn.fileno(), select.EPOLLIN)

    return conns


def get_events(ep):
    """Generator returning stream of epoll events.
    """

    while 1:
        for event in ep.poll(maxevents=100):
            yield event


def count_messages(ep, num_expected_messages):
    """Count the number of EventSource messages received.
    """

    num_msgs = 0
    start_time = None

    for event in get_events(ep):
        time.sleep(0.5)
        print("--- {}. {}.".format(event, num_msgs))

        efd, _ = event
        inp = os.read(efd, 1024)
        if not inp:
            continue

        print(inp)
        if not inp.startswith("data: "):
            print("Not sse")
            continue

        while inp:

            lines = inp.split("\n\n")
            for ev in lines:
                if not ev:
                    break
                print("Line: {}".format(ev))
                num_msgs += 1

                if num_msgs == 1:                       # First
                    start_time = time.time()

                if num_msgs == num_expected_messages:   # Last
                    return start_time

            try:
                inp = os.read(efd, 1024)
            except OSError:
                # Probably EAGAIN, which is normal for non-blocking
                break # out of  while inp

    return None     # Error


def main():
    """Start here"""

    if len(sys.argv) != 3:
        print(USAGE.format(sys.argv[0]))
        return -1

    num_clients = int(sys.argv[1])
    num_expected_messages = int(sys.argv[2])
    host = "127.0.0.1"
    port = 1234

    ep = select.epoll()
    conns = start_clients(ep, host, port, num_clients)

    start_time = None

    try:

        start_time = count_messages(ep, num_expected_messages)

    except KeyboardInterrupt:
        pass

    elapsed = time.time() - start_time
    print("Elapsed time: %d seconds" % elapsed)

    return 0


if __name__ == "__main__":
    sys.exit(main())
