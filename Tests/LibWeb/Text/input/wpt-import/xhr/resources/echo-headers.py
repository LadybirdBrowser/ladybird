def main(request, response):
    content = str(request.raw_headers)
    response.writer.write_status(200)
    response.writer.write_header(b"Content-Type", b"text/plain")
    response.writer.write_header(b"Content-Length", str(len(content)))
    response.writer.write_header(b"Connection", b"close")
    response.writer.end_headers()
    response.writer.write(content)
    response.close_connection = True
