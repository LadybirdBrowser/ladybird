def main(request, response):
    headers = [(b"Location", request.GET[b'redirectTo'])]
    if b'corp' in request.GET:
        headers.append((b'Cross-Origin-Resource-Policy', request.GET[b'corp']))

    return 302, headers, b""
