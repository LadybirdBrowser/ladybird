import os.path

from wptserve.utils import isomorphic_decode

def main(request, response):
    type = request.GET.first(b"type", None)

    body = open(os.path.join(os.path.dirname(isomorphic_decode(__file__)), u"green.png"), u"rb").read()

    response.add_required_headers = False
    response.writer.write_status(200)

    if b'corp' in request.GET:
        response.writer.write_header(b"cross-origin-resource-policy", request.GET[b'corp'])
    if b'acao' in request.GET:
        response.writer.write_header(b"access-control-allow-origin", request.GET[b'acao'])
    response.writer.write_header(b"content-length", len(body))
    if(type != None):
      response.writer.write_header(b"content-type", type)
    response.writer.end_headers()

    response.writer.write(body)
