# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause


def main(request, response):
    token = request.GET.first(b"token")
    response.headers.set(b"Access-Control-Allow-Origin", b"*")

    if b"unblock" in request.GET:
        request.server.stash.take(token).put(True)
        response.headers.set(b"Cache-Control", b"no-store")
        return b""

    response.headers.set(b"Content-Type", b"application/octet-stream")
    response.headers.set(b"X-Content-Type-Options", b"nosniff")
    response.headers.set(b"Content-Length", b"64")

    marker = request.headers.get(b"Accept", b"")
    if marker in (b"stored", b"check"):
        response.headers.set(b"Cache-Control", b"max-age=600")
        return marker.ljust(64, b"K")

    # A leaked pending entry must remain stale even if the replacement body finalizes it.
    response.headers.set(b"Cache-Control", b"max-age=0")
    release = request.server.stash.get_queue()
    request.server.stash.put(token, release)
    response.write_status_headers()
    response.writer.write_content(b"abcdefghij")
    release.get(timeout=120)
    response.writer.write_content(b"K" * 54)
