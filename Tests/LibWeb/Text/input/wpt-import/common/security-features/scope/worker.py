import os, sys, json

from wptserve.utils import isomorphic_decode, isomorphic_encode
import importlib
util = importlib.import_module("common.security-features.scope.util")

def main(request, response):
  policyDeliveries = json.loads(request.GET.first(b'policyDeliveries', b'[]'))
  worker_type = request.GET.first(b'type', b'classic')
  commonjs_url = u'%s://%s:%s/common/security-features/resources/common.sub.js' % (
      request.url_parts.scheme, request.url_parts.hostname,
      request.url_parts.port)
  if worker_type == b'classic':
    import_line = u'importScripts("%s");' % commonjs_url
  else:
    import_line = u'import "%s";' % commonjs_url

  maybe_additional_headers = {}
  error = u''
  for delivery in policyDeliveries:
    if delivery[u'deliveryType'] == u'meta':
      error = u'<meta> cannot be used in WorkerGlobalScope'
    elif delivery[u'deliveryType'] == u'http-rp':
      if delivery[u'key'] == u'referrerPolicy':
        maybe_additional_headers[b'Referrer-Policy'] = isomorphic_encode(delivery[u'value'])
      elif delivery[u'key'] == u'mixedContent' and delivery[u'value'] == u'opt-in':
        maybe_additional_headers[b'Content-Security-Policy'] = b'block-all-mixed-content'
      elif delivery[u'key'] == u'upgradeInsecureRequests' and delivery[u'value'] == u'upgrade':
        maybe_additional_headers[b'Content-Security-Policy'] = b'upgrade-insecure-requests'
      else:
        error = u'invalid delivery key for http-rp: %s' % delivery[u'key']
    else:
      error = u'invalid deliveryType: %s' % delivery[u'deliveryType']

  handler = lambda: util.get_template(u'worker.js.template') % ({
      u'import': import_line,
      u'error': error
  })
  util.respond(
      request,
      response,
      payload_generator=handler,
      content_type=b'text/javascript',
      maybe_additional_headers=maybe_additional_headers)
