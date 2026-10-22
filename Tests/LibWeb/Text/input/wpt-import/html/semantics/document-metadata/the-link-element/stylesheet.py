def main(request, response):
    try:
        count = int(request.server.stash.take(request.GET[b"id"]))
    except:
        count = 0

    if b"count" in request.GET:
        return str(count)
    request.server.stash.put(request.GET[b"id"], str(count + 1))
    headers = [(b"Content-Type", b"text/css")]
    return 200, headers, u'body {color: red;}'
