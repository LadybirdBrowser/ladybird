import datetime
import json
import time
from base64 import b64decode

from wptserve.utils import isomorphic_decode, isomorphic_encode

NOTEHDRS = set([u'content-type', u'access-control-allow-origin', u'last-modified', u'etag'])
NOBODYSTATUS = set([204, 304])
LOCATIONHDRS = set([u'location', u'content-location'])
DATEHDRS = set([u'date', u'expires', u'last-modified'])

def main(request, response):
    dispatch = request.GET.first(b"dispatch", None)
    uuid = request.GET.first(b"uuid", None)
    response.headers.set(b"Access-Control-Allow-Credentials", b"true")

    if request.method == u"OPTIONS":
        return handle_preflight(uuid, request, response)
    if not uuid:
        response.status = (404, b"Not Found")
        response.headers.set(b"Content-Type", b"text/plain")
        return b"UUID not found"
    if dispatch == b'test':
        return handle_test(uuid, request, response)
    elif dispatch == b'state':
        return handle_state(uuid, request, response)
    response.status = (404, b"Not Found")
    response.headers.set(b"Content-Type", b"text/plain")
    return b"Fallthrough"

def handle_preflight(uuid, request, response):
    response.status = (200, b"OK")
    response.headers.set(b"Access-Control-Allow-Origin", request.headers.get(b"origin") or '*')
    response.headers.set(b"Access-Control-Allow-Methods", b"GET")
    response.headers.set(b"Access-Control-Allow-Headers", request.headers.get(b"Access-Control-Request-Headers") or "*")
    response.headers.set(b"Access-Control-Max-Age", b"86400")
    return b"Preflight request"

def handle_state(uuid, request, response):
    response.headers.set(b"Content-Type", b"text/plain")
    return json.dumps(request.server.stash.take(uuid))

def handle_test(uuid, request, response):
    server_state = request.server.stash.take(uuid) or []
    try:
        requests = json.loads(b64decode(request.headers.get(b'Test-Requests', b"")))
    except:
        response.status = (400, b"Bad Request")
        response.headers.set(b"Content-Type", b"text/plain")
        return b"No or bad Test-Requests request header"
    config = requests[len(server_state)]
    if not config:
        response.status = (404, b"Not Found")
        response.headers.set(b"Content-Type", b"text/plain")
        return b"Config not found"
    noted_headers = {}
    now = time.time()
    for header in config.get(u'response_headers', []):
        if header[0].lower() in LOCATIONHDRS: # magic locations
            if (len(header[1]) > 0):
                header[1] = u"%s&target=%s" % (request.url, header[1])
            else:
                header[1] = request.url
        if header[0].lower() in DATEHDRS and isinstance(header[1], int):  # magic dates
            header[1] = http_date(now, header[1])
        response.headers.set(isomorphic_encode(header[0]), isomorphic_encode(header[1]))
        if header[0].lower() in NOTEHDRS:
            noted_headers[header[0].lower()] = header[1]
    state = {
        u'now': now,
        u'request_method': request.method,
        u'request_headers': dict([[isomorphic_decode(h.lower()), isomorphic_decode(request.headers[h])] for h in request.headers]),
        u'response_headers': noted_headers
    }
    server_state.append(state)
    request.server.stash.put(uuid, server_state)

    if u"access-control-allow-origin" not in noted_headers:
        response.headers.set(b"Access-Control-Allow-Origin", b"*")
    if u"content-type" not in noted_headers:
        response.headers.set(b"Content-Type", b"text/plain")
    response.headers.set(b"Server-Request-Count", len(server_state))

    code, phrase = config.get(u"response_status", [200, b"OK"])
    if config.get(u"expected_type", u"").endswith(u'validated'):
        ref_hdrs = server_state[0][u'response_headers']
        previous_lm = ref_hdrs.get(u'last-modified', False)
        if previous_lm and request.headers.get(b"If-Modified-Since", False) == isomorphic_encode(previous_lm):
            code, phrase = [304, b"Not Modified"]
        previous_etag = ref_hdrs.get(u'etag', False)
        if previous_etag and request.headers.get(b"If-None-Match", False) == isomorphic_encode(previous_etag):
            code, phrase = [304, b"Not Modified"]
        if code != 304:
            code, phrase = [999, b'304 Not Generated']
    response.status = (code, phrase)

    content = config.get(u"response_body", uuid)
    if code in NOBODYSTATUS:
        return b""
    return content


def get_header(headers, header_name):
    result = None
    for header in headers:
        if header[0].lower() == header_name.lower():
            result = header[1]
    return result

WEEKDAYS = [u'Mon', u'Tue', u'Wed', u'Thu', u'Fri', u'Sat', u'Sun']
MONTHS = [None, u'Jan', u'Feb', u'Mar', u'Apr', u'May', u'Jun', u'Jul',
          u'Aug', u'Sep', u'Oct', u'Nov', u'Dec']

def http_date(now, delta_secs=0):
    date = datetime.datetime.utcfromtimestamp(now + delta_secs)
    return u"%s, %.2d %s %.4d %.2d:%.2d:%.2d GMT" % (
        WEEKDAYS[date.weekday()],
        date.day,
        MONTHS[date.month],
        date.year,
        date.hour,
        date.minute,
        date.second)
