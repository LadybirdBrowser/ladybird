import json
from cookies.resources import helpers

from wptserve.utils import isomorphic_decode

def main(request, response):
    headers = helpers.setNoCacheAndCORSHeaders(request, response)
    cookies = helpers.readCookies(request)
    headers.append((b"Content-Type", b"text/html; charset=utf-8"))

    tmpl = u"""
<!DOCTYPE html>
<script>
  var data = %s;
  data.type = "COOKIES";

  try {
    data.domcookies = document.cookie;
  } catch (e) {}

  if (window.parent != window) {
    window.parent.postMessage(data, "*");
    if (window.top != window.parent)
      window.top.postMessage(data, "*");
  }


  if (window.opener)
    window.opener.postMessage(data, "*");

  window.addEventListener("message", e => {
    console.log(e);
    if (e.data == "reload")
      window.location.reload();
  });
</script>
"""
    decoded_cookies = {isomorphic_decode(key): isomorphic_decode(val) for key, val in cookies.items()}
    return headers, tmpl % json.dumps(decoded_cookies)
