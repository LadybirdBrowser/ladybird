import time

def main(request, response):
    delay = float(request.GET.first(b"ms", 500))
    time.sleep(delay / 1E3)

    return [(b"Access-Control-Allow-Origin", b"*"), (b"Access-Control-Allow-Methods", b"YO"), (b"Content-type", b"text/plain")], b"TEST_DELAY"
