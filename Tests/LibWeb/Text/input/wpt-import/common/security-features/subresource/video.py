import os, sys
from wptserve.utils import isomorphic_decode
import importlib
subresource = importlib.import_module("common.security-features.subresource.subresource")

def generate_payload(request, server_data):
    file = os.path.join(request.doc_root, u"media", u"movie_5.webm")
    return open(file, "rb").read()


def main(request, response):
    handler = lambda data: generate_payload(request, data)
    subresource.respond(request,
                        response,
                        payload_generator = handler,
                        access_control_allow_origin = b"*",
                        content_type = b"video/webm")
