# mypy: allow-untyped-defs

import base64
import tempfile

from http.cookies import BaseCookie
from io import BytesIO
from typing import Dict, List, TypeVar
from urllib.parse import parse_qsl, urlsplit

from . import stash
from .cgi import FieldStorage
from .utils import HTTPException, isomorphic_encode, isomorphic_decode

KT = TypeVar('KT')
VT = TypeVar('VT')

missing = object()


class Server:
    """Data about the server environment

    .. attribute:: config

    Environment configuration information with information about the
    various servers running, their hostnames and ports.

    .. attribute:: stash

    Stash object holding state stored on the server between requests.

    """
    config = None

    def __init__(self, request):
        self._stash = None
        self._request = request

    @property
    def stash(self):
        if self._stash is None:
            address, authkey = stash.load_env_config()
            self._stash = stash.Stash(self._request.url_parts.path, address, authkey)
        return self._stash


class InputFile:
    max_buffer_size = 1024*1024

    def __init__(self, rfile, length):
        """File-like object used to provide a seekable view of request body data"""
        self._file = rfile
        self.length = length

        self._file_position = 0

        if length > self.max_buffer_size:
            self._buf = tempfile.TemporaryFile()
        else:
            self._buf = BytesIO()

    def close(self):
        self._buf.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    @property
    def _buf_position(self):
        rv = self._buf.tell()
        assert rv <= self._file_position
        return rv

    def read(self, bytes=-1):
        assert self._buf_position <= self._file_position

        if bytes < 0:
            bytes = self.length - self._buf_position
        bytes_remaining = min(bytes, self.length - self._buf_position)

        if bytes_remaining == 0:
            return b""

        if self._buf_position != self._file_position:
            buf_bytes = min(bytes_remaining, self._file_position - self._buf_position)
            old_data = self._buf.read(buf_bytes)
            bytes_remaining -= buf_bytes
        else:
            old_data = b""

        assert bytes_remaining == 0 or self._buf_position == self._file_position, (
            "Before reading buffer position (%i) didn't match file position (%i)" %
            (self._buf_position, self._file_position))
        new_data = self._file.read(bytes_remaining)
        self._buf.write(new_data)
        self._file_position += bytes_remaining
        assert bytes_remaining == 0 or self._buf_position == self._file_position, (
            "After reading buffer position (%i) didn't match file position (%i)" %
            (self._buf_position, self._file_position))

        return old_data + new_data

    def tell(self):
        return self._buf_position

    def seek(self, offset):
        if offset > self.length or offset < 0:
            raise ValueError
        if offset <= self._file_position:
            self._buf.seek(offset)
        else:
            self.read(offset - self._file_position)

    def readline(self, max_bytes=None):
        if max_bytes is None:
            max_bytes = self.length - self._buf_position

        if self._buf_position < self._file_position:
            data = self._buf.readline(max_bytes)
            if data.endswith(b"\n") or len(data) == max_bytes:
                return data
        else:
            data = b""

        assert self._buf_position == self._file_position

        initial_position = self._file_position
        found = False
        buf = []
        max_bytes -= len(data)
        while not found:
            readahead = self.read(min(2, max_bytes))
            max_bytes -= len(readahead)
            for i, c in enumerate(readahead):
                if c == b"\n"[0]:
                    buf.append(readahead[:i+1])
                    found = True
                    break
            if not found:
                buf.append(readahead)
            if not readahead or not max_bytes:
                break
        new_data = b"".join(buf)
        data += new_data
        self.seek(initial_position + len(new_data))
        return data

    def readlines(self):
        rv = []
        while True:
            data = self.readline()
            if data:
                rv.append(data)
            else:
                break
        return rv

    def __next__(self):
        data = self.readline()
        if data:
            return data
        else:
            raise StopIteration

    next = __next__

    def __iter__(self):
        return self


class Request:
    """Object representing a HTTP request.

    .. attribute:: doc_root

    The local directory to use as a base when resolving paths

    .. attribute:: route_match

    Regexp match object from matching the request path to the route
    selected for the request.

    .. attribute:: client_address

    Contains a tuple of the form (host, port) representing the client's address.

    .. attribute:: protocol_version

    HTTP version specified in the request.

    .. attribute:: method

    HTTP method in the request.

    .. attribute:: request_path

    Request path as it appears in the HTTP request.

    .. attribute:: url_base

    The prefix part of the path; typically / unless the handler has a url_base set

    .. attribute:: url

    Absolute URL for the request.

    .. attribute:: url_parts

    Parts of the requested URL as obtained by urlparse.urlsplit(path)

    .. attribute:: request_line

    Raw request line

    .. attribute:: headers

    RequestHeaders object providing a dictionary-like representation of
    the request headers.

    .. attribute:: raw_headers.

    Dictionary of non-normalized request headers.

    .. attribute:: body

    Request body as a string

    .. attribute:: raw_input

    File-like object representing the body of the request.

    .. attribute:: GET

    MultiDict representing the parameters supplied with the request.
    Note that these may be present on non-GET requests; the name is
    chosen to be familiar to users of other systems such as PHP.
    Both keys and values are binary strings.

    .. attribute:: POST

    MultiDict representing the request body parameters. Most parameters
    are present as string values, but file uploads have file-like
    values. All string values (including keys) have binary type.

    .. attribute:: cookies

    A Cookies object representing cookies sent with the request with a
    dictionary-like interface.

    .. attribute:: auth

    An instance of Authentication with username and password properties
    representing any credentials supplied using HTTP authentication.

    .. attribute:: server

    Server object containing information about the server environment.
    """

    def __init__(self, request_handler):
        self.doc_root = request_handler.server.router.doc_root
        self.route_match = None  # Set by the router
        self.client_address = request_handler.client_address

        self.protocol_version = request_handler.protocol_version
        self.method = request_handler.command

        # Keys and values in raw headers are native strings.
        self._headers = None
        self.raw_headers = request_handler.headers

        scheme = request_handler.server.scheme
        host = self.raw_headers.get("Host")
        port = request_handler.server.server_address[1]

        if host is None:
            host = request_handler.server.server_address[0]
        else:
            if ":" in host:
                host, port = host.split(":", 1)

        self.request_path = request_handler.path
        self.url_base = "/"

        if self.request_path.startswith(scheme + "://"):
            self.url = self.request_path
        else:
            # TODO(#23362): Stop using native strings for URLs.
            self.url = "%s://%s:%s%s" % (
                scheme, host, port, self.request_path)
        self.url_parts = urlsplit(self.url)

        self.request_line = request_handler.raw_requestline

        self.raw_input = InputFile(request_handler.rfile,
                                   int(self.raw_headers.get("Content-Length", 0)))

        self._body = None

        self._GET = None
        self._POST = None
        self._cookies = None
        self._auth = None

        self.server = Server(self)

    def close(self):
        return self.raw_input.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def __repr__(self):
        return "<Request %s %s>" % (self.method, self.url)

    @property
    def GET(self):
        if self._GET is None:
            kwargs = {
                "keep_blank_values": True,
                "encoding": "iso-8859-1",
            }
            params = parse_qsl(self.url_parts.query, **kwargs)
            self._GET = MultiDict()
            for key, value in params:
                self._GET.add(isomorphic_encode(key), isomorphic_encode(value))
        return self._GET

    @property
    def POST(self):
        if self._POST is None:
            # Work out the post parameters
            pos = self.raw_input.tell()
            self.raw_input.seek(0)
            kwargs = {
                "fp": self.raw_input,
                "environ": {"REQUEST_METHOD": self.method},
                "headers": self.raw_headers,
                "keep_blank_values": True,
                "encoding": "iso-8859-1",
            }
            fs = FieldStorage(**kwargs)
            self._POST = MultiDict.from_field_storage(fs)
            self.raw_input.seek(pos)
        return self._POST

    @property
    def cookies(self):
        if self._cookies is None:
            parser = BinaryCookieParser()
            cookie_headers = self.headers.get("cookie", b"")
            parser.load(cookie_headers)
            cookies = Cookies()
            for key, value in parser.items():
                cookies[isomorphic_encode(key)] = CookieValue(value)
            self._cookies = cookies
        return self._cookies

    @property
    def headers(self):
        if self._headers is None:
            self._headers = RequestHeaders(self.raw_headers)
        return self._headers

    @property
    def body(self):
        if self._body is None:
            pos = self.raw_input.tell()
            self.raw_input.seek(0)
            self._body = self.raw_input.read()
            self.raw_input.seek(pos)
        return self._body

    @property
    def auth(self):
        if self._auth is None:
            self._auth = Authentication(self.headers)
        return self._auth


class H2Request(Request):
    def __init__(self, request_handler):
        self.h2_stream_id = request_handler.h2_stream_id
        self.frames = []
        super().__init__(request_handler)


class RequestHeaders(Dict[bytes, List[bytes]]):
    """Read-only dictionary-like API for accessing request headers.

    Unlike BaseHTTPRequestHandler.headers, this class always returns all
    headers with the same name (separated by commas). And it ensures all keys
    (i.e. names of headers) and values have binary type.
    """
    def __init__(self, items):
        for header in items.keys():
            key = isomorphic_encode(header).lower()
            # get all headers with the same name
            values = items.getallmatchingheaders(header)
            if len(values) > 1:
                # collect the multiple variations of the current header
                multiples = []
                # loop through the values from getallmatchingheaders
                for value in values:
                    # getallmatchingheaders returns raw header lines, so
                    # split to get name, value
                    multiples.append(isomorphic_encode(value).split(b':', 1)[1].strip())
                headers = multiples
            else:
                headers = [isomorphic_encode(items[header])]
            dict.__setitem__(self, key, headers)

    def __getitem__(self, key):
        """Get all headers of a certain (case-insensitive) name. If there is
        more than one, the values are returned comma separated"""
        key = isomorphic_encode(key)
        values = dict.__getitem__(self, key.lower())
        if len(values) == 1:
            return values[0]
        else:
            return b", ".join(values)

    def __setitem__(self, name, value):
        raise Exception

    def get(self, key, default=None):
        """Get a string representing all headers with a particular value,
        with multiple headers separated by a comma. If no header is found
        return a default value

        :param key: The header name to look up (case-insensitive)
        :param default: The value to return in the case of no match
        """
        try:
            return self[key]
        except KeyError:
            return default

    def get_list(self, key, default=missing):
        """Get all the header values for a particular field name as
        a list"""
        key = isomorphic_encode(key)
        try:
            return dict.__getitem__(self, key.lower())
        except KeyError:
            if default is not missing:
                return default
            else:
                raise

    def __contains__(self, key):
        key = isomorphic_encode(key)
        return dict.__contains__(self, key.lower())

    def iteritems(self):
        for item in self:
            yield item, self[item]

    def itervalues(self):
        for item in self:
            yield self[item]


class CookieValue:
    """Representation of cookies.

    Note that cookies are considered read-only and the string value
    of the cookie will not change if you update the field values.
    However this is not enforced.

    .. attribute:: key

    The name of the cookie.

    .. attribute:: value

    The value of the cookie

    .. attribute:: expires

    The expiry date of the cookie

    .. attribute:: path

    The path of the cookie

    .. attribute:: comment

    The comment of the cookie.

    .. attribute:: domain

    The domain with which the cookie is associated

    .. attribute:: max_age

    The max-age value of the cookie.

    .. attribute:: secure

    Whether the cookie is marked as secure

    .. attribute:: httponly

    Whether the cookie is marked as httponly

    """
    def __init__(self, morsel):
        self.key = morsel.key
        self.value = morsel.value

        for attr in ["expires", "path",
                     "comment", "domain", "max-age",
                     "secure", "version", "httponly"]:
            setattr(self, attr.replace("-", "_"), morsel[attr])

        self._str = morsel.OutputString()

    def __str__(self):
        return self._str

    def __repr__(self):
        return self._str

    def __eq__(self, other):
        """Equality comparison for cookies. Compares to other cookies
        based on value alone and on non-cookies based on the equality
        of self.value with the other object so that a cookie with value
        "ham" compares equal to the string "ham"
        """
        if hasattr(other, "value"):
            return self.value == other.value
        return self.value == other


class MultiDict(Dict[KT, VT]):
    """Dictionary type that holds multiple values for each key"""
    # TODO: this should perhaps also order the keys
    def __init__(self):
        pass

    def __setitem__(self, name, value):
        dict.__setitem__(self, name, [value])

    def add(self, name, value):
        if name in self:
            dict.__getitem__(self, name).append(value)
        else:
            dict.__setitem__(self, name, [value])

    def __getitem__(self, key):
        """Get the first value with a given key"""
        return self.first(key)

    def first(self, key, default=missing):
        """Get the first value with a given key

        :param key: The key to lookup
        :param default: The default to return if key is
                        not found (throws if nothing is
                        specified)
        """
        if key in self and dict.__getitem__(self, key):
            return dict.__getitem__(self, key)[0]
        elif default is not missing:
            return default
        raise KeyError(key)

    def last(self, key, default=missing):
        """Get the last value with a given key

        :param key: The key to lookup
        :param default: The default to return if key is
                        not found (throws if nothing is
                        specified)
        """
        if key in self and dict.__getitem__(self, key):
            return dict.__getitem__(self, key)[-1]
        elif default is not missing:
            return default
        raise KeyError(key)

    # We need to explicitly override dict.get; otherwise, it won't call
    # __getitem__ and would return a list instead.
    def get(self, key, default=None):
        """Get the first value with a given key

        :param key: The key to lookup
        :param default: The default to return if key is
                        not found (None by default)
        """
        return self.first(key, default)

    def get_list(self, key):
        """Get all values with a given key as a list

        :param key: The key to lookup
        """
        if key in self:
            return dict.__getitem__(self, key)
        else:
            return []

    @classmethod
    def from_field_storage(cls, fs):
        """Construct a MultiDict from a FieldStorage

        Note that all keys and values are binary strings.
        """
        self = cls()
        if fs.list is None:
            return self
        for key in fs:
            values = fs[key]
            if not isinstance(values, list):
                values = [values]

            for value in values:
                if not value.filename:
                    value = isomorphic_encode(value.value)
                else:
                    assert isinstance(value, FieldStorage)
                self.add(isomorphic_encode(key), value)
        return self


class BinaryCookieParser(BaseCookie):  # type: ignore
    """A subclass of BaseCookie that returns values in binary strings

    This is not intended to store the cookies; use Cookies instead.
    """
    def value_decode(self, val):
        """Decode value from network to (real_value, coded_value).

        Override BaseCookie.value_decode.
        """
        return isomorphic_encode(val), val

    def value_encode(self, val):
        raise NotImplementedError('BinaryCookieParser is not for setting cookies')

    def load(self, rawdata):
        """Load cookies from a binary string.

        This overrides and calls BaseCookie.load. Unlike BaseCookie.load, it
        does not accept dictionaries.
        """
        assert isinstance(rawdata, bytes)
        # BaseCookie.load expects a native string
        super().load(isomorphic_decode(rawdata))


class Cookies(MultiDict[bytes, CookieValue]):
    """MultiDict specialised for Cookie values

    Keys are binary strings and values are CookieValue objects.
    """
    def __init__(self):
        pass

    def __getitem__(self, key):
        return self.last(key)


class Authentication:
    """Object for dealing with HTTP Authentication

    .. attribute:: username

    The username supplied in the HTTP Authorization
    header, or None

    .. attribute:: password

    The password supplied in the HTTP Authorization
    header, or None

    Both attributes are binary strings (`str` in Py2, `bytes` in Py3), since
    RFC7617 Section 2.1 does not specify the encoding for username & password
    (as long it's compatible with ASCII). UTF-8 should be a relatively safe
    choice if callers need to decode them as most browsers use it.
    """
    def __init__(self, headers):
        self.username = None
        self.password = None

        auth_schemes = {b"Basic": self.decode_basic}

        if "authorization" in headers:
            header = headers.get("authorization")
            assert isinstance(header, bytes)
            auth_type, data = header.split(b" ", 1)
            if auth_type in auth_schemes:
                self.username, self.password = auth_schemes[auth_type](data)
            else:
                raise HTTPException(400, "Unsupported authentication scheme %s" % auth_type)

    def decode_basic(self, data):
        assert isinstance(data, bytes)
        decoded_data = base64.b64decode(data)
        return decoded_data.split(b":", 1)
