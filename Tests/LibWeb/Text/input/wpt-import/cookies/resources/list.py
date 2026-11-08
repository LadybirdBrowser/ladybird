import json
from cookies.resources import helpers

from wptserve.utils import isomorphic_decode

def main(request, response):
    headers = helpers.setNoCacheAndCORSHeaders(request, response)
    cookies = helpers.readCookies(request)
    decoded_cookies = {isomorphic_decode(key): isomorphic_decode(val) for key, val in cookies.items()}
    return headers, json.dumps(decoded_cookies)
