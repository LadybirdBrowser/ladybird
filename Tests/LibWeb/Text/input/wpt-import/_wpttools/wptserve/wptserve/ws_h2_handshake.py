# mypy: allow-untyped-defs

"""This file provides the opening handshake processor for the Bootstrapping
WebSockets with HTTP/2 protocol (RFC 8441).

Specification:
https://tools.ietf.org/html/rfc8441
"""

from pywebsocket3 import common

from pywebsocket3.handshake.base import get_mandatory_header
from pywebsocket3.handshake.base import HandshakeException
from pywebsocket3.handshake.base import validate_mandatory_header
from pywebsocket3.handshake.base import HandshakerBase


def check_connect_method(request):
    if request.method != 'CONNECT':
        raise HandshakeException('Method is not CONNECT: %r' % request.method)


class WsH2Handshaker(HandshakerBase):
    def __init__(self, request, dispatcher):
        """Bootstrapping handshake processor for the WebSocket protocol with HTTP/2 (RFC 8441).

        :param request: mod_python request.

        :param dispatcher: Dispatcher (dispatch.Dispatcher).

        WsH2Handshaker will add attributes such as ws_resource during handshake.
        """

        super().__init__(request, dispatcher)

    def _transform_header(self, header):
        return header.lower()

    def _protocol_rfc(self):
        return 'RFC 8441'

    def _validate_request(self):
        check_connect_method(self._request)
        validate_mandatory_header(self._request, ':protocol', 'websocket')
        get_mandatory_header(self._request, 'authority')

    def _set_accept(self):
        # irrelevant for HTTP/2 handshake
        pass

    def _send_handshake(self):
        # We are not actually sending the handshake, but just preparing it. It
        # will be flushed by the caller.
        self._request.status = 200

        self._request.headers_out['upgrade'] = common.WEBSOCKET_UPGRADE_TYPE
        self._request.headers_out[
            'connection'] = common.UPGRADE_CONNECTION_TYPE

        if self._request.ws_protocol is not None:
            self._request.headers_out[
                'sec-websocket-protocol'] = self._request.ws_protocol

        if (self._request.ws_extensions is not None and
                len(self._request.ws_extensions) != 0):
            self._request.headers_out[
                'sec-websocket-extensions'] = common.format_extensions(
                    self._request.ws_extensions)

        # Headers not specific for WebSocket
        for name, value in self._request.extra_headers:
            self._request.headers_out[name] = value
