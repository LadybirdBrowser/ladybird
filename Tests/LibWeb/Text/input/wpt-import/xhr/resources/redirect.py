import time
from urllib.parse import parse_qs

from wptserve.utils import isomorphic_encode

def main(request, response):
    code = int(request.GET.first(b"code", 302))
    location = request.GET.first(b"location", isomorphic_encode(request.url_parts.path + u"?followed"))
    if location:
        location = parse_qs(u"location=" + location.decode(u"UTF-8"))[u"location"][0]
        if location.startswith(u"redirect.py"):
            location += u"&code=" + str(code)

    if b"delay" in request.GET:
        delay = float(request.GET.first(b"delay"))
        time.sleep(delay / 1E3)

    if b"followed" in request.GET:
        return [(b"Content:Type", b"text/plain")], b"MAGIC HAPPENED"
    else:
        return (code, b"WEBSRT MARKETING"), [(b"Location", location)], b"TEST"
