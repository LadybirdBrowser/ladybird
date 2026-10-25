import os, sys, json

from wptserve.utils import isomorphic_decode, isomorphic_encode

import importlib
util = importlib.import_module("common.security-features.scope.util")

def main(request, response):
  policyDeliveries = json.loads(request.GET.first(b"policyDeliveries", b"[]"))
  maybe_additional_headers = {}
  meta = u''
  error = u''
  for delivery in policyDeliveries:
    if delivery[u'deliveryType'] == u'meta':
      if delivery[u'key'] == u'referrerPolicy':
        meta += u'<meta name="referrer" content="%s">' % delivery[u'value']
      else:
        error = u'invalid delivery key'
    elif delivery[u'deliveryType'] == u'http-rp':
      if delivery[u'key'] == u'referrerPolicy':
        maybe_additional_headers[b'Referrer-Policy'] = isomorphic_encode(delivery[u'value'])
      else:
        error = u'invalid delivery key'
    else:
      error = u'invalid deliveryType'

  handler = lambda: util.get_template(u"document.html.template") % ({
      u"meta": meta,
      u"error": error,
      u"scriptOrigin": request.url_parts.scheme + u'://' + request.url_parts.hostname + u':' + str(request.url_parts.port)
  })
  util.respond(
      request,
      response,
      payload_generator=handler,
      content_type=b"text/html",
      maybe_additional_headers=maybe_additional_headers)
