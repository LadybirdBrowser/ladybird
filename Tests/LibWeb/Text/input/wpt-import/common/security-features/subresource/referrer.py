def main(request, response):
    referrer = request.headers.get(b"referer", b"")
    response_headers = [(b"Content-Type", b"text/javascript")]
    return (200, response_headers, b"window.referrer = '" + referrer + b"'")
