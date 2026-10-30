def main(request, response):
    headers = [(b"Content-Type", b"text/javascript"), (b"Cache-control", b"public, max-age=100")]
    body = u"throw('fox');"
    return 200, headers, body
