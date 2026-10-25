import os, sys
from wptserve.utils import isomorphic_decode
import importlib
subresource = importlib.import_module("common.security-features.subresource.subresource")

def generate_payload(request, server_data):
    data = (u'{"headers": %(headers)s}') % server_data
    type = b'image'
    if b"type" in request.GET:
        type = request.GET[b"type"]

    if b"id" in request.GET:
        request.server.stash.put(request.GET[b"id"], data)

    if type == b'image':
        return subresource.get_template(u"image.css.template") % {u"id": isomorphic_decode(request.GET[b"id"])}

    elif type == b'font':
        return subresource.get_template(u"font.css.template") % {u"id": isomorphic_decode(request.GET[b"id"])}

    elif type == b'svg':
        return subresource.get_template(u"svg.css.template") % {
            u"id": isomorphic_decode(request.GET[b"id"]),
            u"property": isomorphic_decode(request.GET[b"property"])}

    # A `'stylesheet-only'`-type stylesheet has no nested resources; this is
    # useful in tests that cover referrers for stylesheet fetches (e.g. fetches
    # triggered by `@import` statements).
    elif type == b'stylesheet-only':
        return u''

def generate_import_rule(request, server_data):
    return u"@import url('%(url)s');" % {
        u"url": subresource.create_url(request, swap_origin=True,
                                       query_parameter_to_remove=u"import-rule")
    }

def generate_report_headers_payload(request, server_data):
    stashed_data = request.server.stash.take(request.GET[b"id"])
    return stashed_data

def main(request, response):
    payload_generator = lambda data: generate_payload(request, data)
    content_type = b"text/css"
    referrer_policy = b"unsafe-url"
    if b"import-rule" in request.GET:
        payload_generator = lambda data: generate_import_rule(request, data)

    if b"report-headers" in request.GET:
        payload_generator = lambda data: generate_report_headers_payload(request, data)
        content_type = b'application/json'

    if b"referrer-policy" in request.GET:
        referrer_policy = request.GET[b"referrer-policy"]

    subresource.respond(
        request,
        response,
        payload_generator = payload_generator,
        content_type = content_type,
        maybe_additional_headers = { b"Referrer-Policy": referrer_policy })
