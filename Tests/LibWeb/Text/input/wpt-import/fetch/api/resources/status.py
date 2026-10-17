from wptserve.utils import isomorphic_encode

def main(request, response):
    code = int(request.GET.first(b"code", 200))
    text = request.GET.first(b"text", b"OMG")
    content = request.GET.first(b"content", b"")
    type = request.GET.first(b"type", b"")
    status = (code, text)
    headers = [(b"Content-Type", type),
               (b"X-Request-Method", isomorphic_encode(request.method))]
    return status, headers, content
