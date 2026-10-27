import json
from wptserve.utils import isomorphic_decode

# A server used to store and retrieve arbitrary data.
# This is used by: ./dispatcher.js
def main(request, response):
    # This server is configured so that is accept to receive any requests and
    # any cookies the web browser is willing to send.
    response.headers.set(b"Access-Control-Allow-Credentials", b"true")
    response.headers.set(b'Access-Control-Allow-Methods', b'OPTIONS, GET, POST')
    response.headers.set(b'Access-Control-Allow-Headers', b'Content-Type')
    response.headers.set(b"Access-Control-Allow-Origin", request.headers.get(b"origin") or '*')

    if b"cacheable" in request.GET:
        response.headers.set(b"Cache-Control", b"max-age=31536000")
    else:
        response.headers.set(b'Cache-Control', b'no-cache, no-store, must-revalidate')

    # CORS preflight
    if request.method == u'OPTIONS':
        return b''

    uuid = request.GET[b'uuid']
    stash = request.server.stash;

    # The stash is accessed concurrently by many clients. A lock is used to
    # avoid unterleaved read/write from different clients.
    with stash.lock:
        queue = stash.take(uuid, '/common/dispatcher') or [];

        # Push into the |uuid| queue, the requested headers.
        if b"show-headers" in request.GET:
            headers = {};
            for key, value in request.headers.items():
                headers[isomorphic_decode(key)] = isomorphic_decode(request.headers[key])
            headers = json.dumps(headers);
            queue.append(headers);
            ret = b'';

        # Push into the |uuid| queue, the posted data.
        elif request.method == u'POST':
            queue.append(request.body)
            ret = b'done'

        # Pull from the |uuid| queue, the posted data.
        else:
            if len(queue) == 0:
                ret = b'not ready'
            else:
                ret = queue.pop(0)

        stash.put(uuid, queue, '/common/dispatcher')
    return ret;
