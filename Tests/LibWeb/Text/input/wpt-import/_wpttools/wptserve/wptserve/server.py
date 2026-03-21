# mypy: allow-untyped-defs

import errno
import http
import http.server
import ipaddress
import os
import platform
import selectors
import socket
import socketserver
import ssl
import sys
import threading
import time
import traceback
import uuid
from collections import OrderedDict
from queue import Empty, Queue
from typing import Dict

from h2.config import H2Configuration
from h2.connection import H2Connection
from h2.events import RequestReceived, ConnectionTerminated, DataReceived, StreamReset, StreamEnded
from h2.exceptions import StreamClosedError, ProtocolError
from h2.settings import SettingCodes
from h2.utilities import extract_method_header

from urllib.parse import urlsplit, urlunsplit

from pywebsocket3 import dispatch
from pywebsocket3.handshake import HandshakeException, AbortedByUserException

from . import routes as default_routes
from .config import ConfigBuilder
from .logger import get_logger
from .request import Server, Request, H2Request
from .response import Response, H2Response
from .router import Router
from .utils import HTTPException, get_error_cause, isomorphic_decode, isomorphic_encode
from .constants import h2_headers
from .ws_h2_handshake import WsH2Handshaker

# We need to stress test that browsers can send/receive many headers (there is
# no specified limit), but the Python stdlib has an arbitrary limit of 100
# headers. Hitting the limit leads to HTTP 431, so we monkey patch it higher.
# https://bugs.python.org/issue26586
# https://github.com/web-platform-tests/wpt/pull/24451
import http.client
assert isinstance(getattr(http.client, '_MAXHEADERS'), int)
setattr(http.client, '_MAXHEADERS', 512)

"""
HTTP server designed for testing purposes.

The server is designed to provide flexibility in the way that
requests are handled, and to provide control both of exactly
what bytes are put on the wire for the response, and in the
timing of sending those bytes.

The server is based on the stdlib HTTPServer, but with some
notable differences in the way that requests are processed.
Overall processing is handled by a WebTestRequestHandler,
which is a subclass of BaseHTTPRequestHandler. This is responsible
for parsing the incoming request. A RequestRewriter is then
applied and may change the request data if it matches a
supplied rule.

Once the request data had been finalised, Request and Response
objects are constructed. These are used by the other parts of the
system to read information about the request and manipulate the
response.

Each request is handled by a particular handler function. The
mapping between Request and the appropriate handler is determined
by a Router. By default handlers are installed to interpret files
under the document root with .py extensions as executable python
files (see handlers.py for the api for such files), .asis files as
bytestreams to be sent literally and all other files to be served
statically.

The handler functions are responsible for either populating the
fields of the response object, which will then be written when the
handler returns, or for directly writing to the output stream.
"""


class RequestRewriter:
    def __init__(self, rules):
        """Object for rewriting the request path.

        :param rules: Initial rules to add; a list of three item tuples
                      (method, input_path, output_path), defined as for
                      register()
        """
        self.rules = {}
        for rule in reversed(rules):
            self.register(*rule)
        self.logger = get_logger()

    def register(self, methods, input_path, output_path):
        """Register a rewrite rule.

        :param methods: Set of methods this should match. "*" is a
                        special value indicating that all methods should
                        be matched.

        :param input_path: Path to match for the initial request.

        :param output_path: Path to replace the input path with in
                            the request.
        """
        if isinstance(methods, (bytes, str)):
            methods = [methods]
        self.rules[input_path] = (methods, output_path)

    def rewrite(self, request_handler):
        """Rewrite the path in a BaseHTTPRequestHandler instance, if
           it matches a rule.

        :param request_handler: BaseHTTPRequestHandler for which to
                                rewrite the request.
        """
        split_url = urlsplit(request_handler.path)
        if split_url.path in self.rules:
            methods, destination = self.rules[split_url.path]
            if "*" in methods or request_handler.command in methods:
                self.logger.debug("Rewriting request path %s to %s" %
                             (request_handler.path, destination))
                new_url = list(split_url)
                new_url[2] = destination
                new_url = urlunsplit(new_url)
                request_handler.path = new_url


class WebTestServer(http.server.ThreadingHTTPServer):
    allow_reuse_address = True
    # Older versions of Python might throw `OSError: [Errno 0] Error`
    # instead of `SSLEOFError`.
    acceptable_errors = (errno.EPIPE, errno.ECONNABORTED, 0)
    request_queue_size = 2000

    # Ensure that we don't hang on shutdown waiting for requests
    daemon_threads = True

    def __init__(self, server_address, request_handler_cls,
                 router, rewriter, bind_address, ws_doc_root=None,
                 config=None, use_ssl=False, key_file=None, certificate=None,
                 encrypt_after_connect=False, latency=None, http2=False, **kwargs):
        """Server for HTTP(s) Requests

        :param server_address: tuple of (server_name, port)

        :param request_handler_cls: BaseHTTPRequestHandler-like class to use for
                                    handling requests.

        :param router: Router instance to use for matching requests to handler
                       functions

        :param rewriter: RequestRewriter-like instance to use for preprocessing
                         requests before they are routed

        :param config: Dictionary holding environment configuration settings for
                       handlers to read, or None to use the default values.

        :param use_ssl: Boolean indicating whether the server should use SSL

        :param key_file: Path to key file to use if SSL is enabled.

        :param certificate: Path to certificate to use if SSL is enabled.

        :param ws_doc_root: Document root for websockets

        :param encrypt_after_connect: For each connection, don't start encryption
                                      until a CONNECT message has been received.
                                      This enables the server to act as a
                                      self-proxy.

        :param bind_address True to bind the server to both the IP address and
                            port specified in the server_address parameter.
                            False to bind the server only to the port in the
                            server_address parameter, but not to the address.
        :param latency: Delay in ms to wait before serving each response, or
                        callable that returns a delay in ms
        """
        self._shutdown_event = threading.Event()
        self._shutdown_write_sock = None

        self.router = router
        self.rewriter = rewriter

        self.scheme = "http2" if http2 else "https" if use_ssl else "http"
        self.logger = get_logger()

        self.latency = latency

        if bind_address:
            hostname_port = server_address
        else:
            hostname_port = ("",server_address[1])

        super().__init__(hostname_port, request_handler_cls)

        if config is not None:
            Server.config = config
        else:
            self.logger.debug("Using default configuration")
            with ConfigBuilder(self.logger,
                               browser_host=server_address[0],
                               ports={"http": [self.server_address[1]]}) as config:
                assert config["ssl_config"] is None
                Server.config = config



        self.ws_doc_root = ws_doc_root
        self.key_file = key_file
        self.certificate = certificate
        self.encrypt_after_connect = use_ssl and encrypt_after_connect

        if use_ssl and not encrypt_after_connect:
            if http2:
                ssl_context = ssl.create_default_context(purpose=ssl.Purpose.CLIENT_AUTH)
                ssl_context.load_cert_chain(keyfile=self.key_file, certfile=self.certificate)
                ssl_context.set_alpn_protocols(['h2'])
                self.socket = ssl_context.wrap_socket(self.socket,
                                                      do_handshake_on_connect=False,
                                                      server_side=True)

            else:
                ssl_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                ssl_context.load_cert_chain(keyfile=self.key_file, certfile=self.certificate)
                self.socket = ssl_context.wrap_socket(self.socket,
                                                      do_handshake_on_connect=False,
                                                      server_side=True)

    def server_bind(self):
        if platform.system() != "Darwin":
            super().server_bind()
        else:
            # We override this on macOS to workaround gethostbyaddr triggering the local
            # network alert even when passed "localhost" (rdar://153097791); this should
            # be the same as the superclass implementation except for the addition of
            # our check.
            socketserver.TCPServer.server_bind(self)
            host, port = self.server_address[:2]
            if (
                ipaddress.ip_address(host).is_loopback and
                ipaddress.ip_address(socket.gethostbyname("localhost")).is_loopback
            ):
                self.server_name = "localhost"
            else:
                self.server_name = socket.getfqdn(host)
            self.server_port = port

    def serve_forever(self, poll_interval=0.5):
        """Handle one request at a time until shutdown.

        This overrides the superclass implementation to use a socket pair to process
        shutdown requests, avoiding waiting the poll_interval before shutting down.
        It does, however, still call service_actions() every poll_interval.

        """
        shutdown_read_sock, self._shutdown_write_sock = socket.socketpair()
        self._shutdown_event.clear()

        try:
            with selectors.DefaultSelector() as selector:
                selector.register(self, selectors.EVENT_READ)
                selector.register(shutdown_read_sock, selectors.EVENT_READ)

                while True:
                    events = selector.select(timeout=poll_interval)

                    # Handle shutdown requests before any request
                    if any(
                        key.fileobj == shutdown_read_sock and mask == selectors.EVENT_READ
                        for key, mask in events
                    ):
                        shutdown_read_sock.recv(1)
                        break

                    for key, mask in events:
                        if key.fileobj == self and mask == selectors.EVENT_READ:
                            super()._handle_request_noblock()
                        else:
                            assert False, "unreachable"
                    else:
                        self.service_actions()

        finally:
            shutdown_read_sock.close()
            self._shutdown_write_sock.close()
            self._shutdown_event.set()

    def shutdown(self):
        """Stops the serve_forever loop and waits for it to finish."""
        self._shutdown_write_sock.send(b'x')
        self._shutdown_event.wait()

    def finish_request(self, request, client_address):
        if isinstance(self.socket, ssl.SSLSocket):
            request.do_handshake()
        super().finish_request(request, client_address)

    def handle_error(self, request, client_address):
        error = sys.exc_info()[1]

        if ((isinstance(error, OSError) and
             isinstance(error.args, tuple) and
             error.args[0] in self.acceptable_errors) or
            (isinstance(error, IOError) and
             error.errno in self.acceptable_errors) or
            # `SSLEOFError` and `SSLError` may occur when a client
            # (e.g., wptrunner's `TestEnvironment`) tests for connectivity
            # but doesn't perform the handshake.
            isinstance(error, ssl.SSLEOFError) or isinstance(error, ssl.SSLError)):
            pass  # remote hang up before the result is sent
        else:
            msg = traceback.format_exc()
            self.logger.error(f"{type(error)} {error}")
            self.logger.info(msg)


class BaseWebTestRequestHandler(http.server.BaseHTTPRequestHandler):
    """RequestHandler for WebTestHttpd"""

    def __init__(self, *args, **kwargs):
        self.logger = get_logger()
        super().__init__(*args, **kwargs)

    def finish_handling_h1(self, request_line_is_valid):

        self.server.rewriter.rewrite(self)

        with Request(self) as request:
            response = Response(self, request)

            if request.method == "CONNECT":
                self.handle_connect(response)
                return

            if not request_line_is_valid:
                response.set_error(414)
                response.write()
                return

            self.logger.debug(f"{request.method} {request.request_path}")
            handler = self.server.router.get_handler(request)
            self.finish_handling(request, response, handler)

    def finish_handling(self, request, response, handler):
        # If the handler we used for the request had a non-default base path
        # set update the doc_root of the request to reflect this
        if hasattr(handler, "base_path") and handler.base_path:
            request.doc_root = handler.base_path
        if hasattr(handler, "url_base") and handler.url_base != "/":
            request.url_base = handler.url_base

        if self.server.latency is not None:
            if callable(self.server.latency):
                latency = self.server.latency()
            else:
                latency = self.server.latency
            self.logger.warning("Latency enabled. Sleeping %i ms" % latency)
            time.sleep(latency / 1000.)

        if handler is None:
            self.logger.debug("No Handler found!")
            response.set_error(404)
        else:
            try:
                handler(request, response)
            except HTTPException as e:
                exc = get_error_cause(e) if 500 <= e.code < 600 else e
                response.set_error(e.code, exc)
            except Exception as e:
                response.set_error(500, e)
        self.logger.debug("%i %s %s (%s) %i" % (response.status[0],
                                                request.method,
                                                request.request_path,
                                                request.headers.get('Referer'),
                                                request.raw_input.length))

        if not response.writer.content_written:
            response.write()

        # If a python handler has been used, the old ones won't send a END_STR data frame, so this
        # allows for backwards compatibility by accounting for these handlers that don't close streams
        if isinstance(response, H2Response) and not response.writer.stream_ended:
            response.writer.end_stream()

        # If we want to remove this in the future, a solution is needed for
        # scripts that produce a non-string iterable of content, since these
        # can't set a Content-Length header. A notable example of this kind of
        # problem is with the trickle pipe i.e. foo.js?pipe=trickle(d1)
        if response.close_connection:
            self.close_connection = True

        if not self.close_connection:
            # Ensure that the whole request has been read from the socket
            request.raw_input.read()

    def handle_connect(self, response):
        self.logger.debug("Got CONNECT")
        response.status = 200
        response.write()
        if self.server.encrypt_after_connect:
            self.logger.debug("Enabling SSL for connection")
            ssl_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ssl_context.load_cert_chain(keyfile=self.server.key_file, certfile=self.server.certificate)
            self.request = ssl_context.wrap_socket(self.connection,
                                                   server_side=True)
            self.setup()
        return

    def log_request(self, code="-", size="-"):
        if isinstance(code, http.HTTPStatus):
            code = code.value

        self.logger.debug(
            "{} - - [{}] {!r} {!s} {!s}".format(
                self.address_string(),
                self.log_date_time_string(),
                self.requestline,
                code,
                size,
            )
        )

    def log_error(self, format, *args):
        self.logger.error(
            "{} - - [{}] {}".format(
                self.address_string(), self.log_date_time_string(), format % args
            )
        )

    def log_message(self, format, *args):
        self.logger.info(
            "{} - - [{}] {}".format(
                self.address_string(), self.log_date_time_string(), format % args
            )
        )


class Http2WebTestRequestHandler(BaseWebTestRequestHandler):
    protocol_version = "HTTP/2.0"

    def handle_one_request(self):
        """
        This is the main HTTP/2 Handler.

        When a browser opens a connection to the server
        on the HTTP/2 port, the server enters this which will initiate the h2 connection
        and keep running throughout the duration of the interaction, and will read/write directly
        from the socket.

        Because there can be multiple H2 connections active at the same
        time, a UUID is created for each so that it is easier to tell them apart in the logs.
        """

        config = H2Configuration(client_side=False)
        self.conn = H2ConnectionGuard(H2Connection(config=config))
        self.close_connection = False

        # Generate a UUID to make it easier to distinguish different H2 connection debug messages
        self.uid = str(uuid.uuid4())[:8]

        self.logger.debug('(%s) Initiating h2 Connection' % self.uid)

        with self.conn as connection:
            # Bootstrapping WebSockets with HTTP/2 specification requires
            # ENABLE_CONNECT_PROTOCOL to be set in order to enable WebSocket
            # over HTTP/2
            new_settings = dict(connection.local_settings)
            new_settings[SettingCodes.ENABLE_CONNECT_PROTOCOL] = 1
            connection.local_settings.update(new_settings)
            connection.local_settings.acknowledge()

            connection.initiate_connection()
            data = connection.data_to_send()
            window_size = connection.remote_settings.initial_window_size

        try:
            self.request.sendall(data)
        except ConnectionResetError:
            self.logger.warning("Connection reset during h2 setup")
            return

        # Dict of { stream_id: (thread, queue) }
        stream_queues = {}

        try:
            while not self.close_connection:
                data = self.request.recv(window_size)
                if data == '':
                    self.logger.debug('(%s) Socket Closed' % self.uid)
                    self.close_connection = True
                    continue

                with self.conn as connection:
                    frames = connection.receive_data(data)
                    window_size = connection.remote_settings.initial_window_size

                self.logger.debug('(%s) Frames Received: ' % self.uid + str(frames))

                for frame in frames:
                    if isinstance(frame, ConnectionTerminated):
                        self.logger.debug('(%s) Connection terminated by remote peer ' % self.uid)
                        self.close_connection = True

                        # Flood all the streams with connection terminated, this will cause them to stop
                        for stream_id, (thread, queue) in stream_queues.items():
                            queue.put(frame)

                    elif hasattr(frame, 'stream_id'):
                        if frame.stream_id not in stream_queues:
                            queue = Queue()
                            stream_queues[frame.stream_id] = (self.start_stream_thread(frame, queue), queue)
                        stream_queues[frame.stream_id][1].put(frame)

                        if isinstance(frame, StreamEnded) or getattr(frame, "stream_ended", False):
                            del stream_queues[frame.stream_id]

        except OSError as e:
            self.logger.error(f'({self.uid}) Closing Connection - \n{str(e)}')
            if not self.close_connection:
                self.close_connection = True
        except Exception as e:
            self.logger.error(f'({self.uid}) Unexpected Error - \n{str(e)}')
        finally:
            for stream_id, (thread, queue) in stream_queues.items():
                queue.put(None)
                thread.join()

    def _is_extended_connect_frame(self, frame):
        if not isinstance(frame, RequestReceived):
            return False

        method = extract_method_header(frame.headers)
        if method != b"CONNECT":
            return False

        protocol = ""
        for key, value in frame.headers:
            if key in (b':protocol', ':protocol'):
                protocol = isomorphic_encode(value)
                break
        if protocol != b"websocket":
            raise ProtocolError(f"Invalid protocol {protocol} with CONNECT METHOD")

        return True

    def start_stream_thread(self, frame, queue):
        """
        This starts a new thread to handle frames for a specific stream.
        :param frame: The first frame on the stream
        :param queue: A queue object that the thread will use to check for new frames
        :return: The thread object that has already been started
        """
        if self._is_extended_connect_frame(frame):
            target = Http2WebTestRequestHandler._stream_ws_thread
        else:
            target = Http2WebTestRequestHandler._stream_thread
        t = threading.Thread(
            target=target,
            args=(self, frame.stream_id, queue)
        )
        t.start()
        return t

    def _stream_ws_thread(self, stream_id, queue):
        frame = queue.get(True, None)

        if frame is None:
            return

        # Needs to be unbuffered for websockets.
        rfile, wfile = os.pipe()
        with os.fdopen(rfile, 'rb') as rfile, os.fdopen(wfile, 'wb', 0) as wfile:
            stream_handler = H2HandlerCopy(self, frame, rfile)

            h2request = H2Request(stream_handler)
            h2response = H2Response(stream_handler, h2request)

            dispatcher = dispatch.Dispatcher(self.server.ws_doc_root, None, False)
            if not dispatcher.get_handler_suite(stream_handler.path):
                h2response.set_error(404)
                h2response.write()
                return

            request_wrapper = _WebSocketRequest(stream_handler, h2response)

            handshaker = WsH2Handshaker(request_wrapper, dispatcher)
            try:
                handshaker.do_handshake()
            except HandshakeException as e:
                self.logger.info("Handshake failed")
                h2response.set_error(e.status, e)
                h2response.write()
                return
            except AbortedByUserException:
                h2response.write()
                return

            # h2 Handshaker prepares the headers but does not send them down the
            # wire. Flush the headers here.
            try:
                h2response.write_status_headers()
            except StreamClosedError:
                # work around https://github.com/web-platform-tests/wpt/issues/27786
                # The stream was already closed.
                return

            request_wrapper._dispatcher = dispatcher

            # we need two threads:
            # - one to handle the frame queue
            # - one to handle the request (dispatcher.transfer_data is blocking)
            # the alternative is to have only one (blocking) thread. That thread
            # will call transfer_data. That would require a special case in
            # handle_one_request, to bypass the queue and write data to wfile
            # directly.
            t = threading.Thread(
                target=Http2WebTestRequestHandler._stream_ws_sub_thread,
                args=(self, request_wrapper, stream_handler, queue)
            )
            t.start()

            while not self.close_connection:
                try:
                    frame = queue.get(True, 1)
                except Empty:
                    continue

                if isinstance(frame, DataReceived):
                    wfile.write(frame.data)
                    if frame.stream_ended:
                        raise NotImplementedError("frame.stream_ended")
                elif frame is None or isinstance(frame, (StreamReset, StreamEnded, ConnectionTerminated)):
                    self.logger.error(f'({self.uid} - {stream_id}) Stream Reset, Thread Closing')
                    break

        t.join()

    def _stream_ws_sub_thread(self, request, stream_handler, queue):
        dispatcher = request._dispatcher
        try:
            dispatcher.transfer_data(request)
        except (StreamClosedError, ProtocolError):
            # work around https://github.com/web-platform-tests/wpt/issues/27786
            # The stream was already closed.
            queue.put(None)
            return

        stream_id = stream_handler.h2_stream_id
        with stream_handler.conn as connection:
            try:
                connection.end_stream(stream_id)
                data = connection.data_to_send()
                stream_handler.request.sendall(data)
            except (StreamClosedError, ProtocolError):  # maybe the stream has already been closed
                pass
        queue.put(None)

    def _stream_thread(self, stream_id, queue):
        """
        This thread processes frames for a specific stream. It waits for frames to be placed
        in the queue, and processes them. When it receives a request frame, it will start processing
        immediately, even if there are data frames to follow. One of the reasons for this is that it
        can detect invalid requests before needing to read the rest of the frames.
        """

        # The file-like pipe object that will be used to share data to request object if data is received
        wfile = None
        rfile = None
        request = None
        response = None
        req_handler = None

        def cleanup():
            # Try to close the files
            # Ignore any exception (e.g. if the file handle was already closed for some reason).
            if rfile:
                try:
                    rfile.close()
                except OSError:
                    pass
            if wfile:
                try:
                    wfile.close()
                except OSError:
                    pass

        while not self.close_connection:
            try:
                frame = queue.get(True, 1)
            except Empty:
                # Restart to check for close_connection
                continue

            self.logger.debug(f'({self.uid} - {stream_id}) {str(frame)}')
            if isinstance(frame, RequestReceived):
                cleanup()

                pipe_rfile, pipe_wfile = os.pipe()
                (rfile, wfile) = os.fdopen(pipe_rfile, 'rb'), os.fdopen(pipe_wfile, 'wb')

                stream_handler = H2HandlerCopy(self, frame, rfile)

                stream_handler.server.rewriter.rewrite(stream_handler)
                request = H2Request(stream_handler)
                response = H2Response(stream_handler, request)

                req_handler = stream_handler.server.router.get_handler(request)

                if hasattr(req_handler, "frame_handler"):
                    # Convert this to a handler that will utilise H2 specific functionality, such as handling individual frames
                    req_handler = self.frame_handler(request, response, req_handler)

                if hasattr(req_handler, 'handle_headers'):
                    req_handler.handle_headers(frame, request, response)

            elif isinstance(frame, DataReceived):
                wfile.write(frame.data)

                if hasattr(req_handler, 'handle_data'):
                    req_handler.handle_data(frame, request, response)

            elif frame is None or isinstance(frame, (StreamReset, StreamEnded, ConnectionTerminated)):
                self.logger.debug(f'({self.uid} - {stream_id}) Stream Reset, Thread Closing')
                break

            if request is not None:
                request.frames.append(frame)

            if getattr(frame, "stream_ended", False):
                try:
                    self.finish_handling(request, response, req_handler)
                except StreamClosedError:
                    self.logger.debug('(%s - %s) Unable to write response; stream closed' %
                                    (self.uid, stream_id))
                break

        cleanup()

    def frame_handler(self, request, response, handler):
        try:
            return handler.frame_handler(request)
        except HTTPException as e:
            exc = get_error_cause(e) if 500 <= e.code < 600 else e
            response.set_error(exc.code, exc)
            response.write()
        except Exception as e:
            response.set_error(500, e)
            response.write()


class H2ConnectionGuard:
    """H2Connection objects are not threadsafe, so this keeps thread safety"""
    def __init__(self, obj):
        assert isinstance(obj, H2Connection)
        self.obj = obj
        self.lock = threading.Lock()

    def __enter__(self):
        self.lock.acquire()
        return self.obj

    def __exit__(self, exception_type, exception_value, traceback):
        self.lock.release()


class H2Headers(Dict[bytes, bytes]):
    def __init__(self, headers):
        self.raw_headers = OrderedDict()
        for key, val in headers:
            key = isomorphic_decode(key)
            val = isomorphic_decode(val)
            self.raw_headers[key] = val
            dict.__setitem__(self, self._convert_h2_header_to_h1(key), val)

    def _convert_h2_header_to_h1(self, header_key):
        if header_key[1:] in h2_headers and header_key[0] == ':':
            return header_key[1:]
        else:
            return header_key

    # TODO This does not seem relevant for H2 headers, so using a dummy function for now
    def getallmatchingheaders(self, header):
        return ['dummy function']


class H2HandlerCopy:
    def __init__(self, handler, req_frame, rfile):
        self.headers = H2Headers(req_frame.headers)
        self.command = self.headers['method']
        self.path = self.headers['path']
        self.h2_stream_id = req_frame.stream_id
        self.server = handler.server
        self.protocol_version = handler.protocol_version
        self.client_address = handler.client_address
        self.raw_requestline = ''
        self.rfile = rfile
        self.request = handler.request
        self.conn = handler.conn

class Http1WebTestRequestHandler(BaseWebTestRequestHandler):
    protocol_version = "HTTP/1.1"

    def handle_one_request(self):
        response = None

        try:
            self.close_connection = False

            request_line_is_valid = self.get_request_line()

            if self.close_connection:
                return

            request_is_valid = self.parse_request()
            if not request_is_valid:
                #parse_request() actually sends its own error responses
                return

            self.finish_handling_h1(request_line_is_valid)

        except socket.timeout as e:
            self.log_error("Request timed out: %r", e)
            self.close_connection = True
            return

        except Exception as e:
            if response:
                response.set_error(500, e)
                response.write()

    def get_request_line(self):
        try:
            self.raw_requestline = self.rfile.readline(65537)
        except OSError:
            self.close_connection = True
            return False
        if len(self.raw_requestline) > 65536:
            self.requestline = ''
            self.request_version = ''
            self.command = ''
            return False
        if not self.raw_requestline:
            self.close_connection = True
        return True

class WebTestHttpd:
    """
    :param host: Host from which to serve (default: 127.0.0.1)
    :param port: Port from which to serve (default: 8000)
    :param server_cls: Class to use for the server (default depends on ssl vs non-ssl)
    :param handler_cls: Class to use for the RequestHandler
    :param use_ssl: Use a SSL server if no explicit server_cls is supplied
    :param key_file: Path to key file to use if ssl is enabled
    :param certificate: Path to certificate file to use if ssl is enabled
    :param encrypt_after_connect: For each connection, don't start encryption
                                  until a CONNECT message has been received.
                                  This enables the server to act as a
                                  self-proxy.
    :param router_cls: Router class to use when matching URLs to handlers
    :param doc_root: Document root for serving files
    :param ws_doc_root: Document root for websockets
    :param routes: List of routes with which to initialize the router
    :param rewriter_cls: Class to use for request rewriter
    :param rewrites: List of rewrites with which to initialize the rewriter_cls
    :param config: Dictionary holding environment configuration settings for
                   handlers to read, or None to use the default values.
    :param bind_address: Boolean indicating whether to bind server to IP address.
    :param latency: Delay in ms to wait before serving each response, or
                    callable that returns a delay in ms

    HTTP server designed for testing scenarios.

    Takes a router class which provides one method get_handler which takes a Request
    and returns a handler function.

    .. attribute:: host

      The host name or ip address of the server

    .. attribute:: port

      The port on which the server is running

    .. attribute:: router

      The Router object used to associate requests with resources for this server

    .. attribute:: rewriter

      The Rewriter object used for URL rewriting

    .. attribute:: use_ssl

      Boolean indicating whether the server is using ssl

    .. attribute:: started

      Boolean indicating whether the server is running

    """
    def __init__(self, host="127.0.0.1", port=8000,
                 server_cls=None, handler_cls=Http1WebTestRequestHandler,
                 use_ssl=False, key_file=None, certificate=None, encrypt_after_connect=False,
                 router_cls=Router, doc_root=os.curdir, ws_doc_root=None, routes=None,
                 rewriter_cls=RequestRewriter, bind_address=True, rewrites=None,
                 latency=None, config=None, http2=False):

        if routes is None:
            routes = default_routes.routes

        self.host = host

        self.router = router_cls(doc_root, routes)
        self.rewriter = rewriter_cls(rewrites if rewrites is not None else [])

        self.use_ssl = use_ssl
        self.http2 = http2
        self.logger = get_logger()

        if server_cls is None:
            server_cls = WebTestServer

        if use_ssl:
            if not os.path.exists(key_file):
                raise ValueError(f"SSL certificate not found: {key_file}")
            if not os.path.exists(certificate):
                raise ValueError(f"SSL key not found: {certificate}")

        try:
            self.httpd = server_cls((host, port),
                                    handler_cls,
                                    self.router,
                                    self.rewriter,
                                    config=config,
                                    bind_address=bind_address,
                                    ws_doc_root=ws_doc_root,
                                    use_ssl=use_ssl,
                                    key_file=key_file,
                                    certificate=certificate,
                                    encrypt_after_connect=encrypt_after_connect,
                                    latency=latency,
                                    http2=http2)
            self.started = False

            _host, self.port = self.httpd.socket.getsockname()
        except Exception:
            self.logger.critical("Failed to start HTTP server on port %s; "
                                 "is something already using that port?" % port)
            raise

    def start(self):
        """Start the server.

        :param block: True to run the server on the current thread, blocking,
                      False to run on a separate thread."""
        http_type = "http2" if self.http2 else "https" if self.use_ssl else "http"
        http_scheme = "https" if self.use_ssl else "http"
        self.logger.info(f"Starting {http_type} server on {http_scheme}://{self.host}:{self.port}")
        self.started = True
        self.server_thread = threading.Thread(target=self.httpd.serve_forever)
        self.server_thread.daemon = True  # don't hang on exit
        self.server_thread.start()

    def stop(self):
        """
        Stops the server.

        If the server is not running, this method has no effect.
        """
        if self.started:
            try:
                self.httpd.shutdown()
                self.httpd.server_close()
                self.server_thread.join()
                self.server_thread = None
                self.logger.info(f"Stopped http server on {self.host}:{self.port}")
            except AttributeError:
                pass
            self.started = False
        self.httpd = None

    def get_url(self, path="/", query=None, fragment=None):
        if not self.started:
            return None

        return urlunsplit(("http" if not self.use_ssl else "https",
                           f"{self.host}:{self.port}",
                           path, query, fragment))


class _WebSocketConnection:
    def __init__(self, request_handler, response):
        """Mimic mod_python mp_conn.

        :param request_handler: A H2HandlerCopy instance.

        :param response: A H2Response instance.
        """

        self._request_handler = request_handler
        self._response = response

        self.remote_addr = self._request_handler.client_address

    def write(self, data):
        self._response.writer.write_data(data, False)

    def read(self, length):
        return self._request_handler.rfile.read(length)


class _WebSocketRequest:
    def __init__(self, request_handler, response):
        """Mimic mod_python request.

        :param request_handler: A H2HandlerCopy instance.

        :param response: A H2Response instance.
        """

        self.connection = _WebSocketConnection(request_handler, response)
        self.protocol = "HTTP/2"
        self._response = response

        self.uri = request_handler.path
        self.unparsed_uri = request_handler.path
        self.method = request_handler.command
        # read headers from request_handler
        self.headers_in = request_handler.headers
        # write headers directly into H2Response
        self.headers_out = response.headers

    # proxies status to H2Response
    @property
    def status(self):
        return self._response.status

    @status.setter
    def status(self, status):
        self._response.status = status
