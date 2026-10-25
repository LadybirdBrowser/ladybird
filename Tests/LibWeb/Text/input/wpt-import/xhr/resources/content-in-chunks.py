import time


def main(request, response):
    """Streams each hex-encoded `chunk` parameter as its own chunk of a chunked
    response, pausing `ms` milliseconds in between so that consumers observe one
    network callback per chunk. `type` sets the Content-Type header."""
    content_type = request.GET.first(b"type", b"text/plain")
    delay = float(request.GET.first(b"ms", b"100")) / 1E3
    chunks = [bytes.fromhex(chunk.decode(u"ascii")) for chunk in request.GET.get_list(b"chunk")]

    response.headers.set(b"Content-Type", content_type)
    response.headers.set(b"Transfer-Encoding", b"chunked")
    response.write_status_headers()

    for chunk in chunks:
        time.sleep(delay)
        response.writer.write(b"%x\r\n" % len(chunk))
        response.writer.write(chunk)
        response.writer.write(b"\r\n")
    response.writer.write(b"0\r\n\r\n")
