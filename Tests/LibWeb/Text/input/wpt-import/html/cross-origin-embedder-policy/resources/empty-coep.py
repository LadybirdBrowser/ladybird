def main(request, response):
    headers = [(b'Content-Type', b'text/html')]

    for value in request.GET.get_list(b'value'):
        headers.append((b'Cross-Origin-Embedder-Policy', value))

    return (200, headers, u'')
