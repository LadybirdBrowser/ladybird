# mypy: allow-untyped-defs

from collections import deque
import base64
import gzip as gzip_module
import hashlib
import os
import re
import time
import uuid

from html import escape
from io import BytesIO
from typing import Any, Callable, ClassVar, Dict, Optional, TypeVar

T = TypeVar('T')


def resolve_content(response):
    return b"".join(item for item in response.iter_content(read_file=True))


class Pipeline:
    pipes: ClassVar[Dict[str, Callable[..., Any]]] = {}

    def __init__(self, pipe_string):
        self.pipe_functions = self.parse(pipe_string)

    def parse(self, pipe_string):
        functions = []
        for item in PipeTokenizer().tokenize(pipe_string):
            if not item:
                break
            if item[0] == "function":
                functions.append((self.pipes[item[1]], []))
            elif item[0] == "argument":
                functions[-1][1].append(item[1])
        return functions

    def __call__(self, request, response):
        for func, args in self.pipe_functions:
            response = func(request, response, *args)
        return response


class PipeTokenizer:
    def __init__(self):
        #This whole class can likely be replaced by some regexps
        self.state = None

    def tokenize(self, string):
        self.string = string
        self.state = self.func_name_state
        self._index = 0
        while self.state:
            yield self.state()
        yield None

    def get_char(self):
        if self._index >= len(self.string):
            return None
        rv = self.string[self._index]
        self._index += 1
        return rv

    def func_name_state(self):
        rv = ""
        while True:
            char = self.get_char()
            if char is None:
                self.state = None
                if rv:
                    return ("function", rv)
                else:
                    return None
            elif char == "(":
                self.state = self.argument_state
                return ("function", rv)
            elif char == "|":
                if rv:
                    return ("function", rv)
            else:
                rv += char

    def argument_state(self):
        rv = ""
        while True:
            char = self.get_char()
            if char is None:
                self.state = None
                return ("argument", rv)
            elif char == "\\":
                rv += self.get_escape()
                if rv is None:
                    #This should perhaps be an error instead
                    return ("argument", rv)
            elif char == ",":
                return ("argument", rv)
            elif char == ")":
                self.state = self.func_name_state
                return ("argument", rv)
            else:
                rv += char

    def get_escape(self):
        char = self.get_char()
        escapes = {"n": "\n",
                   "r": "\r",
                   "t": "\t"}
        return escapes.get(char, char)


class pipe:
    def __init__(self, *arg_converters: Callable[[str], Any]):
        self.arg_converters = arg_converters
        self.max_args = len(self.arg_converters)
        self.min_args = 0
        opt_seen = False
        for item in self.arg_converters:
            if not opt_seen:
                if isinstance(item, opt):
                    opt_seen = True
                else:
                    self.min_args += 1
            else:
                if not isinstance(item, opt):
                    raise ValueError("Non-optional argument cannot follow optional argument")

    def __call__(self, f):
        def inner(request, response, *args):
            if not (self.min_args <= len(args) <= self.max_args):
                raise ValueError("Expected between %d and %d args, got %d" %
                                 (self.min_args, self.max_args, len(args)))
            arg_values = tuple(f(x) for f, x in zip(self.arg_converters, args))
            return f(request, response, *arg_values)
        Pipeline.pipes[f.__name__] = inner
        #We actually want the undecorated function in the main namespace
        return f


class opt:
    def __init__(self, f: Callable[[str], Any]):
        self.f = f

    def __call__(self, arg: str) -> Any:
        return self.f(arg)


def nullable(func: Callable[[str], T]) -> Callable[[str], Optional[T]]:
    def inner(arg: str) -> Optional[T]:
        if arg.lower() == "null":
            return None
        else:
            return func(arg)
    return inner


def boolean(arg: str) -> bool:
    if arg.lower() in ("true", "1"):
        return True
    elif arg.lower() in ("false", "0"):
        return False
    raise ValueError


@pipe(int)
def status(request, response, code):
    """Alter the status code.

    :param code: Status code to use for the response."""
    response.status = code
    return response


@pipe(str, str, opt(boolean))
def header(request, response, name, value, append=False):
    """Set a HTTP header.

    Replaces any existing HTTP header of the same name unless
    append is set, in which case the header is appended without
    replacement.

    :param name: Name of the header to set.
    :param value: Value to use for the header.
    :param append: True if existing headers should not be replaced
    """
    if not append:
        response.headers.set(name, value)
    else:
        response.headers.append(name, value)
    return response


@pipe(str)
def trickle(request, response, delays):
    """Send the response in parts, with time delays.

    :param delays: A string of delays and amounts, in bytes, of the
                   response to send. Each component is separated by
                   a colon. Amounts in bytes are plain integers, whilst
                   delays are floats prefixed with a single d e.g.
                   d1:100:d2
                   Would cause a 1 second delay, would then send 100 bytes
                   of the file, and then cause a 2 second delay, before sending
                   the remainder of the file.

                   If the last token is of the form rN, instead of sending the
                   remainder of the file, the previous N instructions will be
                   repeated until the whole file has been sent e.g.
                   d1:100:d2:r2
                   Causes a delay of 1s, then 100 bytes to be sent, then a 2s delay
                   and then a further 100 bytes followed by a two second delay
                   until the response has been fully sent.
                   """
    def parse_delays():
        parts = delays.split(":")
        rv = []
        for item in parts:
            if item.startswith("d"):
                item_type = "delay"
                item = item[1:]
                value = float(item)
            elif item.startswith("r"):
                item_type = "repeat"
                value = int(item[1:])
                if not value % 2 == 0:
                    raise ValueError
            else:
                item_type = "bytes"
                value = int(item)
            if len(rv) and rv[-1][0] == item_type:
                rv[-1][1] += value
            else:
                rv.append((item_type, value))
        return rv

    delays = parse_delays()
    if not delays:
        return response
    content = resolve_content(response)
    offset = [0]

    if not ("Cache-Control" in response.headers or
            "Pragma" in response.headers or
            "Expires" in response.headers):
        response.headers.set("Cache-Control", "no-cache, no-store, must-revalidate")
        response.headers.set("Pragma", "no-cache")
        response.headers.set("Expires", "0")

    def add_content(delays, repeat=False):
        for i, (item_type, value) in enumerate(delays):
            if item_type == "bytes":
                yield content[offset[0]:offset[0] + value]
                offset[0] += value
            elif item_type == "delay":
                time.sleep(value)
            elif item_type == "repeat":
                if i != len(delays) - 1:
                    continue
                while offset[0] < len(content):
                    yield from add_content(delays[-(value + 1):-1], True)

        if not repeat and offset[0] < len(content):
            yield content[offset[0]:]

    response.content = add_content(delays)
    return response


@pipe(nullable(int), opt(nullable(int)))
def slice(request, response, start, end=None):
    """Send a byte range of the response body

    :param start: The starting offset. Follows python semantics including
                  negative numbers.

    :param end: The ending offset, again with python semantics and None
                (spelled "null" in a query string) to indicate the end of
                the file.
    """
    content = resolve_content(response)[start:end]
    response.content = content
    response.headers.set("Content-Length", len(content))
    return response


class ReplacementTokenizer:
    def arguments(self, token):
        unwrapped = token[1:-1].decode('utf8')
        return ("arguments", re.split(r",\s*", unwrapped) if unwrapped else [])

    def ident(self, token):
        return ("ident", token.decode('utf8'))

    def index(self, token):
        token = token[1:-1].decode('utf8')
        try:
            index = int(token)
        except ValueError:
            index = token
        return ("index", index)

    def var(self, token):
        token = token[:-1].decode('utf8')
        return ("var", token)

    def tokenize(self, string):
        assert isinstance(string, bytes)
        return self.scanner.scan(string)[0]

    # re.Scanner is missing from typeshed:
    # https://github.com/python/typeshed/pull/3071
    scanner = re.Scanner([(br"\$\w+:", var),  # type: ignore
                          (br"\$?\w+", ident),
                          (br"\[[^\]]*\]", index),
                          (br"\([^)]*\)", arguments)])


class FirstWrapper:
    def __init__(self, params):
        self.params = params

    def __getitem__(self, key):
        try:
            if isinstance(key, str):
                key = key.encode('iso-8859-1')
            return self.params.first(key)
        except KeyError:
            return ""


@pipe(opt(nullable(str)))
def sub(request, response, escape_type="html"):
    """Substitute environment information about the server and request into the script.

    :param escape_type: String detailing the type of escaping to use. Known values are
                        "html" and "none", with "html" the default for historic reasons.

    The format is a very limited template language. Substitutions are
    enclosed by {{ and }}. There are several available substitutions:

    host
      A simple string value and represents the primary host from which the
      tests are being run.
    domains
      A dictionary of available domains indexed by subdomain name.
    ports
      A dictionary of lists of ports indexed by protocol.
    location
      A dictionary of parts of the request URL. Valid keys are
      'server, 'scheme', 'host', 'hostname', 'port', 'path' and 'query'.
      'server' is scheme://host:port, 'host' is hostname:port, and query
      includes the leading '?', but other delimiters are omitted.
    headers
      A dictionary of HTTP headers in the request.
    header_or_default(header, default)
      The value of an HTTP header, or a default value if it is absent.
      For example::

        {{header_or_default(X-Test, test-header-absent)}}

    GET
      A dictionary of query parameters supplied with the request.
    uuid()
      A pesudo-random UUID suitable for usage with stash
    file_hash(algorithm, filepath)
      The cryptographic hash of a file. Supported algorithms: md5, sha1,
      sha224, sha256, sha384, and sha512. For example::

        {{file_hash(md5, dom/interfaces.html)}}

    fs_path(filepath)
      The absolute path to a file inside the wpt document root

    So for example in a setup running on localhost with a www
    subdomain and a http server on ports 80 and 81::

      {{host}} => localhost
      {{domains[www]}} => www.localhost
      {{ports[http][1]}} => 81

    It is also possible to assign a value to a variable name, which must start
    with the $ character, using the ":" syntax e.g.::

      {{$id:uuid()}}

    Later substitutions in the same file may then refer to the variable
    by name e.g.::

      {{$id}}
    """
    content = resolve_content(response)

    new_content = template(request, content, escape_type=escape_type)

    response.content = new_content
    return response

class SubFunctions:
    @staticmethod
    def uuid(request):
        return str(uuid.uuid4())

    # Maintain a list of supported algorithms, restricted to those that are
    # available on all platforms [1]. This ensures that test authors do not
    # unknowingly introduce platform-specific tests.
    #
    # [1] https://docs.python.org/2/library/hashlib.html
    supported_algorithms = ("md5", "sha1", "sha224", "sha256", "sha384", "sha512")

    @staticmethod
    def file_hash(request, algorithm, path):
        assert isinstance(algorithm, str)
        if algorithm not in SubFunctions.supported_algorithms:
            raise ValueError("Unsupported encryption algorithm: '%s'" % algorithm)

        hash_obj = getattr(hashlib, algorithm)()
        absolute_path = os.path.join(request.doc_root, path)

        try:
            with open(absolute_path, "rb") as f:
                hash_obj.update(f.read())
        except OSError:
            # In this context, an unhandled IOError will be interpreted by the
            # server as an indication that the template file is non-existent.
            # Although the generic "Exception" is less precise, it avoids
            # triggering a potentially-confusing HTTP 404 error in cases where
            # the path to the file to be hashed is invalid.
            raise Exception('Cannot open file for hash computation: "%s"' % absolute_path)

        return base64.b64encode(hash_obj.digest()).strip()

    @staticmethod
    def fs_path(request, path):
        if not path.startswith("/"):
            subdir = request.request_path[len(request.url_base):]
            if "/" in subdir:
                subdir = subdir.rsplit("/", 1)[0]
            root_rel_path = subdir + "/" + path
        else:
            root_rel_path = path[1:]
        root_rel_path = root_rel_path.replace("/", os.path.sep)
        absolute_path = os.path.abspath(os.path.join(request.doc_root, root_rel_path))
        if ".." in os.path.relpath(absolute_path, request.doc_root):
            raise ValueError("Path outside wpt root")
        return absolute_path

    @staticmethod
    def header_or_default(request, name, default):
        return request.headers.get(name, default)

def template(request, content, escape_type="html"):
    #TODO: There basically isn't any error handling here
    tokenizer = ReplacementTokenizer()

    variables = {}

    def config_replacement(match):
        content, = match.groups()

        tokens = tokenizer.tokenize(content)
        tokens = deque(tokens)

        token_type, field = tokens.popleft()
        assert isinstance(field, str)

        if token_type == "var":
            variable = field
            token_type, field = tokens.popleft()
            assert isinstance(field, str)
        else:
            variable = None

        if token_type != "ident":
            raise Exception("unexpected token type %s (token '%r'), expected ident" % (token_type, field))

        if field in variables:
            value = variables[field]
        elif hasattr(SubFunctions, field):
            value = getattr(SubFunctions, field)
        elif field == "headers":
            value = request.headers
        elif field == "GET":
            value = FirstWrapper(request.GET)
        elif field == "hosts":
            value = request.server.config.all_domains
        elif field == "domains":
            value = request.server.config.all_domains[""]
        elif field == "host":
            value = request.server.config["browser_host"]
        elif field in request.server.config:
            value = request.server.config[field]
        elif field == "location":
            value = {"server": "%s://%s:%s" % (request.url_parts.scheme,
                                               request.url_parts.hostname,
                                               request.url_parts.port),
                     "scheme": request.url_parts.scheme,
                     "host": "%s:%s" % (request.url_parts.hostname,
                                        request.url_parts.port),
                     "hostname": request.url_parts.hostname,
                     "port": request.url_parts.port,
                     "path": request.url_parts.path,
                     "pathname": request.url_parts.path,
                     "query": "?%s" % request.url_parts.query}
        elif field == "url_base":
            value = request.url_base
        else:
            raise Exception("Undefined template variable %s" % field)

        while tokens:
            ttype, field = tokens.popleft()
            if ttype == "index":
                value = value[field]
            elif ttype == "arguments":
                value = value(request, *field)
            else:
                raise Exception(
                    "unexpected token type %s (token '%r'), expected ident or arguments" % (ttype, field)
                )

        assert isinstance(value, (int, (bytes, str))), tokens

        if variable is not None:
            variables[variable] = value

        escape_func = {"html": lambda x:escape(x, quote=True),
                       "none": lambda x:x}[escape_type]

        # Should possibly support escaping for other contexts e.g. script
        # TODO: read the encoding of the response
        # cgi.escape() only takes text strings in Python 3.
        if isinstance(value, bytes):
            value = value.decode("utf-8")
        elif isinstance(value, int):
            value = str(value)
        return escape_func(value).encode("utf-8")

    template_regexp = re.compile(br"{{([^}]*)}}")
    new_content = template_regexp.sub(config_replacement, content)

    return new_content

@pipe()
def gzip(request, response):
    """This pipe gzip-encodes response data.

    It sets (or overwrites) these HTTP headers:
    Content-Encoding is set to gzip
    Content-Length is set to the length of the compressed content
    """
    content = resolve_content(response)
    response.headers.set("Content-Encoding", "gzip")

    out = BytesIO()
    with gzip_module.GzipFile(fileobj=out, mode="w") as f:
        f.write(content)
    response.content = out.getvalue()

    response.headers.set("Content-Length", len(response.content))

    return response
