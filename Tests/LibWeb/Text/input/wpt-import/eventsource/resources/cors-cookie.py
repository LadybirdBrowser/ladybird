from datetime import datetime

def main(request, response):
    last_event_id = request.headers.get(b"Last-Event-Id", b"")
    ident = request.GET.first(b'ident', b"test")
    cookie = b"COOKIE" if ident in request.cookies else b"NO_COOKIE"
    origin = request.GET.first(b'origin', request.headers[b"origin"])
    credentials = request.GET.first(b'credentials', b'true')

    headers = []

    if origin != b'none':
        headers.append((b"Access-Control-Allow-Origin", origin));

    if credentials != b'none':
        headers.append((b"Access-Control-Allow-Credentials", credentials));

    if last_event_id == b'':
        headers.append((b"Content-Type", b"text/event-stream"))
        response.set_cookie(ident, b"COOKIE")
        data = b"id: 1\nretry: 200\ndata: first %s\n\n" % cookie
    elif last_event_id == b'1':
        headers.append((b"Content-Type", b"text/event-stream"))
        long_long_time_ago = datetime.now().replace(year=2001, month=7, day=27)
        response.set_cookie(ident, b"COOKIE", expires=long_long_time_ago)
        data = b"id: 2\ndata: second %s\n\n" % cookie
    else:
        headers.append((b"Content-Type", b"stop"))
        data = b"data: " + last_event_id + cookie + b"\n\n";

    return headers, data
