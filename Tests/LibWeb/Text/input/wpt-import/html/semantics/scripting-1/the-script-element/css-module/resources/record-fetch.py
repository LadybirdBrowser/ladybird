def main(request, response):
    try:
        stash_key = request.GET.first(b"key")
        action = request.GET.first(b"action")

        run_count = request.server.stash.take(stash_key)
        if not run_count:
            run_count = 0

        if action == b"incCount":
            request.server.stash.put(stash_key, run_count + 1)
            response.headers.set(b"Content-Type", b"text/css")
            response.content = b'#test { background-color: #FF0000; }'
        elif action == b"getCount":
            response.headers.set(b"Content-Type", b"text/json")
            response.content = b'{"count": %d }' % run_count
        else:
            response.set_error(400, u"Invalid action")
    except:
        response.set_error(400, u"Not enough parameters")
