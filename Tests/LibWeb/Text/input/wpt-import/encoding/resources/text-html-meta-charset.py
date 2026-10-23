def main(request, response):
  response.headers.set(b"Content-Type", b"text/html")
  response.content = b"<meta charset=\"" + request.GET.first(b"label") + b"\">hello encoding"
