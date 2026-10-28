from wptserve.utils import isomorphic_encode

# Outputs the request body, with controls and non-ASCII bytes escaped
# (b"\n" becomes b"\\x0a"), and with backslashes doubled.
# As a convenience, CRLF newlines are left as is.

def escape_byte(byte):
    # Convert int byte into a single-char binary string.
    byte = bytes([byte])
    if b"\0" <= byte <= b"\x1F" or byte >= b"\x7F":
        return b"\\x%02x" % ord(byte)
    if byte == b"\\":
        return b"\\\\"
    return byte

def main(request, response):

    headers = [(b"X-Request-Method", isomorphic_encode(request.method)),
               (b"X-Request-Content-Length", request.headers.get(b"Content-Length", b"NO")),
               (b"X-Request-Content-Type", request.headers.get(b"Content-Type", b"NO")),
               # Avoid any kind of content sniffing on the response.
               (b"Content-Type", b"text/plain; charset=UTF-8")]

    content = b"".join(map(escape_byte, request.body)).replace(b"\\x0d\\x0a", b"\r\n")

    return headers, content
