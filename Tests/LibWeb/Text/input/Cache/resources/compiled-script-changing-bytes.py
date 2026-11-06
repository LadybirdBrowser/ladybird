# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause


def main(request, response):
    token = request.GET.first(b"token")
    load_count = (request.server.stash.take(token) or 0) + 1
    request.server.stash.put(token, load_count)

    response.headers.set(b"Content-Type", b"text/javascript")
    response.headers.set(b"Cache-Control", b"no-cache")
    return f"globalThis.value = function () {{ return {load_count}; }};".encode()
