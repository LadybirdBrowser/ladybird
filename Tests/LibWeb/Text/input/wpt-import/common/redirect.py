def main(request, response):
    """Simple handler that causes redirection.

    The request should typically have two query parameters:
    status - The status to use for the redirection. Defaults to 302.
    location - The resource to redirect to.

    This utility optionally supports CORS (iff the `enable-cors` query param is
    present).
    """
    status = 302
    if b"status" in request.GET:
        try:
            status = int(request.GET.first(b"status"))
        except ValueError:
            pass

    response.status = status

    location = request.GET.first(b"location")

    response.headers.set(b"Location", location)

    if request.GET.get(b"enable-cors") is not None:
        origin = request.headers.get(b"Origin")
        if origin:
            response.headers.set(b"Content-Type", b"text/plain")
            response.headers.set(b"Access-Control-Allow-Origin", origin)
            response.headers.set(b"Access-Control-Allow-Credentials", 'true')
