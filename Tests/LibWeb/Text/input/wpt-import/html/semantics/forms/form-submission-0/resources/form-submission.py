from urllib.parse import parse_qsl

def main(request, response):
    params = dict(parse_qsl(request.url_parts.query))
    if params.get('query') == '1':
        if request.headers.get(b'Content-Type') == b'application/x-www-form-urlencoded':
            result = request.body == b'foo=bara'
        elif request.headers.get(b'Content-Type') == b'text/plain':
            result = request.body == b'qux=baz\r\n'
        else:
            result = request.POST.first(b'foo') == b'bar'
    elif params.get('expected_body') is not None:
        result = request.body == params['expected_body'].encode('UTF-8')
    else:
        result = False

    return ([(b"Content-Type", b"text/plain")],
            b"OK" if result else b"FAIL")
