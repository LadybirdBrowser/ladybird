def main(request, response):
    headers = [(b"Cross-Origin-Resource-Policy", request.GET[b'corp'])]
    if b'origin' in request.headers:
        headers.append((b'Access-Control-Allow-Origin', request.headers[b'origin']))

    return 200, headers, b"hello"
