# Copyright 2011, Google Inc.
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
""" A Standalone WebSocket Server for testing purposes

pywebsocket3 is an API that provides WebSocket functionalities with
a standalone WebSocket server. It is intended for testing or
experimental purposes.

Installation
============
1. Follow standalone server documentation to start running the
standalone server. It can be read by running the following command:

    $ pydoc pywebsocket3.standalone

2. Once the standalone server is launched verify it by accessing
http://localhost[:port]/console.html. Include the port number when
specified on launch. If everything is working correctly, you
will see a simple echo console.


Writing WebSocket handlers
==========================

When a WebSocket request comes in, the resource name
specified in the handshake is considered as if it is a file path under
<websock_handlers> and the handler defined in
<websock_handlers>/<resource_name>_wsh.py is invoked.

For example, if the resource name is /example/chat, the handler defined in
<websock_handlers>/example/chat_wsh.py is invoked.

A WebSocket handler is composed of the following three functions:

    web_socket_do_extra_handshake(request)
    web_socket_transfer_data(request)
    web_socket_passive_closing_handshake(request)

where:
    request: mod_python request.

web_socket_do_extra_handshake is called during the handshake after the
headers are successfully parsed and WebSocket properties (ws_origin,
and ws_resource) are added to request. A handler
can reject the request by raising an exception.

A request object has the following properties that you can use during the
extra handshake (web_socket_do_extra_handshake):
- ws_resource
- ws_origin
- ws_version
- ws_extensions
- ws_deflate
- ws_protocol
- ws_requested_protocols

The last two are a bit tricky. See the next subsection.


Subprotocol Negotiation
-----------------------

ws_protocol is always set to None when
web_socket_do_extra_handshake is called. If ws_requested_protocols is not
None, you must choose one subprotocol from this list and set it to
ws_protocol.

Data Transfer
-------------

web_socket_transfer_data is called after the handshake completed
successfully. A handler can receive/send messages from/to the client
using request. pywebsocket3.msgutil module provides utilities
for data transfer.

You can receive a message by the following statement.

    message = request.ws_stream.receive_message()

This call blocks until any complete text frame arrives, and the payload data
of the incoming frame will be stored into message. When you're using IETF
HyBi 00 or later protocol, receive_message() will return None on receiving
client-initiated closing handshake. When any error occurs, receive_message()
will raise some exception.

You can send a message by the following statement.

    request.ws_stream.send_message(message)


Closing Connection
------------------

Executing the following statement or just return-ing from
web_socket_transfer_data cause connection close.

    request.ws_stream.close_connection()

close_connection will wait
for closing handshake acknowledgement coming from the client. When it
couldn't receive a valid acknowledgement, raises an exception.

web_socket_passive_closing_handshake is called after the server receives
incoming closing frame from the client peer immediately. You can specify
code and reason by return values. They are sent as a outgoing closing frame
from the server. A request object has the following properties that you can
use in web_socket_passive_closing_handshake.
- ws_close_code
- ws_close_reason


Threading
---------

A WebSocket handler must be thread-safe. The standalone
server uses threads by default.


Configuring WebSocket Extension Processors
------------------------------------------

See extensions.py for supported WebSocket extensions. Note that they are
unstable and their APIs are subject to change substantially.

A request object has these extension processing related attributes.

- ws_requested_extensions:

  A list of common.ExtensionParameter instances representing extension
  parameters received from the client in the client's opening handshake.
  You shouldn't modify it manually.

- ws_extensions:

  A list of common.ExtensionParameter instances representing extension
  parameters to send back to the client in the server's opening handshake.
  You shouldn't touch it directly. Instead, call methods on extension
  processors.

- ws_extension_processors:

  A list of loaded extension processors. Find the processor for the
  extension you want to configure from it, and call its methods.
"""

# vi:sts=4 sw=4 et tw=72
