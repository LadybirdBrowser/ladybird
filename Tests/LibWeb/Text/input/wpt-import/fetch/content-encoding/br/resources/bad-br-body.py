def main(request, response):
    headers = [(b"Content-Encoding", b"br")]
    return headers, b"not actually br"
