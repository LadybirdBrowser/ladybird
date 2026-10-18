def main(request, response):
    status = request.GET.first(b'status')
    response.status = (status, b"");
    if b'tao_value' in request.GET:
      response.headers.set(b'timing-allow-origin', request.GET.first(b'tao_value'))
    if b'allow_origin' in request.GET:
      response.headers.set(b'access-control-allow-origin', request.GET.first(b'allow_origin'))

