def main(request, response):
    headers = []
    if b"headers" in request.GET:
        checked_headers = request.GET.first(b"headers").split(b"|")
        for header in checked_headers:
            if header in request.headers:
                headers.append((b"x-request-" + header, request.headers.get(header, b"")))

    if b"cors" in request.GET:
        if b"Origin" in request.headers:
            headers.append((b"Access-Control-Allow-Origin", request.headers.get(b"Origin", b"")))
        else:
            headers.append((b"Access-Control-Allow-Origin", b"*"))
        headers.append((b"Access-Control-Allow-Credentials", b"true"))
        headers.append((b"Access-Control-Allow-Methods", b"GET, POST, HEAD"))
        exposed_headers = [b"x-request-" + header for header in checked_headers]
        headers.append((b"Access-Control-Expose-Headers", b", ".join(exposed_headers)))
        if b"allow_headers" in request.GET:
            headers.append((b"Access-Control-Allow-Headers", request.GET[b'allow_headers']))
        else:
            headers.append((b"Access-Control-Allow-Headers", b", ".join(request.headers)))

    headers.append((b"content-type", b"text/plain"))
    return headers, b""
