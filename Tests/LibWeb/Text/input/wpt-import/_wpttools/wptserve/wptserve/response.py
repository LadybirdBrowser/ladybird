# mypy: allow-untyped-defs

import json
import uuid
import traceback
from collections import OrderedDict
from datetime import datetime, timedelta, timezone
from io import BytesIO

from hpack.struct import HeaderTuple
from http.cookies import BaseCookie, Morsel
from hyperframe.frame import HeadersFrame, DataFrame, ContinuationFrame

from .constants import response_codes, h2_headers
from .logger import get_logger
from .utils import isomorphic_decode, isomorphic_encode

missing = object()


class Response:
    """Object representing the response to a HTTP request

    :param handler: RequestHandler being used for this response
    :param request: Request that this is the response for

    .. attribute:: request

       Request associated with this Response.

    .. attribute:: encoding

       The encoding to use when converting unicode to strings for output.

    .. attribute:: add_required_headers

       Boolean indicating whether mandatory headers should be added to the
       response.

    .. attribute:: send_body_for_head_request

       Boolean, default False, indicating whether the body content should be
       sent when the request method is HEAD.

    .. attribute:: writer

       The ResponseWriter for this response

    .. attribute:: status

       Status tuple (code, message). Can be set to an integer in which case the
       message part is filled in automatically, or a tuple (code, message) in
       which case code is an int and message is a text or binary string.

    .. attribute:: headers

       List of HTTP headers to send with the response. Each item in the list is a
       tuple of (name, value).

    .. attribute:: content

       The body of the response. This can either be a string or a iterable of response
       parts. If it is an iterable, any item may be a string or a function of zero
       parameters which, when called, returns a string."""

    def __init__(self, handler, request, response_writer_cls=None):
        self.request = request
        self.encoding = "utf8"

        self.add_required_headers = True
        self.send_body_for_head_request = False
        self.close_connection = False

        self.logger = get_logger()
        self.writer = response_writer_cls(handler, self) if response_writer_cls else ResponseWriter(handler, self)

        self._status = (200, None)
        self.headers = ResponseHeaders()
        self.content = []

    @property
    def status(self):
        return self._status

    @status.setter
    def status(self, value):
        if hasattr(value, "__len__"):
            if len(value) != 2:
                raise ValueError
            else:
                code = int(value[0])
                message = value[1]
                # Only call str() if message is not a string type, so that we
                # don't get `str(b"foo") == "b'foo'"` in Python 3.
                if not isinstance(message, (bytes, str)):
                    message = str(message)
                self._status = (code, message)
        else:
            self._status = (int(value), None)

    def set_cookie(self, name, value, path="/", domain=None, max_age=None,
                   expires=None, samesite=None, secure=False, httponly=False,
                   comment=None):
        """Set a cookie to be sent with a Set-Cookie header in the
        response

        :param name: name of the cookie (a binary string)
        :param value: value of the cookie (a binary string, or None)
        :param max_age: datetime.timedelta int representing the time (in seconds)
                        until the cookie expires
        :param path: String path to which the cookie applies
        :param domain: String domain to which the cookie applies
        :param samesit: String indicating whether the cookie should be
                         restricted to same site context
        :param secure: Boolean indicating whether the cookie is marked as secure
        :param httponly: Boolean indicating whether the cookie is marked as
                         HTTP Only
        :param comment: String comment
        :param expires: datetime.datetime or datetime.timedelta indicating a
                        time or interval from now when the cookie expires

        """
        # TODO(Python 3): Convert other parameters (e.g. path) to bytes, too.
        if value is None:
            value = b''
            max_age = 0
            expires = timedelta(days=-1)

        name = isomorphic_decode(name)
        value = isomorphic_decode(value)

        days = {i+1: name for i, name in enumerate(["jan", "feb", "mar",
                                                    "apr", "may", "jun",
                                                    "jul", "aug", "sep",
                                                    "oct", "nov", "dec"])}

        if isinstance(expires, timedelta):
            expires = datetime.now(timezone.utc) + expires

        if expires is not None:
            expires_str = expires.strftime("%d %%s %Y %H:%M:%S GMT")
            expires_str = expires_str % days[expires.month]
            expires = expires_str

        if max_age is not None:
            if hasattr(max_age, "total_seconds"):
                max_age = int(max_age.total_seconds())
            max_age = "%.0d" % max_age

        m = Morsel()

        def maybe_set(key, value):
            if value is not None and value is not False:
                m[key] = value

        m.set(name, value, value)
        maybe_set("path", path)
        maybe_set("domain", domain)
        maybe_set("comment", comment)
        maybe_set("expires", expires)
        maybe_set("max-age", max_age)
        maybe_set("secure", secure)
        maybe_set("httponly", httponly)
        maybe_set("samesite", samesite)

        self.headers.append("Set-Cookie", m.OutputString())

    def unset_cookie(self, name):
        """Remove a cookie from those that are being sent with the response"""
        name = isomorphic_decode(name)
        cookies = self.headers.get("Set-Cookie")
        parser = BaseCookie()
        for cookie in cookies:
            parser.load(isomorphic_decode(cookie))

        if name in parser.keys():
            del self.headers["Set-Cookie"]
            for m in parser.values():
                if m.key != name:
                    self.headers.append(("Set-Cookie", m.OutputString()))

    def delete_cookie(self, name, path="/", domain=None):
        """Delete a cookie on the client by setting it to the empty string
        and to expire in the past"""
        self.set_cookie(name, None, path=path, domain=domain, max_age=0,
                        expires=timedelta(days=-1))

    def iter_content(self, read_file=False):
        """Iterator returning chunks of response body content.

        If any part of the content is a function, this will be called
        and the resulting value (if any) returned.

        :param read_file: boolean controlling the behaviour when content is a
                          file handle. When set to False the handle will be
                          returned directly allowing the file to be passed to
                          the output in small chunks. When set to True, the
                          entire content of the file will be returned as a
                          string facilitating non-streaming operations like
                          template substitution.
        """
        if isinstance(self.content, bytes):
            yield self.content
        elif isinstance(self.content, str):
            yield self.content.encode(self.encoding)
        elif hasattr(self.content, "read"):
            # Read the file in chunks rather than reading the whole file into
            # memory at once. (See also ResponseWriter.file_chunk_size)
            while True:
                read = self.content.read(32 * 1024)
                if len(read) == 0:
                    break
                yield read
            self.content.close()
        else:
            for item in self.content:
                if hasattr(item, "__call__"):
                    value = item()
                else:
                    value = item
                if value:
                    yield value

    def write_status_headers(self):
        """Write out the status line and headers for the response"""
        self.writer.write_status(*self.status)
        for item in self.headers:
            self.writer.write_header(*item)
        self.writer.end_headers()

    def write_content(self):
        """Write out the response content"""
        if self.request.method != "HEAD" or self.send_body_for_head_request:
            for item in self.iter_content():
                self.writer.write_content(item)

    def write(self):
        """Write the whole response"""
        self.write_status_headers()
        self.write_content()

    def set_error(self, code, err=None):
        """Set the response status headers and return a JSON error object:

        {"error": {"code": code, "message": message}}
        code is an int (HTTP status code), and message is a text string.
        """
        if 500 <= code < 600:
            message = self._format_server_error(err)
            self.logger.warning(message)
        else:
            if err is None:
                message = ""
            else:
                message = str(err)

        data = json.dumps({"error": {
            "code": code,
            "message": message}
        })
        self.status = code
        self.headers = [("Content-Type", "application/json"),
                        ("Content-Length", len(data))]
        self.content = data

    def _format_server_error(self, err):
        if err is None:
            suffix = "<no traceback>"
        elif isinstance(err, str):
            suffix = err
        elif self.request.server.config.logging["suppress_handler_traceback"]:
            frame = traceback.extract_tb(err.__traceback__)[-1]
            suffix = (f"""File "{frame.filename}", line {frame.lineno} """
                      f"""in {frame.name} (traceback suppressed)""")
        else:
            tb = "\n".join(f"  {line}"
                           for line in traceback.format_tb(err.__traceback__))
            suffix = f"""Traceback (most recent call last):
{tb}  {type(err).__name__}: {err}
"""
        return f"Internal server error loading {self.request.url}:\n  {suffix}"


class MultipartContent:
    def __init__(self, boundary=None, default_content_type=None):
        self.items = []
        if boundary is None:
            boundary = str(uuid.uuid4())
        self.boundary = boundary
        self.default_content_type = default_content_type

    def __call__(self):
        boundary = b"--" + self.boundary.encode("ascii")
        rv = [b"", boundary]
        for item in self.items:
            rv.append(item.to_bytes())
            rv.append(boundary)
        rv[-1] += b"--"
        return b"\r\n".join(rv)

    def append_part(self, data, content_type=None, headers=None):
        if content_type is None:
            content_type = self.default_content_type
        self.items.append(MultipartPart(data, content_type, headers))

    def __iter__(self):
        #This is hackish; when writing the response we need an iterable
        #or a string. For a multipart/byterange response we want an
        #iterable that contains a single callable; the MultipartContent
        #object itself
        yield self


class MultipartPart:
    def __init__(self, data, content_type=None, headers=None):
        assert isinstance(data, bytes), data
        self.headers = ResponseHeaders()

        if content_type is not None:
            self.headers.set("Content-Type", content_type)

        if headers is not None:
            for name, value in headers:
                if name.lower() == b"content-type":
                    func = self.headers.set
                else:
                    func = self.headers.append
                func(name, value)

        self.data = data

    def to_bytes(self):
        rv = []
        for key, value in self.headers:
            assert isinstance(key, bytes)
            assert isinstance(value, bytes)
            rv.append(b"%s: %s" % (key, value))
        rv.append(b"")
        rv.append(self.data)
        return b"\r\n".join(rv)


def _maybe_encode(s):
    """Encode a string or an int into binary data using isomorphic_encode()."""
    if isinstance(s, int):
        return b"%i" % (s,)
    return isomorphic_encode(s)


class ResponseHeaders:
    """Dictionary-like object holding the headers for the response"""
    def __init__(self):
        self.data = OrderedDict()

    def set(self, key, value):
        """Set a header to a specific value, overwriting any previous header
        with the same name

        :param key: Name of the header to set
        :param value: Value to set the header to
        """
        key = _maybe_encode(key)
        value = _maybe_encode(value)
        self.data[key.lower()] = (key, [value])

    def append(self, key, value):
        """Add a new header with a given name, not overwriting any existing
        headers with the same name

        :param key: Name of the header to add
        :param value: Value to set for the header
        """
        key = _maybe_encode(key)
        value = _maybe_encode(value)
        if key.lower() in self.data:
            self.data[key.lower()][1].append(value)
        else:
            self.set(key, value)

    def get(self, key, default=missing):
        """Get the set values for a particular header."""
        key = _maybe_encode(key)
        try:
            return self[key]
        except KeyError:
            if default is missing:
                return []
            return default

    def __getitem__(self, key):
        """Get a list of values for a particular header

        """
        key = _maybe_encode(key)
        return self.data[key.lower()][1]

    def __delitem__(self, key):
        key = _maybe_encode(key)
        del self.data[key.lower()]

    def __contains__(self, key):
        key = _maybe_encode(key)
        return key.lower() in self.data

    def __setitem__(self, key, value):
        self.set(key, value)

    def __iter__(self):
        for key, values in self.data.values():
            for value in values:
                yield key, value

    def items(self):
        return list(self)

    def update(self, items_iter):
        for name, value in items_iter:
            self.append(name, value)

    def __repr__(self):
        return repr(self.data)


class H2Response(Response):

    def __init__(self, handler, request):
        super().__init__(handler, request, response_writer_cls=H2ResponseWriter)

    def write_status_headers(self):
        self.writer.write_headers(self.headers, *self.status)

    # Hacky way of detecting last item in generator
    def write_content(self):
        """Write out the response content"""
        if self.request.method != "HEAD" or self.send_body_for_head_request:
            item = None
            item_iter = self.iter_content()
            try:
                item = next(item_iter)
                while True:
                    check_last = next(item_iter)
                    self.writer.write_data(item, last=False)
                    item = check_last
            except StopIteration:
                if item:
                    self.writer.write_data(item, last=True)


class H2ResponseWriter:

    def __init__(self, handler, response):
        self.socket = handler.request
        self.h2conn = handler.conn
        self._response = response
        self._handler = handler
        self.stream_ended = False
        self.content_written = False
        self.request = response.request
        self.logger = response.logger

    def write_headers(self, headers, status_code, status_message=None, stream_id=None, last=False):
        """
        Send a HEADER frame that is tracked by the local state machine.

        Write a HEADER frame using the H2 Connection object, will only work if the stream is in a state to send
        HEADER frames.

        :param headers: List of (header, value) tuples
        :param status_code: The HTTP status code of the response
        :param stream_id: Id of stream to send frame on. Will use the request stream ID if None
        :param last: Flag to signal if this is the last frame in stream.
        """
        formatted_headers = []
        secondary_headers = []  # Non ':' prefixed headers are to be added afterwards

        for header, value in headers:
            # h2_headers are native strings
            # header field names are strings of ASCII
            if isinstance(header, bytes):
                header = header.decode('ascii')
            # value in headers can be either string or integer
            if isinstance(value, bytes):
                value = self.decode(value)
            if header in h2_headers:
                header = ':' + header
                formatted_headers.append((header, str(value)))
            else:
                secondary_headers.append((header, str(value)))

        formatted_headers.append((':status', str(status_code)))
        formatted_headers.extend(secondary_headers)

        with self.h2conn as connection:
            connection.send_headers(
                stream_id=self.request.h2_stream_id if stream_id is None else stream_id,
                headers=formatted_headers,
                end_stream=last or self.request.method == "HEAD"
            )

            self.write(connection)

    def write_data(self, item, last=False, stream_id=None):
        """
        Send a DATA frame that is tracked by the local state machine.

        Write a DATA frame using the H2 Connection object, will only work if the stream is in a state to send
        DATA frames. Uses flow control to split data into multiple data frames if it exceeds the size that can
        be in a single frame.

        :param item: The content of the DATA frame
        :param last: Flag to signal if this is the last frame in stream.
        :param stream_id: Id of stream to send frame on. Will use the request stream ID if None
        """
        if isinstance(item, (str, bytes)):
            data = BytesIO(self.encode(item))
        else:
            data = item

        # Find the length of the data
        data.seek(0, 2)
        data_len = data.tell()
        data.seek(0)

        # If the data is longer than max payload size, need to write it in chunks
        payload_size = self.get_max_payload_size()
        while data_len > payload_size:
            self.write_data_frame(data.read(payload_size), False, stream_id)
            data_len -= payload_size
            payload_size = self.get_max_payload_size()

        self.write_data_frame(data.read(), last, stream_id)

    def write_data_frame(self, data, last, stream_id=None):
        with self.h2conn as connection:
            connection.send_data(
                stream_id=self.request.h2_stream_id if stream_id is None else stream_id,
                data=data,
                end_stream=last,
            )
            self.write(connection)
        self.stream_ended = last

    def write_push(self, promise_headers, push_stream_id=None, status=None, response_headers=None, response_data=None):
        """Write a push promise, and optionally write the push content.

        This will write a push promise to the request stream. If you do not provide headers and data for the response,
        then no response will be pushed, and you should push them yourself using the ID returned from this function

        :param promise_headers: A list of header tuples that matches what the client would use to
                                request the pushed response
        :param push_stream_id: The ID of the stream the response should be pushed to. If none given, will
                               use the next available id.
        :param status: The status code of the response, REQUIRED if response_headers given
        :param response_headers: The headers of the response
        :param response_data: The response data.
        :return: The ID of the push stream
        """
        with self.h2conn as connection:
            push_stream_id = push_stream_id if push_stream_id is not None else connection.get_next_available_stream_id()
            connection.push_stream(self.request.h2_stream_id, push_stream_id, promise_headers)
            self.write(connection)

        has_data = response_data is not None
        if response_headers is not None:
            assert status is not None
            self.write_headers(response_headers, status, stream_id=push_stream_id, last=not has_data)

        if has_data:
            self.write_data(response_data, last=True, stream_id=push_stream_id)

        return push_stream_id

    def end_stream(self, stream_id=None):
        """Ends the stream with the given ID, or the one that request was made on if no ID given."""
        with self.h2conn as connection:
            connection.end_stream(stream_id if stream_id is not None else self.request.h2_stream_id)
            self.write(connection)
        self.stream_ended = True

    def write_raw_header_frame(self, headers, stream_id=None, end_stream=False, end_headers=False, frame_cls=HeadersFrame):
        """
        Ignores the statemachine of the stream and sends a HEADER frame regardless.

        Unlike `write_headers`, this does not check to see if a stream is in the correct state to have HEADER frames
        sent through to it. It will build a HEADER frame and send it without using the H2 Connection object other than
        to HPACK encode the headers.

        :param headers: List of (header, value) tuples
        :param stream_id: Id of stream to send frame on. Will use the request stream ID if None
        :param end_stream: Set to True to add END_STREAM flag to frame
        :param end_headers: Set to True to add END_HEADERS flag to frame
        """
        if not stream_id:
            stream_id = self.request.h2_stream_id

        header_t = []
        for header, value in headers:
            header_t.append(HeaderTuple(header, value))

        with self.h2conn as connection:
            frame = frame_cls(stream_id, data=connection.encoder.encode(header_t))

            if end_stream:
                self.stream_ended = True
                frame.flags.add('END_STREAM')
            if end_headers:
                frame.flags.add('END_HEADERS')

            data = frame.serialize()
            self.write_raw(data)

    def write_raw_data_frame(self, data, stream_id=None, end_stream=False):
        """
        Ignores the statemachine of the stream and sends a DATA frame regardless.

        Unlike `write_data`, this does not check to see if a stream is in the correct state to have DATA frames
        sent through to it. It will build a DATA frame and send it without using the H2 Connection object. It will
        not perform any flow control checks.

        :param data: The data to be sent in the frame
        :param stream_id: Id of stream to send frame on. Will use the request stream ID if None
        :param end_stream: Set to True to add END_STREAM flag to frame
        """
        if not stream_id:
            stream_id = self.request.h2_stream_id

        frame = DataFrame(stream_id, data=data)

        if end_stream:
            self.stream_ended = True
            frame.flags.add('END_STREAM')

        data = frame.serialize()
        self.write_raw(data)

    def write_raw_continuation_frame(self, headers, stream_id=None, end_headers=False):
        """
        Ignores the statemachine of the stream and sends a CONTINUATION frame regardless.

        This provides the ability to create and write a CONTINUATION frame to the stream, which is not exposed by
        `write_headers` as the h2 library handles the split between HEADER and CONTINUATION internally. Will perform
        HPACK encoding on the headers.

        :param headers: List of (header, value) tuples
        :param stream_id: Id of stream to send frame on. Will use the request stream ID if None
        :param end_headers: Set to True to add END_HEADERS flag to frame
        """
        self.write_raw_header_frame(headers, stream_id=stream_id, end_headers=end_headers, frame_cls=ContinuationFrame)


    def get_max_payload_size(self, stream_id=None):
        """Returns the maximum size of a payload for the given stream."""
        stream_id = stream_id if stream_id is not None else self.request.h2_stream_id
        with self.h2conn as connection:
            return min(connection.remote_settings.max_frame_size, connection.local_flow_control_window(stream_id)) - 9

    def write(self, connection):
        self.content_written = True
        data = connection.data_to_send()
        self.socket.sendall(data)

    def write_raw(self, raw_data):
        """Used for sending raw bytes/data through the socket"""

        self.content_written = True
        self.socket.sendall(raw_data)

    def decode(self, data):
        """Convert bytes to unicode according to response.encoding."""
        if isinstance(data, bytes):
            return data.decode(self._response.encoding)
        elif isinstance(data, str):
            return data
        else:
            raise ValueError(type(data))

    def encode(self, data):
        """Convert unicode to bytes according to response.encoding."""
        if isinstance(data, bytes):
            return data
        elif isinstance(data, str):
            return data.encode(self._response.encoding)
        else:
            raise ValueError


class ResponseWriter:
    """Object providing an API to write out a HTTP response.

    :param handler: The RequestHandler being used.
    :param response: The Response associated with this writer."""
    def __init__(self, handler, response):
        self._wfile = handler.wfile
        self._response = response
        self._handler = handler
        self._status_written = False
        self._headers_seen = set()
        self._headers_complete = False
        self.content_written = False
        self.request = response.request
        self.file_chunk_size = 32 * 1024
        self.default_status = 200

    def _seen_header(self, name):
        return self.encode(name.lower()) in self._headers_seen

    def write_status(self, code, message=None):
        """Write out the status line of a response.

        :param code: The integer status code of the response.
        :param message: The message of the response. Defaults to the message commonly used
                        with the status code."""
        if message is None:
            if code in response_codes:
                message = response_codes[code][0]
            else:
                message = ''
        self.write(b"%s %d %s\r\n" %
                   (isomorphic_encode(self._response.request.protocol_version), code, isomorphic_encode(message)))
        self._status_written = True

    def write_header(self, name, value):
        """Write out a single header for the response.

        If a status has not been written, a default status will be written (currently 200)

        :param name: Name of the header field
        :param value: Value of the header field
        :return: A boolean indicating whether the write succeeds
        """
        if not self._status_written:
            self.write_status(self.default_status)
        self._headers_seen.add(self.encode(name.lower()))
        if not self.write(name):
            return False
        if not self.write(b": "):
            return False
        if isinstance(value, int):
            if not self.write(str(value)):
                return False
        elif not self.write(value):
            return False
        return self.write(b"\r\n")

    def write_default_headers(self):
        for name, f in [("Server", self._handler.version_string),
                        ("Date", self._handler.date_time_string)]:
            if not self._seen_header(name):
                if not self.write_header(name, f()):
                    return False

        if (isinstance(self._response.content, (bytes, str)) and
            not self._seen_header("content-length")):
            #Would be nice to avoid double-encoding here
            if not self.write_header("Content-Length", len(self.encode(self._response.content))):
                return False

        return True

    def end_headers(self):
        """Finish writing headers and write the separator.

        Unless add_required_headers on the response is False,
        this will also add HTTP-mandated headers that have not yet been supplied
        to the response headers.
        :return: A boolean indicating whether the write succeeds
        """

        if self._response.add_required_headers:
            if not self.write_default_headers():
                return False

        if not self.write("\r\n"):
            return False
        if not self._seen_header("content-length"):
            self._response.close_connection = True
        self._headers_complete = True

        return True

    def write_content(self, data):
        """Write the body of the response.

        HTTP-mandated headers will be automatically added with status default to 200 if they have
        not been explicitly set.
        :return: A boolean indicating whether the write succeeds
        """
        if not self._status_written:
            self.write_status(self.default_status)
        if not self._headers_complete:
            self._response.content = data
            self.end_headers()
        return self.write_raw_content(data)

    def write_raw_content(self, data):
        """Writes the data 'as is'"""
        if data is None:
            raise ValueError('data cannot be None')
        if isinstance(data, (str, bytes)):
            # Deliberately allows both text and binary types. See `self.encode`.
            return self.write(data)
        else:
            return self.write_content_file(data)

    def write(self, data):
        """Write directly to the response, converting unicode to bytes
        according to response.encoding.
        :return: A boolean indicating whether the write succeeds
        """
        self.content_written = True
        try:
            self._wfile.write(self.encode(data))
            return True
        except OSError:
            # This can happen if the socket got closed by the remote end
            return False

    def write_content_file(self, data):
        """Write a file-like object directly to the response in chunks."""
        self.content_written = True
        success = True
        while True:
            buf = data.read(self.file_chunk_size)
            if not buf:
                success = False
                break
            try:
                self._wfile.write(buf)
            except OSError:
                success = False
                break
        data.close()
        return success

    def encode(self, data):
        """Convert unicode to bytes according to response.encoding."""
        if isinstance(data, bytes):
            return data
        elif isinstance(data, str):
            return data.encode(self._response.encoding)
        else:
            raise ValueError("data %r should be text or binary, but is %s" % (data, type(data)))
