
import pysse
import os

fd = pysse.start("127.0.0.1", 1234)
print(fd)

f = os.fdopen(fd, "wt")
f.write("Test")
