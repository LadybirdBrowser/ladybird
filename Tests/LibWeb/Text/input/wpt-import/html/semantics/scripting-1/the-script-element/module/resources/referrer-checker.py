def main(request, response):
    referrer = request.headers.get(b"referer", b"")
    response_headers = [(b"Content-Type", b"text/javascript"),
                        (b"Access-Control-Allow-Origin", b"*")]
    return (200, response_headers,
            b"export const referrer = '" + referrer + b"';")
