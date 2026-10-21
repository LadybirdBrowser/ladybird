from wptserve.utils import isomorphic_decode

def should_be_treated_as_same_origin_request(request):
  """Tells whether request should be treated as same-origin request."""
  # In both of the following cases, allow to proceed with handling to simulate
  # 'no-cors' mode: response is sent, but browser will make it opaque.
  if request.GET.first(b'mode') == b'no-cors':
    return True

  # We can't rely on the Origin header field of a fetch request, as it is only
  # present for 'cors' mode or methods other than 'GET'/'HEAD' (i.e. present for
  # 'POST'). See https://fetch.spec.whatwg.org/#http-origin
  assert 'frame_origin ' in request.GET
  frame_origin = request.GET.first(b'frame_origin').decode('utf-8')
  host_origin = request.url_parts.scheme + '://' + request.url_parts.netloc
  return frame_origin == host_origin

def main(request, response):
  if request.method == u'OPTIONS':
    # CORS preflight
    response.headers.set(b'Access-Control-Allow-Origin', b'*')
    response.headers.set(b'Access-Control-Allow-Methods', b'*')
    response.headers.set(b'Access-Control-Allow-Headers', b'*')
    return 'done'

  if b'disallow_cross_origin' not in request.GET:
    response.headers.set(b'Access-Control-Allow-Origin', b'*')
  elif not should_be_treated_as_same_origin_request(request):
    # As simple requests will not trigger preflight, we have to manually block
    # cors requests before making any changes to storage.
    # https://developer.mozilla.org/en-US/docs/Web/HTTP/CORS#simple_requests
    # https://fetch.spec.whatwg.org/#cors-preflight-fetch
    return 'not stashing for cors request'

  url_dir = u'/'.join(request.url_parts.path.split(u'/')[:-1]) + u'/'
  key = request.GET.first(b'key')
  value = request.GET.first(b'value')
  # value here must be a text string. It will be json.dump()'ed in stash-take.py.
  request.server.stash.put(key, isomorphic_decode(value), url_dir)

  return 'done'
