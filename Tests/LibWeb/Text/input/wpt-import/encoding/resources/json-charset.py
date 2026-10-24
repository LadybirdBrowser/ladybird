def main(request, response):
    content_type = request.GET.first(b"type", b"application/json")
    charset = request.GET.first(b"charset", None)
    if charset is not None:
        content_type += b";charset=" + charset
    response.headers.set(b"Content-Type", content_type)
    response.headers.set(b"X-Content-Type-Options", b"nosniff")
    response.content = '{"text":"☃é"}'.encode("utf-8")
