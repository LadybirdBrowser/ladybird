def main(request, response):
    headers = [(b"Content-Type", b"text/plain")]
    stashed_data = {b'control_request_headers': b"", b'preflight': b"0", b'preflight_referrer': b""}

    token = None
    if b"token" in request.GET:
        token = request.GET.first(b"token")

    if b"origin" in request.GET:
        for origin in request.GET[b'origin'].split(b", "):
            headers.append((b"Access-Control-Allow-Origin", origin))
    else:
        headers.append((b"Access-Control-Allow-Origin", b"*"))

    if b"clear-stash" in request.GET:
        if request.server.stash.take(token) is not None:
            return headers, b"1"
        else:
            return headers, b"0"

    if b"credentials" in request.GET:
        headers.append((b"Access-Control-Allow-Credentials", b"true"))

    if request.method == u"OPTIONS":
        if not b"Access-Control-Request-Method" in request.headers:
            response.set_error(400, u"No Access-Control-Request-Method header")
            return b"ERROR: No access-control-request-method in preflight!"

        if request.headers.get(b"Accept", b"") != b"*/*":
            response.set_error(400, u"Request does not have 'Accept: */*' header")
            return b"ERROR: Invalid access in preflight!"

        if b"control_request_headers" in request.GET:
            stashed_data[b'control_request_headers'] = request.headers.get(b"Access-Control-Request-Headers", None)

        if b"max_age" in request.GET:
            headers.append((b"Access-Control-Max-Age", request.GET[b'max_age']))

        if b"allow_headers" in request.GET:
            headers.append((b"Access-Control-Allow-Headers", request.GET[b'allow_headers']))

        if b"allow_methods" in request.GET:
            headers.append((b"Access-Control-Allow-Methods", request.GET[b'allow_methods']))

        preflight_status = 200
        if b"preflight_status" in request.GET:
            preflight_status = int(request.GET.first(b"preflight_status"))

        stashed_data[b'preflight'] = b"1"
        stashed_data[b'preflight_referrer'] = request.headers.get(b"Referer", b"")
        stashed_data[b'preflight_user_agent'] = request.headers.get(b"User-Agent", b"")
        if token:
            request.server.stash.put(token, stashed_data)

        return preflight_status, headers, b""


    if token:
        data = request.server.stash.take(token)
        if data:
            stashed_data = data

    if b"checkUserAgentHeaderInPreflight" in request.GET and request.headers.get(b"User-Agent") != stashed_data[b'preflight_user_agent']:
        return 400, headers, b"ERROR: No user-agent header in preflight"

    #use x-* headers for returning value to bodyless responses
    headers.append((b"Access-Control-Expose-Headers", b"x-did-preflight, x-control-request-headers, x-referrer, x-preflight-referrer, x-origin"))
    headers.append((b"x-did-preflight", stashed_data[b'preflight']))
    if stashed_data[b'control_request_headers'] != None:
        headers.append((b"x-control-request-headers", stashed_data[b'control_request_headers']))
    headers.append((b"x-preflight-referrer", stashed_data[b'preflight_referrer']))
    headers.append((b"x-referrer", request.headers.get(b"Referer", b"")))
    headers.append((b"x-origin", request.headers.get(b"Origin", b"")))

    if token:
        request.server.stash.put(token, stashed_data)

    return headers, b""
