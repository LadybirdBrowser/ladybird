def main(request, response):
    """Send a response with the Origin-Agent-Cluster header given in the
    "header" query parameter, or no header if that is not provided. Other query
    parameters (only their presence/absence matters) are "send-loaded-message"
    and "redirect-first", which modify the behavior a bit.

    In either case, the response will listen for various messages posted and
    coordinate with the sender. See ./helpers.mjs for how these handlers are
    used.
    """

    if b"redirect-first" in request.GET:
      # Create a new query string, which is the same as the one we're given but
      # with the redirect-first component stripped out. This allows tests to use
      # any value (or no value) for the other query params, in combination with
      # redirect-first.
      query_string_pieces = []
      if b"header" in request.GET:
        query_string_pieces.append(b"header=" + request.GET.first(b"header"))
      if b"send-loaded-message" in request.GET:
        query_string_pieces.append(b"send-loaded-message")
      query_string = b"?" + b"&".join(query_string_pieces)

      return (
        302,
        [(b"Location", b"/html/browsers/origin/origin-keyed-agent-clusters/resources/send-oac-header.py" + query_string)],
        u""
      )

    if b"header" in request.GET:
      header = request.GET.first(b"header")
      response.headers.set(b"Origin-Agent-Cluster", header)

    response.headers.set(b"Content-Type", b"text/html")

    return u"""
    <!DOCTYPE html>
    <meta charset="utf-8">
    <title>Helper page for origin-keyed agent cluster tests</title>

    <body>
    <script type="module" src="send-header-page-script.mjs"></script>
    """
