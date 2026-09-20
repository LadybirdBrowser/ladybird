from wptserve.utils import isomorphic_encode

def main(request, response):
    headers = []
    if b"cors" in request.GET:
        headers.append((b"Access-Control-Allow-Origin", b"*"))
        headers.append((b"Access-Control-Allow-Credentials", b"true"))
        headers.append((b"Access-Control-Allow-Methods", b"GET, POST, PUT, FOO"))
        headers.append((b"Access-Control-Allow-Headers", b"x-test, x-foo"))
        headers.append((b"Access-Control-Expose-Headers", b"x-request-method"))

    headers.append((b"x-request-method", isomorphic_encode(request.method)))
    headers.append((b"x-request-content-type", request.headers.get(b"Content-Type", b"NO")))
    headers.append((b"x-request-content-length", request.headers.get(b"Content-Length", b"NO")))
    headers.append((b"x-request-content-encoding", request.headers.get(b"Content-Encoding", b"NO")))
    headers.append((b"x-request-content-language", request.headers.get(b"Content-Language", b"NO")))
    headers.append((b"x-request-content-location", request.headers.get(b"Content-Location", b"NO")))
    return headers, request.body
