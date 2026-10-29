def main(request, response):
    headers = [(b"Content-Encoding", b"gzip")]
    return headers, b"not actually gzip"
