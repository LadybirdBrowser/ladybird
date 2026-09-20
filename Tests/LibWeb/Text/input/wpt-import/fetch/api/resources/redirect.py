import time

from urllib.parse import urlencode, urlparse

from wptserve.utils import isomorphic_decode, isomorphic_encode

def main(request, response):
    stashed_data = {b'count': 0, b'preflight': b"0"}
    status = 302
    headers = [(b"Content-Type", b"text/plain"),
               (b"Cache-Control", b"no-cache"),
               (b"Pragma", b"no-cache")]
    if b"Origin" in request.headers:
        headers.append((b"Access-Control-Allow-Origin", request.headers.get(b"Origin", b"")))
        headers.append((b"Access-Control-Allow-Credentials", b"true"))
    else:
        headers.append((b"Access-Control-Allow-Origin", b"*"))

    token = None
    if b"token" in request.GET:
        token = request.GET.first(b"token")
        data = request.server.stash.take(token)
        if data:
            stashed_data = data

    if request.method == u"OPTIONS":
        if b"allow_headers" in request.GET:
            headers.append((b"Access-Control-Allow-Headers", request.GET[b'allow_headers']))
        stashed_data[b'preflight'] = b"1"
        #Preflight is not redirected: return 200
        if not b"redirect_preflight" in request.GET:
            if token:
                request.server.stash.put(request.GET.first(b"token"), stashed_data)
            return 200, headers, u""

    if b"redirect_status" in request.GET:
        status = int(request.GET[b'redirect_status'])
    elif b"redirect_status" in request.POST:
        status = int(request.POST[b'redirect_status'])

    stashed_data[b'count'] += 1

    if b"location" in request.GET:
        url = isomorphic_decode(request.GET[b'location'])
        if b"simple" not in request.GET:
            scheme = urlparse(url).scheme
            if scheme == u"" or scheme == u"http" or scheme == u"https":
                url += u"&" if u'?' in url else u"?"
                #keep url parameters in location
                url_parameters = {}
                for item in request.GET.items():
                    url_parameters[isomorphic_decode(item[0])] = isomorphic_decode(item[1][0])
                url += urlencode(url_parameters)
                #make sure location changes during redirection loop
                url += u"&count=" + str(stashed_data[b'count'])
        headers.append((b"Location", isomorphic_encode(url)))

    if b"redirect_referrerpolicy" in request.GET:
        headers.append((b"Referrer-Policy", request.GET[b'redirect_referrerpolicy']))

    if b"delay" in request.GET:
        time.sleep(float(request.GET.first(b"delay", 0)) / 1E3)

    if token:
        request.server.stash.put(request.GET.first(b"token"), stashed_data)
        if b"max_count" in request.GET:
            max_count = int(request.GET[b'max_count'])
            #stop redirecting and return count
            if stashed_data[b'count'] > max_count:
                # -1 because the last is not a redirection
                return str(stashed_data[b'count'] - 1)

    return status, headers, u""
