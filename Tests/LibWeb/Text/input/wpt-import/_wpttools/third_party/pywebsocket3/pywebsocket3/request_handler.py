# Copyright 2020, Google Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
# copyright notice, this list of conditions and the following disclaimer
# in the documentation and/or other materials provided with the
# distribution.
#     * Neither the name of Google Inc. nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
"""Request Handler and Request/Connection classes for standalone server.
"""

import os

from six.moves import CGIHTTPServer
from six.moves import http_client

from pywebsocket3 import (
    common,
    dispatch,
    handshake,
    http_header_util,
    memorizingfile,
    util
)

# 1024 is practically large enough to contain WebSocket handshake lines.
_MAX_MEMORIZED_LINES = 1024


class _StandaloneConnection(object):
    """Mimic mod_python mp_conn."""
    def __init__(self, request_handler):
        """Construct an instance.

        Args:
            request_handler: A WebSocketRequestHandler instance.
        """

        self._request_handler = request_handler

    def get_local_addr(self):
        """Getter to mimic mp_conn.local_addr."""

        return (self._request_handler.server.server_name,
                self._request_handler.server.server_port)

    local_addr = property(get_local_addr)

    def get_remote_addr(self):
        """Getter to mimic mp_conn.remote_addr.

        Setting the property in __init__ won't work because the request
        handler is not initialized yet there."""

        return self._request_handler.client_address

    remote_addr = property(get_remote_addr)

    def write(self, data):
        """Mimic mp_conn.write()."""

        return self._request_handler.wfile.write(data)

    def read(self, length):
        """Mimic mp_conn.read()."""

        return self._request_handler.rfile.read(length)

    def get_memorized_lines(self):
        """Get memorized lines."""

        return self._request_handler.rfile.get_memorized_lines()


class _StandaloneRequest(object):
    """Mimic mod_python request."""
    def __init__(self, request_handler, use_tls):
        """Construct an instance.

        Args:
            request_handler: A WebSocketRequestHandler instance.
        """

        self._logger = util.get_class_logger(self)

        self._request_handler = request_handler
        self.connection = _StandaloneConnection(request_handler)
        self._use_tls = use_tls
        self.headers_in = request_handler.headers

    def get_uri(self):
        """Getter to mimic request.uri.

        This method returns the raw data at the Request-URI part of the
        Request-Line, while the uri method on the request object of mod_python
        returns the path portion after parsing the raw data. This behavior is
        kept for compatibility.
        """

        return self._request_handler.path

    uri = property(get_uri)

    def get_unparsed_uri(self):
        """Getter to mimic request.unparsed_uri."""

        return self._request_handler.path

    unparsed_uri = property(get_unparsed_uri)

    def get_method(self):
        """Getter to mimic request.method."""

        return self._request_handler.command

    method = property(get_method)

    def get_protocol(self):
        """Getter to mimic request.protocol."""

        return self._request_handler.request_version

    protocol = property(get_protocol)

    def is_https(self):
        """Mimic request.is_https()."""

        return self._use_tls


class WebSocketRequestHandler(CGIHTTPServer.CGIHTTPRequestHandler):
    """CGIHTTPRequestHandler specialized for WebSocket."""

    # Use httplib.HTTPMessage instead of mimetools.Message.
    MessageClass = http_client.HTTPMessage

    def setup(self):
        """Override SocketServer.StreamRequestHandler.setup to wrap rfile
        with MemorizingFile.

        This method will be called by BaseRequestHandler's constructor
        before calling BaseHTTPRequestHandler.handle.
        BaseHTTPRequestHandler.handle will call
        BaseHTTPRequestHandler.handle_one_request and it will call
        WebSocketRequestHandler.parse_request.
        """

        # Call superclass's setup to prepare rfile, wfile, etc. See setup
        # definition on the root class SocketServer.StreamRequestHandler to
        # understand what this does.
        CGIHTTPServer.CGIHTTPRequestHandler.setup(self)

        self.rfile = memorizingfile.MemorizingFile(
            self.rfile, max_memorized_lines=_MAX_MEMORIZED_LINES)

    def __init__(self, request, client_address, server):
        self._logger = util.get_class_logger(self)

        self._options = server.websocket_server_options

        # Overrides CGIHTTPServerRequestHandler.cgi_directories.
        self.cgi_directories = self._options.cgi_directories
        # Replace CGIHTTPRequestHandler.is_executable method.
        if self._options.is_executable_method is not None:
            self.is_executable = self._options.is_executable_method

        # This actually calls BaseRequestHandler.__init__.
        CGIHTTPServer.CGIHTTPRequestHandler.__init__(self, request,
                                                     client_address, server)

    def parse_request(self):
        """Override BaseHTTPServer.BaseHTTPRequestHandler.parse_request.

        Return True to continue processing for HTTP(S), False otherwise.

        See BaseHTTPRequestHandler.handle_one_request method which calls
        this method to understand how the return value will be handled.
        """

        # We hook parse_request method, but also call the original
        # CGIHTTPRequestHandler.parse_request since when we return False,
        # CGIHTTPRequestHandler.handle_one_request continues processing and
        # it needs variables set by CGIHTTPRequestHandler.parse_request.
        #
        # Variables set by this method will be also used by WebSocket request
        # handling (self.path, self.command, self.requestline, etc. See also
        # how _StandaloneRequest's members are implemented using these
        # attributes).
        if not CGIHTTPServer.CGIHTTPRequestHandler.parse_request(self):
            return False

        if self._options.use_basic_auth:
            auth = self.headers.get('Authorization')
            if auth != self._options.basic_auth_credential:
                self.send_response(401)
                self.send_header('WWW-Authenticate',
                                 'Basic realm="Pywebsocket"')
                self.end_headers()
                self._logger.info('Request basic authentication')
                return False

        whole_path = self.path
        host, port, resource = http_header_util.parse_uri(self.path)
        if resource is None:
            self._logger.info('Invalid URI: %r', self.path)
            self._logger.info('Fallback to CGIHTTPRequestHandler')
            return True
        server_options = self.server.websocket_server_options
        if host is not None:
            validation_host = server_options.validation_host
            if validation_host is not None and host != validation_host:
                self._logger.info('Invalid host: %r (expected: %r)', host,
                                  validation_host)
                self._logger.info('Fallback to CGIHTTPRequestHandler')
                return True
        if port is not None:
            validation_port = server_options.validation_port
            if validation_port is not None and port != validation_port:
                self._logger.info('Invalid port: %r (expected: %r)', port,
                                  validation_port)
                self._logger.info('Fallback to CGIHTTPRequestHandler')
                return True
        self.path = resource

        request = _StandaloneRequest(self, self._options.use_tls)

        try:
            # Fallback to default http handler for request paths for which
            # we don't have request handlers.
            if not self._options.dispatcher.get_handler_suite(self.path):
                self._logger.info('No handler for resource: %r', whole_path)
                self._logger.info('Fallback to CGIHTTPRequestHandler')
                return True
        except dispatch.DispatchException as e:
            self._logger.info('Dispatch failed for error: %s', e)
            self.send_error(e.status)
            return False

        # If any Exceptions without except clause setup (including
        # DispatchException) is raised below this point, it will be caught
        # and logged by WebSocketServer.

        try:
            try:
                handshake.do_handshake(request, self._options.dispatcher)
            except handshake.VersionException as e:
                self._logger.info('Handshake failed for version error: %s', e)
                self.send_response(common.HTTP_STATUS_BAD_REQUEST)
                self.send_header(common.SEC_WEBSOCKET_VERSION_HEADER,
                                 e.supported_versions)
                self.end_headers()
                return False
            except handshake.HandshakeException as e:
                # Handshake for ws(s) failed.
                self._logger.info('Handshake failed for error: %s', e)
                self.send_error(e.status)
                return False

            request._dispatcher = self._options.dispatcher
            self._options.dispatcher.transfer_data(request)
        except handshake.AbortedByUserException as e:
            self._logger.info('Aborted: %s', e)
        return False

    def log_request(self, code='-', size='-'):
        """Override BaseHTTPServer.log_request."""

        self._logger.info('"%s" %s %s', self.requestline, str(code), str(size))

    def log_error(self, *args):
        """Override BaseHTTPServer.log_error."""

        # Despite the name, this method is for warnings than for errors.
        # For example, HTTP status code is logged by this method.
        self._logger.warning('%s - %s', self.address_string(),
                             args[0] % args[1:])

    def is_cgi(self):
        """Test whether self.path corresponds to a CGI script.

        Add extra check that self.path doesn't contains ..
        Also check if the file is a executable file or not.
        If the file is not executable, it is handled as static file or dir
        rather than a CGI script.
        """

        if CGIHTTPServer.CGIHTTPRequestHandler.is_cgi(self):
            if '..' in self.path:
                return False
            # strip query parameter from request path
            resource_name = self.path.split('?', 2)[0]
            # convert resource_name into real path name in filesystem.
            scriptfile = self.translate_path(resource_name)
            if not os.path.isfile(scriptfile):
                return False
            if not self.is_executable(scriptfile):
                return False
            return True
        return False


# vi:sts=4 sw=4 et
