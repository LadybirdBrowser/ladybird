def main(request, response):
    response_headers = [(b"Access-Control-Allow-Origin", b"*"), (b"Content-Type", b"text/javascript")]
    body = b"""
    window.referrers["%s"] = "%s";
    """ % (request.GET.first(b"uid", b""), request.headers.get(b"referer", b""))
    return (200, response_headers, body)
