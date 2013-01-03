WORK IN PROGRESS

A [Server-Sent Events](http://www.w3.org/TR/eventsource/) server in a python extension.

The server itself is in C, single threaded and async (epoll), so should be able to handle thousands of concurrent connections. The wrapper makes using it from Python very easy.

## Install

It's a python extension, with no dependencies:

    sudo python setup.py install

## Test

Copy `test/index.html` to your web server's directory. If using nginx on ubuntu that's `/usr/share/nginx/www/`. If using Apache I think it's `/var/www/`.

In a terminal:

    python test/example.py

In a browser, open the `index.html` you copied earlier, probably by browsing to `http://localhost/`. Don't use a file:// url.

## Some details

Look at example.py and index.html.

Python - start the server:

    import pysse
    fd = pysse.start("127.0.0.1", 1234)

Javacript:

    var source = new EventSource('http://127.0.0.1:1234/');
    source.addEventListener('message', function(event) {
        alert(event.data);
    }, false);

Back in your python:

    import os
    # Event must end with two CR
    os.write(fd, "data: Hello Event Source!\n\n") # Yes, the fd from earlier

Suggested usage:

Your (web)app puts whatever it wants to send on a redis list. The python daemon (daemonized via `upstart`) watches the list, and sends that to the pysse pipe.

    while 1:
        val = redis.blpop('queue')
        os.write(fd, "data: {}\n\n".format(val))

## Not done yet

Authentication
Actually use it in production
