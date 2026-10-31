def main(request, response):
    cookie = request.cookies.first(b"COOKIE_NAME", None)

    response_headers = [(b"Content-Type", b"text/javascript"),
                        (b"Access-Control-Allow-Credentials", b"true")]

    origin = request.headers.get(b"Origin", None)
    if origin:
        response_headers.append((b"Access-Control-Allow-Origin", origin))

    cookie_value = b'';
    if cookie:
        cookie_value = cookie.value;
    return (200, response_headers,
            b"export const cookie = '"+cookie_value+b"';")
