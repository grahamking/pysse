""" Test / Example usage of pysse

Usage: ./example.py [url]
    Without a url, it sends a message every second to all subscribers
    With a url (./example.py /test) it sends the message just to clients
    connected to that url.

Open index.html (same directory) in Firefox. Make sure to use a real web
server, not a file:// url.
"""

import os
import time
import sys

import pysse

fd = pysse.start("127.0.0.1", 1234)
print(fd)

f = os.fdopen(fd, "wt")

url = sys.argv[1] if len(sys.argv) > 1 else None

i = 1
while 1:

    if url:
        print("Sending {} to {}".format(i, url))

        # Single line to specific url subscribers.
        # Prefix line with url. Must start with slash.
        f.write("{} data: Test url {}\n\n".format(url, i))
        f.flush()

    else:
        # Single line to all subscribers
        f.write("data: Global {}\n\n".format(i))
        f.flush()

    i += 1

    time.sleep(1)

# Messages end with two CR
#f.write("event: 42\n")
#f.write("data: line one\n")
#f.write("data: line two\n\n")
