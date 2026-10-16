def main(request, response):
    headers = [(b"Content-Type", b"text/html"),
               (b"Cross-Origin-Resource-Policy", request.GET[b'corp'])]
    return 200, headers, b"<body><h3>The iframe</h3><script>window.onmessage = () => { parent.postMessage('pong', '*'); }</script></body>"

