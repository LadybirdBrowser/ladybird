def main(request, response):
  if b"clear-vary-value-override-cookie" in request.GET:
    response.delete_cookie(b"vary-value-override")
    return b"vary cookie cleared"

  set_cookie_vary = request.GET.first(b"set-vary-value-override-cookie",
                                      default=b"")
  if set_cookie_vary:
    response.set_cookie(b"vary-value-override", set_cookie_vary)
    return b"vary cookie set"

  # If there is a vary-value-override cookie set, then use its value
  # for the VARY header no matter what the query string is set to.  This
  # override is necessary to test the case when two URLs are identical
  # (including query), but differ by VARY header.
  cookie_vary = request.cookies.get(b"vary-value-override")
  if cookie_vary:
    response.headers.set(b"vary", cookie_vary.value)
  else:
    # If there is no cookie, then use the query string value, if present.
    query_vary = request.GET.first(b"vary", default=b"")
    if query_vary:
      response.headers.set(b"vary", query_vary)

  return b"vary response"
