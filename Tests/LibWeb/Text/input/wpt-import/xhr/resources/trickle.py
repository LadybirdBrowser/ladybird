import time

def main(request, response):
    chunk = b"TEST_TRICKLE\n"
    delay = float(request.GET.first(b"ms", 500)) / 1E3
    count = int(request.GET.first(b"count", 50))
    if b"specifylength" in request.GET:
        response.headers.set(b"Content-Length", count * len(chunk))
    time.sleep(delay)
    response.headers.set(b"Content-type", b"text/plain")
    response.write_status_headers()
    time.sleep(delay)
    for i in range(count):
        response.writer.write_content(chunk)
        time.sleep(delay)
