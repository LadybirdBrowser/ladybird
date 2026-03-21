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
"""Standalone WebsocketServer

This file deals with the main module of standalone server. Although it is fine
to import this file directly to use WebSocketServer, it is strongly recommended
to use standalone.py, since it is intended to act as a skeleton of this module.
"""

from __future__ import absolute_import

import logging
import re
import select
import socket
import ssl
import threading
import traceback

from six.moves import BaseHTTPServer, socketserver

from pywebsocket3 import dispatch, util
from pywebsocket3.request_handler import WebSocketRequestHandler


def _alias_handlers(dispatcher, websock_handlers_map_file):
    """Set aliases specified in websock_handler_map_file in dispatcher.

    Args:
        dispatcher: dispatch.Dispatcher instance
        websock_handler_map_file: alias map file
    """

    with open(websock_handlers_map_file) as f:
        for line in f:
            if line[0] == '#' or line.isspace():
                continue
            m = re.match(r'(\S+)\s+(\S+)$', line)
            if not m:
                logging.warning('Wrong format in map file:' + line)
                continue
            try:
                dispatcher.add_resource_path_alias(m.group(1), m.group(2))
            except dispatch.DispatchException as e:
                logging.error(str(e))


class WebSocketServer(socketserver.ThreadingMixIn, BaseHTTPServer.HTTPServer):
    """HTTPServer specialized for WebSocket."""

    # Overrides SocketServer.ThreadingMixIn.daemon_threads
    daemon_threads = True
    # Overrides BaseHTTPServer.HTTPServer.allow_reuse_address
    allow_reuse_address = True

    def __init__(self, options):
        """Override SocketServer.TCPServer.__init__ to set SSL enabled
        socket object to self.socket before server_bind and server_activate,
        if necessary.
        """

        # Fall back to None for embedders that don't know about the
        # handler_encoding option.
        handler_encoding = getattr(options, "handler_encoding", None)

        # Share a Dispatcher among request handlers to save time for
        # instantiation.  Dispatcher can be shared because it is thread-safe.
        options.dispatcher = dispatch.Dispatcher(
            options.websock_handlers, options.scan_dir,
            options.allow_handlers_outside_root_dir, handler_encoding)
        if options.websock_handlers_map_file:
            _alias_handlers(options.dispatcher,
                            options.websock_handlers_map_file)
        warnings = options.dispatcher.source_warnings()
        if warnings:
            for warning in warnings:
                logging.warning('Warning in source loading: %s' % warning)

        self._logger = util.get_class_logger(self)

        self.request_queue_size = options.request_queue_size
        self.__ws_is_shut_down = threading.Event()
        self.__ws_serving = False

        socketserver.BaseServer.__init__(self,
                                         (options.server_host, options.port),
                                         WebSocketRequestHandler)

        # Expose the options object to allow handler objects access it. We name
        # it with websocket_ prefix to avoid conflict.
        self.websocket_server_options = options

        self._create_sockets()
        self.server_bind()
        self.server_activate()

    def _create_sockets(self):
        self.server_name, self.server_port = self.server_address
        self._sockets = []
        if not self.server_name:
            # On platforms that doesn't support IPv6, the first bind fails.
            # On platforms that supports IPv6
            # - If it binds both IPv4 and IPv6 on call with AF_INET6, the
            #   first bind succeeds and the second fails (we'll see 'Address
            #   already in use' error).
            # - If it binds only IPv6 on call with AF_INET6, both call are
            #   expected to succeed to listen both protocol.
            addrinfo_array = [(socket.AF_INET6, socket.SOCK_STREAM, '', '',
                               ''),
                              (socket.AF_INET, socket.SOCK_STREAM, '', '', '')]
        else:
            addrinfo_array = socket.getaddrinfo(self.server_name,
                                                self.server_port,
                                                socket.AF_UNSPEC,
                                                socket.SOCK_STREAM,
                                                socket.IPPROTO_TCP)
        for addrinfo in addrinfo_array:
            self._logger.info('Create socket on: %r', addrinfo)
            family, socktype, proto, canonname, sockaddr = addrinfo
            try:
                socket_ = socket.socket(family, socktype)
            except Exception as e:
                self._logger.info('Skip by failure: %r', e)
                continue
            server_options = self.websocket_server_options
            if server_options.use_tls:
                if server_options.tls_client_auth:
                    if server_options.tls_client_cert_optional:
                        client_cert_ = ssl.CERT_OPTIONAL
                    else:
                        client_cert_ = ssl.CERT_REQUIRED
                else:
                    client_cert_ = ssl.CERT_NONE
                ssl_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                ssl_context.verify_mode = client_cert_
                ssl_context.load_cert_chain(keyfile=server_options.private_key,
                                            certfile=server_options.certificate)
                if client_cert_ != ssl.CERT_NONE:
                    ssl_context.load_verify_locations(cafile=server_options.tls_client_ca)
                socket_ = ssl_context.wrap_socket(socket_, server_side=True)
            self._sockets.append((socket_, addrinfo))

    def server_bind(self):
        """Override SocketServer.TCPServer.server_bind to enable multiple
        sockets bind.
        """

        failed_sockets = []

        for socketinfo in self._sockets:
            socket_, addrinfo = socketinfo
            self._logger.info('Bind on: %r', addrinfo)
            if self.allow_reuse_address:
                socket_.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                socket_.bind(self.server_address)
            except Exception as e:
                self._logger.info('Skip by failure: %r', e)
                socket_.close()
                failed_sockets.append(socketinfo)
            if self.server_address[1] == 0:
                # The operating system assigns the actual port number for port
                # number 0. This case, the second and later sockets should use
                # the same port number. Also self.server_port is rewritten
                # because it is exported, and will be used by external code.
                self.server_address = (self.server_name,
                                       socket_.getsockname()[1])
                self.server_port = self.server_address[1]
                self._logger.info('Port %r is assigned', self.server_port)

        for socketinfo in failed_sockets:
            self._sockets.remove(socketinfo)

    def server_activate(self):
        """Override SocketServer.TCPServer.server_activate to enable multiple
        sockets listen.
        """

        failed_sockets = []

        for socketinfo in self._sockets:
            socket_, addrinfo = socketinfo
            self._logger.info('Listen on: %r', addrinfo)
            try:
                socket_.listen(self.request_queue_size)
            except Exception as e:
                self._logger.info('Skip by failure: %r', e)
                socket_.close()
                failed_sockets.append(socketinfo)

        for socketinfo in failed_sockets:
            self._sockets.remove(socketinfo)

        if len(self._sockets) == 0:
            self._logger.critical(
                'No sockets activated. Use info log level to see the reason.')

    def server_close(self):
        """Override SocketServer.TCPServer.server_close to enable multiple
        sockets close.
        """

        for socketinfo in self._sockets:
            socket_, addrinfo = socketinfo
            self._logger.info('Close on: %r', addrinfo)
            socket_.close()

    def fileno(self):
        """Override SocketServer.TCPServer.fileno."""

        self._logger.critical('Not supported: fileno')
        return self._sockets[0][0].fileno()

    def handle_error(self, request, client_address):
        """Override SocketServer.handle_error."""

        self._logger.error('Exception in processing request from: %r\n%s',
                           client_address, traceback.format_exc())
        # Note: client_address is a tuple.

    def get_request(self):
        """Override TCPServer.get_request."""

        accepted_socket, client_address = self.socket.accept()

        server_options = self.websocket_server_options
        if server_options.use_tls:
            # Print cipher in use. Handshake is done on accept.
            self._logger.debug('Cipher: %s', accepted_socket.cipher())
            self._logger.debug('Client cert: %r',
                               accepted_socket.getpeercert())

        return accepted_socket, client_address

    def serve_forever(self, poll_interval=0.5):
        """Override SocketServer.BaseServer.serve_forever."""

        self.__ws_serving = True
        self.__ws_is_shut_down.clear()
        handle_request = self.handle_request
        if hasattr(self, '_handle_request_noblock'):
            handle_request = self._handle_request_noblock
        else:
            self._logger.warning('Fallback to blocking request handler')
        try:
            while self.__ws_serving:
                r, w, e = select.select(
                    [socket_[0] for socket_ in self._sockets], [], [],
                    poll_interval)
                for socket_ in r:
                    self.socket = socket_
                    handle_request()
                self.socket = None
        finally:
            self.__ws_is_shut_down.set()

    def shutdown(self):
        """Override SocketServer.BaseServer.shutdown."""

        self.__ws_serving = False
        self.__ws_is_shut_down.wait()


# vi:sts=4 sw=4 et
