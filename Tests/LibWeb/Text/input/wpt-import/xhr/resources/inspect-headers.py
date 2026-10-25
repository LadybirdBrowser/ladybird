from wptserve.utils import isomorphic_encode

def get_response(raw_headers, filter_value, filter_name):
    result = b""
    # raw_headers.raw_items() returns the (name, value) header pairs as
    # tuples of strings. Convert them to bytes before comparing.
    # TODO: Get access to the raw headers, so that whitespace between
    # name, ":" and value can also be checked:
    # https://github.com/web-platform-tests/wpt/issues/28756
    for field in raw_headers.raw_items():
        name = isomorphic_encode(field[0])
        value = isomorphic_encode(field[1])
        if filter_value:
            if value == filter_value:
                result += name + b","
        elif name.lower() == filter_name:
            result += name + b": " + value + b"\n"
    return result

def main(request, response):
    headers = []
    if b"cors" in request.GET:
        headers.append((b"Access-Control-Allow-Origin", b"*"))
        headers.append((b"Access-Control-Allow-Credentials", b"true"))
        headers.append((b"Access-Control-Allow-Methods", b"GET, POST, PUT, FOO"))
        headers.append((b"Access-Control-Allow-Headers", b"x-test, x-foo"))
        headers.append((
            b"Access-Control-Expose-Headers",
            b"x-request-method, x-request-content-type, x-request-query, x-request-content-length"))
    headers.append((b"content-type", b"text/plain"))

    filter_value = request.GET.first(b"filter_value", b"")
    filter_name = request.GET.first(b"filter_name", b"").lower()
    result = get_response(request.raw_headers, filter_value, filter_name)

    return headers, result
