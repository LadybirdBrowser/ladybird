# Copyright 2012, Google Inc.
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
"""This file provides the opening handshake processor for the WebSocket
protocol (RFC 6455).

Specification:
http://tools.ietf.org/html/rfc6455
"""

from __future__ import absolute_import

import base64
import re
from hashlib import sha1

from pywebsocket3 import common, util
from pywebsocket3.handshake.base import (
    get_mandatory_header,
    HandshakeException,
    parse_token_list,
    validate_mandatory_header,
    HandshakerBase
)


# Used to validate the value in the Sec-WebSocket-Key header strictly. RFC 4648
# disallows non-zero padding, so the character right before == must be any of
# A, Q, g and w.
_SEC_WEBSOCKET_KEY_REGEX = re.compile('^[+/0-9A-Za-z]{21}[AQgw]==$')


def check_request_line(request):
    # 5.1 1. The three character UTF-8 string "GET".
    # 5.1 2. A UTF-8-encoded U+0020 SPACE character (0x20 byte).
    if request.method != u'GET':
        raise HandshakeException('Method is not GET: %r' % request.method)

    if request.protocol != u'HTTP/1.1':
        raise HandshakeException('Version is not HTTP/1.1: %r' %
                                 request.protocol)


def compute_accept(key):
    """Computes value for the Sec-WebSocket-Accept header from value of the
    Sec-WebSocket-Key header.
    """

    accept_binary = sha1(key + common.WEBSOCKET_ACCEPT_UUID).digest()
    accept = base64.b64encode(accept_binary)

    return accept


def compute_accept_from_unicode(unicode_key):
    """A wrapper function for compute_accept which takes a unicode string as an
    argument, and encodes it to byte string. It then passes it on to
    compute_accept.
    """

    key = unicode_key.encode('UTF-8')
    return compute_accept(key)


def format_header(name, value):
    return u'%s: %s\r\n' % (name, value)


class Handshaker(HandshakerBase):
    """Opening handshake processor for the WebSocket protocol (RFC 6455)."""
    def __init__(self, request, dispatcher):
        """Construct an instance.

        Args:
            request: mod_python request.
            dispatcher: Dispatcher (dispatch.Dispatcher).

        Handshaker will add attributes such as ws_resource during handshake.
        """
        super(Handshaker, self).__init__(request, dispatcher)

    def _transform_header(self, header):
        return header

    def _protocol_rfc(self):
        return 'RFC 6455'

    def _validate_connection_header(self):
        connection = get_mandatory_header(self._request,
                                          common.CONNECTION_HEADER)

        try:
            connection_tokens = parse_token_list(connection)
        except HandshakeException as e:
            raise HandshakeException('Failed to parse %s: %s' %
                                     (common.CONNECTION_HEADER, e))

        connection_is_valid = False
        for token in connection_tokens:
            if token.lower() == common.UPGRADE_CONNECTION_TYPE.lower():
                connection_is_valid = True
                break
        if not connection_is_valid:
            raise HandshakeException(
                '%s header doesn\'t contain "%s"' %
                (common.CONNECTION_HEADER, common.UPGRADE_CONNECTION_TYPE))

    def _validate_request(self):
        check_request_line(self._request)
        validate_mandatory_header(self._request, common.UPGRADE_HEADER,
                                  common.WEBSOCKET_UPGRADE_TYPE)
        self._validate_connection_header()
        unused_host = get_mandatory_header(self._request, common.HOST_HEADER)

    def _set_accept(self):
        # Key validation, response generation.
        key = self._get_key()
        accept = compute_accept(key)
        self._logger.debug('%s: %r (%s)', common.SEC_WEBSOCKET_ACCEPT_HEADER,
                           accept, util.hexify(base64.b64decode(accept)))
        self._request._accept = accept

    def _validate_key(self, key):
        if key.find(',') >= 0:
            raise HandshakeException('Request has multiple %s header lines or '
                                     'contains illegal character \',\': %r' %
                                     (common.SEC_WEBSOCKET_KEY_HEADER, key))

        # Validate
        key_is_valid = False
        try:
            # Validate key by quick regex match before parsing by base64
            # module. Because base64 module skips invalid characters, we have
            # to do this in advance to make this server strictly reject illegal
            # keys.
            if _SEC_WEBSOCKET_KEY_REGEX.match(key):
                decoded_key = base64.b64decode(key)
                if len(decoded_key) == 16:
                    key_is_valid = True
        except TypeError as e:
            pass

        if not key_is_valid:
            raise HandshakeException('Illegal value for header %s: %r' %
                                     (common.SEC_WEBSOCKET_KEY_HEADER, key))

        return decoded_key

    def _get_key(self):
        key = get_mandatory_header(self._request,
                                   common.SEC_WEBSOCKET_KEY_HEADER)

        decoded_key = self._validate_key(key)

        self._logger.debug('%s: %r (%s)', common.SEC_WEBSOCKET_KEY_HEADER, key,
                           util.hexify(decoded_key))

        return key.encode('UTF-8')

    def _create_handshake_response(self, accept):
        response = []

        response.append(u'HTTP/1.1 101 Switching Protocols\r\n')

        # WebSocket headers
        response.append(
            format_header(common.UPGRADE_HEADER,
                          common.WEBSOCKET_UPGRADE_TYPE))
        response.append(
            format_header(common.CONNECTION_HEADER,
                          common.UPGRADE_CONNECTION_TYPE))
        response.append(
            format_header(common.SEC_WEBSOCKET_ACCEPT_HEADER,
                          accept.decode('UTF-8')))
        if self._request.ws_protocol is not None:
            response.append(
                format_header(common.SEC_WEBSOCKET_PROTOCOL_HEADER,
                              self._request.ws_protocol))
        if (self._request.ws_extensions is not None
                and len(self._request.ws_extensions) != 0):
            response.append(
                format_header(
                    common.SEC_WEBSOCKET_EXTENSIONS_HEADER,
                    common.format_extensions(self._request.ws_extensions)))

        # Headers not specific for WebSocket
        for name, value in self._request.extra_headers:
            response.append(format_header(name, value))

        response.append(u'\r\n')

        return u''.join(response)

    def _send_handshake(self):
        raw_response = self._create_handshake_response(self._request._accept)
        self._request.connection.write(raw_response.encode('UTF-8'))
        self._logger.debug('Sent server\'s opening handshake: %r',
                           raw_response)


# vi:sts=4 sw=4 et
