from wptserve.handlers import json_handler


@json_handler
def main(request, response):
    dir = u'/'.join(request.url_parts.path.split(u'/')[:-1]) + u'/'
    key = request.GET.first(b"key")
    response.headers.set(b'Access-Control-Allow-Origin', b'*')
    return request.server.stash.take(key, dir)
