import os
from wptserve.utils import isomorphic_encode
from json import dumps, loads

def main(request, response):
    key = request.GET.first(b"key").decode("utf8")
    mode = request.GET.first(b"mode", "content")
    status = int(request.GET.first(b"status", b"200"))
    stash = request.server.stash
    cors = request.GET.first(b"cors", "true")
    if cors == "true" or mode == b"info":
        response.headers.set(b"Access-Control-Allow-Origin", b"*")

    response.status = status
    with stash.lock:
        requests = loads(stash.take(key) or '[]')
        if mode == b"info":
            response.headers.set(b"Content-Type", "application/json")
            json_reqs = dumps(requests)
            response.content = json_reqs
            stash.put(key, json_reqs)
            return
        else:
            headers = {}
            for header, value in request.headers.items():
                headers[header.decode("utf8")] = value[0].decode("utf8")
            path = request.url
            requests.append({"headers": headers, "url": request.url})
            stash.put(key, dumps(requests))

    response.headers.set(b"Content-Type", request.GET.first(b"type", "text/plain"))
    response.headers.set(b"Cache-Control", request.GET.first(b"cache-control", b"max-age: 604800"))
    if b"file" in request.GET:
        path = os.path.join(os.path.dirname(isomorphic_encode(__file__)), os.path.basename(request.GET.first(b"file")))
        response.content = open(path, mode=u'rb').read()
    else:
        return request.GET.first(b"content", "123")
