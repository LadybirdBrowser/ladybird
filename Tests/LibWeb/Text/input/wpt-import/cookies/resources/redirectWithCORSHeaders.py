from cookies.resources.helpers import setNoCacheAndCORSHeaders

def main(request, response):
    """Simple handler that causes redirection.

    The request should typically have two query parameters:
    status - The status to use for the redirection. Defaults to 302.
    location - The resource to redirect to.
    """
    status = 302
    if b"status" in request.GET:
        try:
            status = int(request.GET.first(b"status"))
        except ValueError:
            pass
    headers = setNoCacheAndCORSHeaders(request, response)

    location = request.GET.first(b"location")

    headers.append((b"Location", location))

    return status, headers, b""
