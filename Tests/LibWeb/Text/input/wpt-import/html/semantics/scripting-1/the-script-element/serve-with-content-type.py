import os

from wptserve.utils import isomorphic_decode

def main(request, response):
    directory = os.path.dirname(isomorphic_decode(__file__))

    try:
        file_name = request.GET.first(b"fn")
        content_type = request.GET.first(b"ct")
        with open(os.path.join(directory, isomorphic_decode(file_name)), u"rb") as fh:
            content = fh.read()

        response.headers.set(b"Content-Type", content_type)
        response.content = content
    except:
        response.set_error(400, u"Not enough parameters or file not found")
