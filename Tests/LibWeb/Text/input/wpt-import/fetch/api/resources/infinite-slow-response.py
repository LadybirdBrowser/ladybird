import time


def url_dir(request):
    return u'/'.join(request.url_parts.path.split(u'/')[:-1]) + u'/'


def stash_write(request, key, value):
    """Write to the stash, overwriting any previous value"""
    request.server.stash.take(key, url_dir(request))
    request.server.stash.put(key, value, url_dir(request))


def main(request, response):
    stateKey = request.GET.first(b"stateKey", b"")
    abortKey = request.GET.first(b"abortKey", b"")

    if stateKey:
        stash_write(request, stateKey, 'open')

    response.headers.set(b"Content-type", b"text/plain")
    response.write_status_headers()

    # Writing an initial 2k so browsers realise it's there. *shrug*
    response.writer.write(b"." * 2048)

    while True:
        if not response.writer.write(b"."):
            break
        if abortKey and request.server.stash.take(abortKey, url_dir(request)):
            break
        time.sleep(0.01)

    if stateKey:
        stash_write(request, stateKey, 'closed')
