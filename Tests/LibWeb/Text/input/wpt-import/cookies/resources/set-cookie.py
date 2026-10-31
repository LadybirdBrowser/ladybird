from datetime import date

def main(request, response):
    """
    Returns cookie name and path from query params in a Set-Cookie header.

    e.g.

    > GET /cookies/resources/set-cookie.py?name=match-slash&path=%2F HTTP/1.1
    > Host: localhost:8000
    > User-Agent: curl/7.43.0
    > Accept: */*
    >
    < HTTP/1.1 200 OK
    < Content-Type: application/json
    < Set-Cookie: match-slash=1; Path=/; Expires=09 Jun 2021 10:18:14 GMT
    < Server: BaseHTTP/0.3 Python/2.7.12
    < Date: Tue, 04 Oct 2016 18:16:06 GMT
    < Content-Length: 80
    """

    name = request.GET[b'name']
    path = request.GET[b'path']
    samesite = request.GET.get(b'samesite')
    secure = b'secure' in request.GET
    expiry_year = date.today().year + 1
    cookie = b"%s=1; Path=%s; Expires=09 Jun %d 10:18:14 GMT" % (name, path, expiry_year)
    if samesite:
        cookie += b";SameSite=%s" % samesite
    if secure:
        cookie += b";Secure"

    headers = [
        (b"Content-Type", b"application/json"),
        (b"Set-Cookie", cookie)
    ]

    # Set the cors enabled headers.
    origin = request.headers.get(b"Origin")
    if origin is not None and origin != b"null":
        headers.append((b"Access-Control-Allow-Origin", origin))
        headers.append((b"Access-Control-Allow-Credentials", 'true'))

    body = b"var dummy='value';"
    return headers, body
