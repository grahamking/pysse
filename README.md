WORK IN PROGRESS

Python - start the server:

    import pysse
    fd = pysse.start("127.0.0.1", 1234)

Javacript:

    var source = new EventSource('/sse/');
    source.addEventListener('message', function(event) {
        alert(event.data);
    }, false);

Back in your python:

    import os
    os.write(fd, "Hello Event Source!")     # Yes that's the fd from earlier
