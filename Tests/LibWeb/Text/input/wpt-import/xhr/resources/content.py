from wptserve.utils import isomorphic_encode

def main(request, response):
    response_ctype = b''

    if b"response_charset_label" in request.GET:
        response_ctype = b";charset=" + request.GET.first(b"response_charset_label")

    headers = [(b"Content-type", b"text/plain" + response_ctype),
               (b"X-Request-Method", isomorphic_encode(request.method)),
               (b"X-Request-Query", isomorphic_encode(request.url_parts.query) if request.url_parts.query else b"NO"),
               (b"X-Request-Content-Length", request.headers.get(b"Content-Length", b"NO")),
               (b"X-Request-Content-Type", request.headers.get(b"Content-Type", b"NO"))]

    if b"content" in request.GET:
        content = request.GET.first(b"content")
    else:
        content = request.body

    return headers, content
