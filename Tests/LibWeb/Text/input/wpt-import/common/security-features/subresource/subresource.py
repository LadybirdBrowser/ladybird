import os, json
from urllib.parse import parse_qsl, SplitResult, urlencode, urlsplit, urlunsplit

from wptserve.utils import isomorphic_decode, isomorphic_encode

def get_template(template_basename):
    script_directory = os.path.dirname(os.path.abspath(isomorphic_decode(__file__)))
    template_directory = os.path.abspath(os.path.join(script_directory,
                                                      u"template"))
    template_filename = os.path.join(template_directory, template_basename)

    with open(template_filename, "r") as f:
        return f.read()


def redirect(url, response):
    response.add_required_headers = False
    response.writer.write_status(301)
    response.writer.write_header(b"access-control-allow-origin", b"*")
    response.writer.write_header(b"location", isomorphic_encode(url))
    response.writer.end_headers()
    response.writer.write(u"")


# TODO(kristijanburnik): subdomain_prefix is a hardcoded value aligned with
# referrer-policy-test-case.js. The prefix should be configured in one place.
def __get_swapped_origin_netloc(netloc, subdomain_prefix = u"www1."):
    if netloc.startswith(subdomain_prefix):
        return netloc[len(subdomain_prefix):]
    else:
        return subdomain_prefix + netloc


# Creates a URL (typically a redirect target URL) that is the same as the
# current request URL `request.url`, except for:
# - When `swap_scheme` or `swap_origin` is True, its scheme/origin is changed
#   to the other one. (http <-> https, ws <-> wss, etc.)
# - For `downgrade`, we redirect to a URL that would be successfully loaded
#   if and only if upgrade-insecure-request is applied.
# - `query_parameter_to_remove` parameter is removed from query part.
#   Its default is "redirection" to avoid redirect loops.
def create_url(request,
               swap_scheme=False,
               swap_origin=False,
               downgrade=False,
               query_parameter_to_remove=u"redirection"):
    parsed = urlsplit(request.url)
    destination_netloc = parsed.netloc

    scheme = parsed.scheme
    if swap_scheme:
        scheme = u"http" if parsed.scheme == u"https" else u"https"
        hostname = parsed.netloc.split(u':')[0]
        # Always use the HTTPS port to allow testing upgrades correctly, similar
        # to the downgrade case below.
        port = request.server.config[u"ports"]["https"][0]
        destination_netloc = u":".join([hostname, str(port)])

    if downgrade:
        # These rely on some unintuitive cleverness due to WPT's test setup:
        # 'Upgrade-Insecure-Requests' does not upgrade the port number,
        # so we use URLs in the form `http://[domain]:[https-port]`,
        # which will be upgraded to `https://[domain]:[https-port]`.
        # If the upgrade fails, the load will fail, as we don't serve HTTP over
        # the secure port.
        if parsed.scheme == u"https":
            scheme = u"http"
        elif parsed.scheme == u"wss":
            scheme = u"ws"
        else:
            raise ValueError(u"Downgrade redirection: Invalid scheme '%s'" %
                             parsed.scheme)
        hostname = parsed.netloc.split(u':')[0]
        port = request.server.config[u"ports"][parsed.scheme][0]
        destination_netloc = u":".join([hostname, str(port)])

    if swap_origin:
        destination_netloc = __get_swapped_origin_netloc(destination_netloc)

    parsed_query = parse_qsl(parsed.query, keep_blank_values=True)
    parsed_query = [x for x in parsed_query if x[0] != query_parameter_to_remove]

    destination_url = urlunsplit(SplitResult(
        scheme = scheme,
        netloc = destination_netloc,
        path = parsed.path,
        query = urlencode(parsed_query),
        fragment = None))

    return destination_url


def preprocess_redirection(request, response):
    if b"redirection" not in request.GET:
        return False

    redirection = request.GET[b"redirection"]

    if redirection == b"no-redirect":
        return False
    elif redirection == b"keep-scheme":
        redirect_url = create_url(request, swap_scheme=False)
    elif redirection == b"swap-scheme":
        redirect_url = create_url(request, swap_scheme=True)
    elif redirection == b"downgrade":
        redirect_url = create_url(request, downgrade=True)
    elif redirection == b"keep-origin":
        redirect_url = create_url(request, swap_origin=False)
    elif redirection == b"swap-origin":
        redirect_url = create_url(request, swap_origin=True)
    else:
        raise ValueError(u"Invalid redirection type '%s'" % isomorphic_decode(redirection))

    redirect(redirect_url, response)
    return True


def preprocess_stash_action(request, response):
    if b"action" not in request.GET:
        return False

    action = request.GET[b"action"]

    key = request.GET[b"key"]
    stash = request.server.stash
    path = request.GET[b"path"] if b"path" in request.GET \
           else isomorphic_encode(request.url.split(u'?')[0])

    if action == b"put":
        value = isomorphic_decode(request.GET[b"value"])
        stash.take(key=key, path=path)
        stash.put(key=key, value=value, path=path)
        response_data = json.dumps({u"status": u"success", u"result": isomorphic_decode(key)})
    elif action == b"purge":
        value = stash.take(key=key, path=path)
        return False
    elif action == b"check-scheme":
        scheme = urlsplit(request.url).scheme
        if scheme in [u"https", u"wss"]:
            stash.take(key=key, path=path)
            stash.put(key=key, value=u"upgraded", path=path)
        return False
    elif action == b"take":
        value = stash.take(key=key, path=path)
        if value == u"upgraded":
            status = u"upgraded"
        elif value is None:
            status = u"allowed"
        else:
            status = u"blocked"
        response_data = json.dumps({u"status": status, u"result": value})
    else:
        return False

    response.add_required_headers = False
    response.writer.write_status(200)
    response.writer.write_header(b"content-type", b"text/javascript")
    response.writer.write_header(b"cache-control", b"no-cache; must-revalidate")
    response.writer.end_headers()
    response.writer.write(response_data)
    return True


def __noop(request, response):
    return u""


def respond(request,
            response,
            status_code = 200,
            content_type = b"text/html",
            payload_generator = __noop,
            cache_control = b"no-cache; must-revalidate",
            access_control_allow_origin = b"*",
            maybe_additional_headers = None):
    if preprocess_redirection(request, response):
        return

    if preprocess_stash_action(request, response):
        return

    response.add_required_headers = False
    response.writer.write_status(status_code)

    if access_control_allow_origin != None:
        response.writer.write_header(b"access-control-allow-origin",
                                     access_control_allow_origin)
    response.writer.write_header(b"content-type", content_type)
    response.writer.write_header(b"cache-control", cache_control)

    additional_headers = maybe_additional_headers or {}
    for header, value in additional_headers.items():
        response.writer.write_header(header, value)

    response.writer.end_headers()

    new_headers = {}
    new_val = []
    for key, val in request.headers.items():
        if len(val) == 1:
            new_val = isomorphic_decode(val[0])
        else:
            new_val = [isomorphic_decode(x) for x in val]
        new_headers[isomorphic_decode(key)] = new_val

    server_data = {u"headers": json.dumps(new_headers, indent = 4)}

    payload = payload_generator(server_data)
    response.writer.write(payload)
