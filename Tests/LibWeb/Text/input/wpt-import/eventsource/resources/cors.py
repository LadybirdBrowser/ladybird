import os
from wptserve import pipes

from wptserve.utils import isomorphic_decode

def run_other(request, response, path):
    #This is a terrible hack
    environ = {u"__file__": path}
    exec(compile(open(path, u"r").read(), path, u'exec'), environ, environ)
    rv = environ[u"main"](request, response)
    return rv

def main(request, response):
    origin = request.GET.first(b"origin", request.headers[b"origin"])
    credentials = request.GET.first(b"credentials", b"true")

    response.headers.update([(b"Access-Control-Allow-Origin", origin),
                             (b"Access-Control-Allow-Credentials", credentials)])

    handler = request.GET.first(b'run')
    if handler in [b"status-reconnect",
                   b"message",
                   b"redirect",
                   b"cache-control"]:
        if handler == b"cache-control":
            response.headers.set(b"Content-Type", b"text/event-stream")
            rv = open(os.path.join(request.doc_root, u"eventsource", u"resources", u"cache-control.event_stream"), u"r").read()
            response.content = rv
            pipes.sub(request, response)
            return
        elif handler == b"redirect":
            return run_other(request, response, os.path.join(request.doc_root, u"common", u"redirect.py"))
        else:
            return run_other(request, response, os.path.join(os.path.dirname(isomorphic_decode(__file__)), isomorphic_decode(handler) + u".py"))
    else:
        return
