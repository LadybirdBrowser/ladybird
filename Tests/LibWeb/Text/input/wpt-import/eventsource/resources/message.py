import time

def main(request, response):
    mime = request.GET.first(b"mime", b"text/event-stream")
    message = request.GET.first(b"message", b"data: data");
    newline = b"" if request.GET.first(b"newline", None) == b"none" else b"\n\n";
    sleep = int(request.GET.first(b"sleep", b"0"))

    headers = [(b"Content-Type", mime)]
    body = message + newline + b"\n"
    if sleep != 0:
        time.sleep(sleep/1000)

    return headers, body
