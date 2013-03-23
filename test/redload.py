"""Push lots of EventSource messages into a redis queue.
USAGE: redload.py num_messages_to_push
"""

import redis
import random
import sys

QUEUE = "pysse"

num_msgs = int(sys.argv[1])

client = redis.StrictRedis(host="127.0.0.1")
for i in range(num_msgs):

    # Random 16 byte string of printable characters
    msg = "data: %s\n\n" % "".join(chr(random.randint(0x30,0x5B)) for _ in range(16))

    client.lpush(QUEUE, msg)
