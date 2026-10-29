def main(request, response):
    headers = [(b"Content-Encoding", b"zstd")]
    return headers, b"not actually zstd"
