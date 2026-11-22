# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause


def main(request, response):
    response.headers.set(b"Access-Control-Allow-Origin", b"*")
    if request.method == "OPTIONS":
        response.headers.set(b"Access-Control-Allow-Headers", b"last-event-id")
        return b""

    token = request.GET.first(b"token")
    connection = (request.server.stash.take(token) or 0) + 1
    request.server.stash.put(token, connection)

    last_event_id = request.headers.get(b"Last-Event-ID", b"none").decode()
    response.headers.set(b"Content-Type", b"text/event-stream")
    response.headers.set(b"Cache-Control", b"no-store")

    # The first stream sets the last event ID, and the second resets it with a bare "id" field.
    if connection == 1:
        return b"retry: 0\nid: 1\ndata: set the ID\n\n"
    if connection == 2:
        return f"retry: 0\nid\ndata: reset the ID, Last-Event-ID was {last_event_id}\n\n".encode()
    return f"retry: 100000\ndata: Last-Event-ID is {last_event_id}\n\n".encode()
