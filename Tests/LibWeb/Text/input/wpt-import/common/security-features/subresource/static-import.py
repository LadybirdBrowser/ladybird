import os, sys, json
from urllib.parse import unquote

from wptserve.utils import isomorphic_decode
import importlib
subresource = importlib.import_module("common.security-features.subresource.subresource")

def get_csp_value(value):
    '''
    Returns actual CSP header values (e.g. "worker-src 'self'") for the
    given string used in PolicyDelivery's value (e.g. "worker-src-self").
    '''

    # script-src
    # Test-related scripts like testharness.js and inline scripts containing
    # test bodies.
    # 'unsafe-inline' is added as a workaround here. This is probably not so
    # bad, as it shouldn't intefere non-inline-script requests that we want to
    # test.
    if value == 'script-src-wildcard':
        return "script-src * 'unsafe-inline'"
    if value == 'script-src-self':
        return "script-src 'self' 'unsafe-inline'"
    # Workaround for "script-src 'none'" would be more complicated, because
    # - "script-src 'none' 'unsafe-inline'" is handled somehow differently from
    #   "script-src 'none'", i.e.
    #   https://w3c.github.io/webappsec-csp/#match-url-to-source-list Step 3
    #   handles the latter but not the former.
    # - We need nonce- or path-based additional values to allow same-origin
    #   test scripts like testharness.js.
    # Therefore, we disable 'script-src-none' tests for now in
    # `/content-security-policy/spec.src.json`.
    if value == 'script-src-none':
        return "script-src 'none'"

    # worker-src
    if value == 'worker-src-wildcard':
        return 'worker-src *'
    if value == 'worker-src-self':
        return "worker-src 'self'"
    if value == 'worker-src-none':
        return "worker-src 'none'"
    raise Exception('Invalid delivery_value: %s' % value)

def generate_payload(request):
    import_url = unquote(isomorphic_decode(request.GET[b'import_url']))
    return subresource.get_template(u"static-import.js.template") % {
        u"import_url": import_url
    }

def main(request, response):
    def payload_generator(_): return generate_payload(request)
    maybe_additional_headers = {}
    if b'contentSecurityPolicy' in request.GET:
        csp = unquote(isomorphic_decode(request.GET[b'contentSecurityPolicy']))
        maybe_additional_headers[b'Content-Security-Policy'] = get_csp_value(csp)
    subresource.respond(request,
                        response,
                        payload_generator = payload_generator,
                        content_type = b"application/javascript",
                        maybe_additional_headers = maybe_additional_headers)
