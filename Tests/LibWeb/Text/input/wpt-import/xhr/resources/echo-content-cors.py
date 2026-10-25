def main(request, response):
    headers = [(b"X-Request-Method", request.method),
               (b"X-Request-Content-Length", request.headers.get(b"Content-Length", b"NO")),
               (b"X-Request-Content-Type", request.headers.get(b"Content-Type", b"NO")),
               (b"Access-Control-Allow-Credentials", b"true"),
               # Avoid any kind of content sniffing on the response.
               (b"Content-Type", b"text/plain")]

    origin = request.GET.first(b"origin", request.headers.get(b"origin"))
    if origin != None:
        headers.append((b"Access-Control-Allow-Origin", origin))

    request_headers = request.GET.first(b"origin", request.headers.get(b"access-control-request-headers"))
    if request_headers != None:
        headers.append((b"Access-Control-Allow-Headers", request_headers))

    request_method = request.GET.first(b"origin", request.headers.get(b"access-control-request-method"))
    if request_method != None:
        headers.append((b"Access-Control-Allow-Methods", b"OPTIONS, " + request_method))

    content = request.body

    return headers, content
