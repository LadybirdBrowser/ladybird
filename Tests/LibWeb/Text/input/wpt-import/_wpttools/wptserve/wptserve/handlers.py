# mypy: allow-untyped-defs

import json
import os
import pathlib
from collections import defaultdict

from urllib.parse import quote, unquote, urljoin

from .constants import content_types
from .pipes import Pipeline, template
from .ranges import RangeParser
from .request import Authentication
from .response import MultipartContent
from .utils import HTTPException

from html import escape

__all__ = ["file_handler", "python_script_handler",
           "FunctionHandler", "handler", "json_handler",
           "as_is_handler", "ErrorHandler", "BasicAuthHandler"]


def guess_content_type(path):
    ext = os.path.splitext(path)[1].lstrip(".")
    if ext in content_types:
        return content_types[ext]

    return "application/octet-stream"


def filesystem_path(base_path, request, url_base="/"):
    if base_path is None:
        base_path = request.doc_root

    path = unquote(request.url_parts.path)

    if path.startswith(url_base):
        path = path[len(url_base):]

    if ".." in path:
        raise HTTPException(404)

    new_path = os.path.join(base_path, path)

    # Otherwise setting path to / allows access outside the root directory
    if not new_path.startswith(base_path):
        raise HTTPException(404)

    return new_path


class DirectoryHandler:
    def __init__(self, base_path=None, url_base="/"):
        self.base_path = base_path
        self.url_base = url_base

    def __repr__(self):
        return "<%s base_path:%s url_base:%s>" % (self.__class__.__name__, self.base_path, self.url_base)

    def __call__(self, request, response):
        url_path = request.url_parts.path

        if not url_path.endswith("/"):
            response.status = 301
            response.headers = [("Location", "%s/" % request.url)]
            return

        path = filesystem_path(self.base_path, request, self.url_base)

        assert os.path.isdir(path)

        response.headers = [("Content-Type", "text/html")]
        response.content = """<!doctype html>
<meta name="viewport" content="width=device-width">
<title>Directory listing for %(path)s</title>
<h1>Directory listing for %(path)s</h1>
<ul>
%(items)s
</ul>
""" % {"path": escape(url_path),
       "items": "\n".join(self.list_items(url_path, path))}  # noqa: E122

    def list_items(self, base_path, path):
        assert base_path.endswith("/")

        # TODO: this won't actually list all routes, only the
        # ones that correspond to a real filesystem path. It's
        # not possible to list every route that will match
        # something, but it should be possible to at least list the
        # statically defined ones

        if base_path != "/":
            link = urljoin(base_path, "..")
            yield ("""<li class="dir"><a href="%(link)s">%(name)s</a></li>""" %
                   {"link": link, "name": ".."})
        items = []
        prev_item = None
        # This ensures that .headers always sorts after the file it provides the headers for. E.g.,
        # if we have x, x-y, and x.headers, the order will be x, x.headers, and then x-y.
        for item in sorted(os.listdir(path), key=lambda x: (x[:-len(".headers")], x) if x.endswith(".headers") else (x, x)):
            if prev_item and prev_item + ".headers" == item:
                items[-1][1] = item
                prev_item = None
                continue
            items.append([item, None])
            prev_item = item
        for item, dot_headers in items:
            link = escape(quote(item))
            dot_headers_markup = ""
            if dot_headers is not None:
                dot_headers_markup = (""" (<a href="%(link)s">.headers</a>)""" %
                                      {"link": escape(quote(dot_headers))})
            if os.path.isdir(os.path.join(path, item)):
                link += "/"
                class_ = "dir"
            else:
                class_ = "file"
            yield ("""<li class="%(class)s"><a href="%(link)s">%(name)s</a>%(headers)s</li>""" %
                   {"link": link, "name": escape(item), "class": class_,
                    "headers": dot_headers_markup})


def parse_qs(qs):
    """Parse a query string given as a string argument (data of type
    application/x-www-form-urlencoded). Data are returned as a dictionary. The
    dictionary keys are the unique query variable names and the values are
    lists of values for each name.

    This implementation is used instead of Python's built-in `parse_qs` method
    in order to support the semicolon character (which the built-in method
    interprets as a parameter delimiter)."""
    pairs = [item.split("=", 1) for item in qs.split('&') if item]
    rv = defaultdict(list)
    for pair in pairs:
        if len(pair) == 1 or len(pair[1]) == 0:
            continue
        name = unquote(pair[0].replace('+', ' '))
        value = unquote(pair[1].replace('+', ' '))
        rv[name].append(value)
    return dict(rv)


def wrap_pipeline(path, request, response):
    """Applies pipelines to a response.

    Pipelines are specified in the filename (.sub.) or the query param (?pipe).
    """
    query = parse_qs(request.url_parts.query)
    pipe_string = ""

    if ".sub." in path:
        ml_extensions = {".html", ".htm", ".xht", ".xhtml", ".xml", ".svg"}
        escape_type = "html" if os.path.splitext(path)[1] in ml_extensions else "none"
        pipe_string = "sub(%s)" % escape_type

    if "pipe" in query:
        if pipe_string:
            pipe_string += "|"

        pipe_string += query["pipe"][-1]

    if pipe_string:
        response = Pipeline(pipe_string)(request, response)

    return response


def load_headers(request, path):
    """Loads headers from files for a given path.

    Attempts to load both the neighbouring __dir__{.sub}.headers and
    PATH{.sub}.headers (applying template substitution if needed); results are
    concatenated in that order.
    """
    def _load(request, path):
        headers_path = path + ".sub.headers"
        if os.path.exists(headers_path):
            use_sub = True
        else:
            headers_path = path + ".headers"
            use_sub = False

        try:
            with open(headers_path, "rb") as headers_file:
                data = headers_file.read()
        except OSError:
            return []
        else:
            if use_sub:
                data = template(request, data, escape_type="none")
            return [tuple(item.strip() for item in line.split(b":", 1))
                    for line in data.splitlines() if line]

    return (_load(request, os.path.join(os.path.dirname(path), "__dir__")) +
            _load(request, path))


class FileHandler:
    def __init__(self, base_path=None, url_base="/"):
        self.base_path = base_path
        self.url_base = url_base
        self.directory_handler = DirectoryHandler(self.base_path, self.url_base)

    def __repr__(self):
        return "<%s base_path:%s url_base:%s>" % (self.__class__.__name__, self.base_path, self.url_base)

    def __call__(self, request, response):
        path = filesystem_path(self.base_path, request, self.url_base)

        if os.path.isdir(path):
            return self.directory_handler(request, response)
        try:
            #This is probably racy with some other process trying to change the file
            file_size = os.stat(path).st_size
            response.headers.update(self.get_headers(request, path))
            if "Range" in request.headers:
                try:
                    byte_ranges = RangeParser()(request.headers['Range'], file_size)
                except HTTPException as e:
                    if e.code == 416:
                        response.headers.set("Content-Range", "bytes */%i" % file_size)
                        raise
            else:
                byte_ranges = None
            data = self.get_data(response, path, byte_ranges)
            response.content = data
            response = wrap_pipeline(path, request, response)
            return response

        except OSError:
            raise HTTPException(404)

    def get_headers(self, request, path):
        rv = load_headers(request, path)

        if not any(key.lower() == b"content-type" for (key, _) in rv):
            rv.insert(0, (b"Content-Type", guess_content_type(path).encode("ascii")))

        return rv

    def get_data(self, response, path, byte_ranges):
        """Return either the handle to a file, or a string containing
        the content of a chunk of the file, if we have a range request."""
        if byte_ranges is None:
            return open(path, 'rb')
        else:
            with open(path, 'rb') as f:
                response.status = 206
                if len(byte_ranges) > 1:
                    parts_content_type, content = self.set_response_multipart(response,
                                                                              byte_ranges,
                                                                              f)
                    for byte_range in byte_ranges:
                        content.append_part(self.get_range_data(f, byte_range),
                                            parts_content_type,
                                            [("Content-Range", byte_range.header_value())])
                    return content
                else:
                    response.headers.set("Content-Range", byte_ranges[0].header_value())
                    return self.get_range_data(f, byte_ranges[0])

    def set_response_multipart(self, response, ranges, f):
        parts_content_type = response.headers.get("Content-Type")
        if parts_content_type:
            parts_content_type = parts_content_type[-1]
        else:
            parts_content_type = None
        content = MultipartContent()
        response.headers.set("Content-Type", "multipart/byteranges; boundary=%s" % content.boundary)
        return parts_content_type, content

    def get_range_data(self, f, byte_range):
        f.seek(byte_range.lower)
        return f.read(byte_range.upper - byte_range.lower)


file_handler = FileHandler()  # type: ignore


class PythonScriptHandler:
    def __init__(self, base_path=None, url_base="/"):
        self.base_path = base_path
        self.url_base = url_base

    def __repr__(self):
        return "<%s base_path:%s url_base:%s>" % (self.__class__.__name__, self.base_path, self.url_base)

    def _load_file(self, request, response, func):
        """
        This loads the requested python file as an environ variable.

        If the requested file is a directory, this instead loads the first
        lexicographically sorted file found in that directory that matches
        "default*.py".

        Once the environ is loaded, the passed `func` is run with this loaded environ.

        :param request: The request object
        :param response: The response object
        :param func: The function to be run with the loaded environ with the modified filepath. Signature: (request, response, environ, path)
        :return: The return of func
        """
        path = filesystem_path(self.base_path, request, self.url_base)

        # Find a default Python file if the specified path is a directory
        if os.path.isdir(path):
            default_py_files = sorted(list(filter(
                pathlib.Path.is_file,
                pathlib.Path(path).glob("default*.py"))))
            if default_py_files:
                path = str(default_py_files[0])

        try:
            environ = {"__file__": path}
            with open(path, 'rb') as f:
                exec(compile(f.read(), path, 'exec'), environ, environ)

            if func is not None:
                return func(request, response, environ, path)

        except OSError:
            raise HTTPException(404)

    def __call__(self, request, response):
        def func(request, response, environ, path):
            if "main" in environ:
                handler = FunctionHandler(environ["main"])
                handler(request, response)
                wrap_pipeline(path, request, response)
            else:
                raise HTTPException(500, "No main function in script %s" % path)

        self._load_file(request, response, func)

    def frame_handler(self, request):
        """
        This creates a FunctionHandler with one or more of the handling functions.

        Used by the H2 server.

        :param request: The request object used to generate the handler.
        :return: A FunctionHandler object with one or more of these functions: `handle_headers`, `handle_data` or `main`
        """
        def func(request, response, environ, path):
            def _main(req, resp):
                pass

            handler = FunctionHandler(_main)
            if "main" in environ:
                handler.func = environ["main"]
            if "handle_headers" in environ:
                handler.handle_headers = environ["handle_headers"]
            if "handle_data" in environ:
                handler.handle_data = environ["handle_data"]

            if handler.func is _main and not hasattr(handler, "handle_headers") and not hasattr(handler, "handle_data"):
                raise HTTPException(500, "No main function or handlers in script %s" % path)

            return handler
        return self._load_file(request, None, func)


python_script_handler = PythonScriptHandler()  # type: ignore


class FunctionHandler:
    def __init__(self, func):
        self.func = func

    def __call__(self, request, response):
        try:
            rv = self.func(request, response)
        except HTTPException:
            raise
        except Exception as e:
            raise HTTPException(500) from e
        if rv is not None:
            if isinstance(rv, tuple):
                if len(rv) == 3:
                    status, headers, content = rv
                    response.status = status
                elif len(rv) == 2:
                    headers, content = rv
                else:
                    raise HTTPException(500)
                response.headers.update(headers)
            else:
                content = rv
            response.content = content
            wrap_pipeline('', request, response)


# The generic name here is so that this can be used as a decorator
def handler(func):
    return FunctionHandler(func)


class JsonHandler:
    def __init__(self, func):
        self.func = func

    def __call__(self, request, response):
        return FunctionHandler(self.handle_request)(request, response)

    def handle_request(self, request, response):
        rv = self.func(request, response)
        response.headers.set("Content-Type", "application/json")
        enc = json.dumps
        if isinstance(rv, tuple):
            rv = list(rv)
            value = tuple(rv[:-1] + [enc(rv[-1])])
            length = len(value[-1])
        else:
            value = enc(rv)
            length = len(value)
        response.headers.set("Content-Length", length)
        return value


def json_handler(func):
    return JsonHandler(func)


class AsIsHandler:
    def __init__(self, base_path=None, url_base="/"):
        self.base_path = base_path
        self.url_base = url_base

    def __call__(self, request, response):
        path = filesystem_path(self.base_path, request, self.url_base)
        if os.path.isdir(path):
            raise HTTPException(
                500, "AsIsHandler cannot process directory, %s" % path)

        try:
            with open(path, 'rb') as f:
                response.writer.write_raw_content(f.read())
            wrap_pipeline(path, request, response)
            response.close_connection = True
        except OSError:
            raise HTTPException(404)


as_is_handler = AsIsHandler()  # type: ignore


class BasicAuthHandler:
    def __init__(self, handler, user, password):
        """
         A Basic Auth handler

         :Args:
         - handler: a secondary handler for the request after authentication is successful (example file_handler)
         - user: string of the valid user name or None if any / all credentials are allowed
         - password: string of the password required
        """
        self.user = user
        self.password = password
        self.handler = handler

    def __call__(self, request, response):
        if "authorization" not in request.headers:
            response.status = 401
            response.headers.set("WWW-Authenticate", "Basic")
            return response
        else:
            auth = Authentication(request.headers)
            if self.user is not None and (self.user != auth.username or self.password != auth.password):
                response.set_error(403, "Invalid username or password")
                return response
            return self.handler(request, response)


basic_auth_handler = BasicAuthHandler(file_handler, None, None)  # type: ignore


class ErrorHandler:
    def __init__(self, status):
        self.status = status

    def __call__(self, request, response):
        response.set_error(self.status)


class StringHandler:
    def __init__(self, data, content_type, **headers):
        """Handler that returns a fixed data string and headers

        :param data: String to use
        :param content_type: Content type header to server the response with
        :param headers: List of headers to send with responses"""

        self.data = data

        self.resp_headers = [("Content-Type", content_type)]
        for k, v in headers.items():
            self.resp_headers.append((k.replace("_", "-"), v))

        self.handler = handler(self.handle_request)

    def handle_request(self, request, response):
        return self.resp_headers, self.data

    def __call__(self, request, response):
        rv = self.handler(request, response)
        return rv


class StaticHandler:
    def __init__(self, path, format_args, content_type, **headers):
        """Handler that reads a file from a path and substitutes some fixed data

        Note that *.headers files have no effect in this handler.

        :param path: Path(s) to template files to use. If a sequence of paths is provided instead
            of a single path, the contents of each file will be concatenated together before the
            `format_args` are interpolated.
        :param format_args: Dictionary of values to substitute into the template file
        :param content_type: Content type header to server the response with
        :param headers: List of headers to send with responses"""
        self._paths = [path] if isinstance(path, str) else path
        self._format_args = format_args
        self._content_type = content_type
        self._headers = headers
        self._handler = None

    def __getnewargs_ex__(self):
        # Do not pickle `self._handler`, which can be arbitrarily large.
        args = self._paths, self._format_args, self._content_type
        return args, self._headers

    def __call__(self, request, response):
        # Load the static file contents lazily so that this handler can be
        # pickled and sent to child processes efficiently. Transporting file
        # contents across processes can slow `wptserve` startup by several
        # seconds (crbug.com/1479850).
        if not self._handler:
            data = ""
            for path in self._paths:
                with open(path) as f:
                    data += f.read()
            if self._format_args:
                data = data % self._format_args
            self._handler = StringHandler(data, self._content_type, **self._headers)
        return self._handler(request, response)
