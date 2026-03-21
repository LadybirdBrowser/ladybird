# -*- coding: utf-8 -*-
"""
h2/stream
~~~~~~~~~

An implementation of a HTTP/2 stream.
"""
from enum import Enum, IntEnum
from hpack import HeaderTuple
from hyperframe.frame import (
    HeadersFrame, ContinuationFrame, DataFrame, WindowUpdateFrame,
    RstStreamFrame, PushPromiseFrame, AltSvcFrame
)

from .errors import ErrorCodes, _error_code_from_int
from .events import (
    RequestReceived, ResponseReceived, DataReceived, WindowUpdated,
    StreamEnded, PushedStreamReceived, StreamReset, TrailersReceived,
    InformationalResponseReceived, AlternativeServiceAvailable,
    _ResponseSent, _RequestSent, _TrailersSent, _PushedRequestSent
)
from .exceptions import (
    ProtocolError, StreamClosedError, InvalidBodyLengthError, FlowControlError
)
from .utilities import (
    guard_increment_window, is_informational_response, authority_from_headers,
    validate_headers, validate_outbound_headers, normalize_outbound_headers,
    HeaderValidationFlags, extract_method_header, normalize_inbound_headers
)
from .windows import WindowManager


class StreamState(IntEnum):
    IDLE = 0
    RESERVED_REMOTE = 1
    RESERVED_LOCAL = 2
    OPEN = 3
    HALF_CLOSED_REMOTE = 4
    HALF_CLOSED_LOCAL = 5
    CLOSED = 6


class StreamInputs(Enum):
    SEND_HEADERS = 0
    SEND_PUSH_PROMISE = 1
    SEND_RST_STREAM = 2
    SEND_DATA = 3
    SEND_WINDOW_UPDATE = 4
    SEND_END_STREAM = 5
    RECV_HEADERS = 6
    RECV_PUSH_PROMISE = 7
    RECV_RST_STREAM = 8
    RECV_DATA = 9
    RECV_WINDOW_UPDATE = 10
    RECV_END_STREAM = 11
    RECV_CONTINUATION = 12  # Added in 2.0.0
    SEND_INFORMATIONAL_HEADERS = 13  # Added in 2.2.0
    RECV_INFORMATIONAL_HEADERS = 14  # Added in 2.2.0
    SEND_ALTERNATIVE_SERVICE = 15  # Added in 2.3.0
    RECV_ALTERNATIVE_SERVICE = 16  # Added in 2.3.0
    UPGRADE_CLIENT = 17  # Added 2.3.0
    UPGRADE_SERVER = 18  # Added 2.3.0


class StreamClosedBy(Enum):
    SEND_END_STREAM = 0
    RECV_END_STREAM = 1
    SEND_RST_STREAM = 2
    RECV_RST_STREAM = 3


# This array is initialized once, and is indexed by the stream states above.
# It indicates whether a stream in the given state is open. The reason we do
# this is that we potentially check whether a stream in a given state is open
# quite frequently: given that we check so often, we should do so in the
# fastest and most performant way possible.
STREAM_OPEN = [False for _ in range(0, len(StreamState))]
STREAM_OPEN[StreamState.OPEN] = True
STREAM_OPEN[StreamState.HALF_CLOSED_LOCAL] = True
STREAM_OPEN[StreamState.HALF_CLOSED_REMOTE] = True


class H2StreamStateMachine:
    """
    A single HTTP/2 stream state machine.

    This stream object implements basically the state machine described in
    RFC 7540 section 5.1.

    :param stream_id: The stream ID of this stream. This is stored primarily
        for logging purposes.
    """
    def __init__(self, stream_id):
        self.state = StreamState.IDLE
        self.stream_id = stream_id

        #: Whether this peer is the client side of this stream.
        self.client = None

        # Whether trailers have been sent/received on this stream or not.
        self.headers_sent = None
        self.trailers_sent = None
        self.headers_received = None
        self.trailers_received = None

        # How the stream was closed. One of StreamClosedBy.
        self.stream_closed_by = None

    def process_input(self, input_):
        """
        Process a specific input in the state machine.
        """
        if not isinstance(input_, StreamInputs):
            raise ValueError("Input must be an instance of StreamInputs")

        try:
            func, target_state = _transitions[(self.state, input_)]
        except KeyError:
            old_state = self.state
            self.state = StreamState.CLOSED
            raise ProtocolError(
                "Invalid input %s in state %s" % (input_, old_state)
            )
        else:
            previous_state = self.state
            self.state = target_state
            if func is not None:
                try:
                    return func(self, previous_state)
                except ProtocolError:
                    self.state = StreamState.CLOSED
                    raise
                except AssertionError as e:  # pragma: no cover
                    self.state = StreamState.CLOSED
                    raise ProtocolError(e)

            return []

    def request_sent(self, previous_state):
        """
        Fires when a request is sent.
        """
        self.client = True
        self.headers_sent = True
        event = _RequestSent()

        return [event]

    def response_sent(self, previous_state):
        """
        Fires when something that should be a response is sent. This 'response'
        may actually be trailers.
        """
        if not self.headers_sent:
            if self.client is True or self.client is None:
                raise ProtocolError("Client cannot send responses.")
            self.headers_sent = True
            event = _ResponseSent()
        else:
            assert not self.trailers_sent
            self.trailers_sent = True
            event = _TrailersSent()

        return [event]

    def request_received(self, previous_state):
        """
        Fires when a request is received.
        """
        assert not self.headers_received
        assert not self.trailers_received

        self.client = False
        self.headers_received = True
        event = RequestReceived()

        event.stream_id = self.stream_id
        return [event]

    def response_received(self, previous_state):
        """
        Fires when a response is received. Also disambiguates between responses
        and trailers.
        """
        if not self.headers_received:
            assert self.client is True
            self.headers_received = True
            event = ResponseReceived()
        else:
            assert not self.trailers_received
            self.trailers_received = True
            event = TrailersReceived()

        event.stream_id = self.stream_id
        return [event]

    def data_received(self, previous_state):
        """
        Fires when data is received.
        """
        if not self.headers_received:
            raise ProtocolError("cannot receive data before headers")
        event = DataReceived()
        event.stream_id = self.stream_id
        return [event]

    def window_updated(self, previous_state):
        """
        Fires when a window update frame is received.
        """
        event = WindowUpdated()
        event.stream_id = self.stream_id
        return [event]

    def stream_half_closed(self, previous_state):
        """
        Fires when an END_STREAM flag is received in the OPEN state,
        transitioning this stream to a HALF_CLOSED_REMOTE state.
        """
        event = StreamEnded()
        event.stream_id = self.stream_id
        return [event]

    def stream_ended(self, previous_state):
        """
        Fires when a stream is cleanly ended.
        """
        self.stream_closed_by = StreamClosedBy.RECV_END_STREAM
        event = StreamEnded()
        event.stream_id = self.stream_id
        return [event]

    def stream_reset(self, previous_state):
        """
        Fired when a stream is forcefully reset.
        """
        self.stream_closed_by = StreamClosedBy.RECV_RST_STREAM
        event = StreamReset()
        event.stream_id = self.stream_id
        return [event]

    def send_new_pushed_stream(self, previous_state):
        """
        Fires on the newly pushed stream, when pushed by the local peer.

        No event here, but definitionally this peer must be a server.
        """
        assert self.client is None
        self.client = False
        self.headers_received = True
        return []

    def recv_new_pushed_stream(self, previous_state):
        """
        Fires on the newly pushed stream, when pushed by the remote peer.

        No event here, but definitionally this peer must be a client.
        """
        assert self.client is None
        self.client = True
        self.headers_sent = True
        return []

    def send_push_promise(self, previous_state):
        """
        Fires on the already-existing stream when a PUSH_PROMISE frame is sent.
        We may only send PUSH_PROMISE frames if we're a server.
        """
        if self.client is True:
            raise ProtocolError("Cannot push streams from client peers.")

        event = _PushedRequestSent()
        return [event]

    def recv_push_promise(self, previous_state):
        """
        Fires on the already-existing stream when a PUSH_PROMISE frame is
        received. We may only receive PUSH_PROMISE frames if we're a client.

        Fires a PushedStreamReceived event.
        """
        if not self.client:
            if self.client is None:  # pragma: no cover
                msg = "Idle streams cannot receive pushes"
            else:  # pragma: no cover
                msg = "Cannot receive pushed streams as a server"
            raise ProtocolError(msg)

        event = PushedStreamReceived()
        event.parent_stream_id = self.stream_id
        return [event]

    def send_end_stream(self, previous_state):
        """
        Called when an attempt is made to send END_STREAM in the
        HALF_CLOSED_REMOTE state.
        """
        self.stream_closed_by = StreamClosedBy.SEND_END_STREAM

    def send_reset_stream(self, previous_state):
        """
        Called when an attempt is made to send RST_STREAM in a non-closed
        stream state.
        """
        self.stream_closed_by = StreamClosedBy.SEND_RST_STREAM

    def reset_stream_on_error(self, previous_state):
        """
        Called when we need to forcefully emit another RST_STREAM frame on
        behalf of the state machine.

        If this is the first time we've done this, we should also hang an event
        off the StreamClosedError so that the user can be informed. We know
        it's the first time we've done this if the stream is currently in a
        state other than CLOSED.
        """
        self.stream_closed_by = StreamClosedBy.SEND_RST_STREAM

        error = StreamClosedError(self.stream_id)

        event = StreamReset()
        event.stream_id = self.stream_id
        event.error_code = ErrorCodes.STREAM_CLOSED
        event.remote_reset = False
        error._events = [event]
        raise error

    def recv_on_closed_stream(self, previous_state):
        """
        Called when an unexpected frame is received on an already-closed
        stream.

        An endpoint that receives an unexpected frame should treat it as
        a stream error or connection error with type STREAM_CLOSED, depending
        on the specific frame. The error handling is done at a higher level:
        this just raises the appropriate error.
        """
        raise StreamClosedError(self.stream_id)

    def send_on_closed_stream(self, previous_state):
        """
        Called when an attempt is made to send data on an already-closed
        stream.

        This essentially overrides the standard logic by throwing a
        more-specific error: StreamClosedError. This is a ProtocolError, so it
        matches the standard API of the state machine, but provides more detail
        to the user.
        """
        raise StreamClosedError(self.stream_id)

    def recv_push_on_closed_stream(self, previous_state):
        """
        Called when a PUSH_PROMISE frame is received on a full stop
        stream.

        If the stream was closed by us sending a RST_STREAM frame, then we
        presume that the PUSH_PROMISE was in flight when we reset the parent
        stream. Rathen than accept the new stream, we just reset it.
        Otherwise, we should call this a PROTOCOL_ERROR: pushing a stream on a
        naturally closed stream is a real problem because it creates a brand
        new stream that the remote peer now believes exists.
        """
        assert self.stream_closed_by is not None

        if self.stream_closed_by == StreamClosedBy.SEND_RST_STREAM:
            raise StreamClosedError(self.stream_id)
        else:
            raise ProtocolError("Attempted to push on closed stream.")

    def send_push_on_closed_stream(self, previous_state):
        """
        Called when an attempt is made to push on an already-closed stream.

        This essentially overrides the standard logic by providing a more
        useful error message. It's necessary because simply indicating that the
        stream is closed is not enough: there is now a new stream that is not
        allowed to be there. The only recourse is to tear the whole connection
        down.
        """
        raise ProtocolError("Attempted to push on closed stream.")

    def send_informational_response(self, previous_state):
        """
        Called when an informational header block is sent (that is, a block
        where the :status header has a 1XX value).

        Only enforces that these are sent *before* final headers are sent.
        """
        if self.headers_sent:
            raise ProtocolError("Information response after final response")

        event = _ResponseSent()
        return [event]

    def recv_informational_response(self, previous_state):
        """
        Called when an informational header block is received (that is, a block
        where the :status header has a 1XX value).
        """
        if self.headers_received:
            raise ProtocolError("Informational response after final response")

        event = InformationalResponseReceived()
        event.stream_id = self.stream_id
        return [event]

    def recv_alt_svc(self, previous_state):
        """
        Called when receiving an ALTSVC frame.

        RFC 7838 allows us to receive ALTSVC frames at any stream state, which
        is really absurdly overzealous. For that reason, we want to limit the
        states in which we can actually receive it. It's really only sensible
        to receive it after we've sent our own headers and before the server
        has sent its header block: the server can't guarantee that we have any
        state around after it completes its header block, and the server
        doesn't know what origin we're talking about before we've sent ours.

        For that reason, this function applies a few extra checks on both state
        and some of the little state variables we keep around. If those suggest
        an unreasonable situation for the ALTSVC frame to have been sent in,
        we quietly ignore it (as RFC 7838 suggests).

        This function is also *not* always called by the state machine. In some
        states (IDLE, RESERVED_LOCAL, CLOSED) we don't bother to call it,
        because we know the frame cannot be valid in that state (IDLE because
        the server cannot know what origin the stream applies to, CLOSED
        because the server cannot assume we still have state around,
        RESERVED_LOCAL because by definition if we're in the RESERVED_LOCAL
        state then *we* are the server).
        """
        # Servers can't receive ALTSVC frames, but RFC 7838 tells us to ignore
        # them.
        if self.client is False:
            return []

        # If we've received the response headers from the server they can't
        # guarantee we still have any state around. Other implementations
        # (like nghttp2) ignore ALTSVC in this state, so we will too.
        if self.headers_received:
            return []

        # Otherwise, this is a sensible enough frame to have received. Return
        # the event and let it get populated.
        return [AlternativeServiceAvailable()]

    def send_alt_svc(self, previous_state):
        """
        Called when sending an ALTSVC frame on this stream.

        For consistency with the restrictions we apply on receiving ALTSVC
        frames in ``recv_alt_svc``, we want to restrict when users can send
        ALTSVC frames to the situations when we ourselves would accept them.

        That means: when we are a server, when we have received the request
        headers, and when we have not yet sent our own response headers.
        """
        # We should not send ALTSVC after we've sent response headers, as the
        # client may have disposed of its state.
        if self.headers_sent:
            raise ProtocolError(
                "Cannot send ALTSVC after sending response headers."
            )

        return


# STATE MACHINE
#
# The stream state machine is defined here to avoid the need to allocate it
# repeatedly for each stream. It cannot be defined in the stream class because
# it needs to be able to reference the callbacks defined on the class, but
# because Python's scoping rules are weird the class object is not actually in
# scope during the body of the class object.
#
# For the sake of clarity, we reproduce the RFC 7540 state machine here:
#
#                          +--------+
#                  send PP |        | recv PP
#                 ,--------|  idle  |--------.
#                /         |        |         \
#               v          +--------+          v
#        +----------+          |           +----------+
#        |          |          | send H /  |          |
# ,------| reserved |          | recv H    | reserved |------.
# |      | (local)  |          |           | (remote) |      |
# |      +----------+          v           +----------+      |
# |          |             +--------+             |          |
# |          |     recv ES |        | send ES     |          |
# |   send H |     ,-------|  open  |-------.     | recv H   |
# |          |    /        |        |        \    |          |
# |          v   v         +--------+         v   v          |
# |      +----------+          |           +----------+      |
# |      |   half   |          |           |   half   |      |
# |      |  closed  |          | send R /  |  closed  |      |
# |      | (remote) |          | recv R    | (local)  |      |
# |      +----------+          |           +----------+      |
# |           |                |                 |           |
# |           | send ES /      |       recv ES / |           |
# |           | send R /       v        send R / |           |
# |           | recv R     +--------+   recv R   |           |
# | send R /  `----------->|        |<-----------'  send R / |
# | recv R                 | closed |               recv R   |
# `----------------------->|        |<----------------------'
#                          +--------+
#
#    send:   endpoint sends this frame
#    recv:   endpoint receives this frame
#
#    H:  HEADERS frame (with implied CONTINUATIONs)
#    PP: PUSH_PROMISE frame (with implied CONTINUATIONs)
#    ES: END_STREAM flag
#    R:  RST_STREAM frame
#
# For the purposes of this state machine we treat HEADERS and their
# associated CONTINUATION frames as a single jumbo frame. The protocol
# allows/requires this by preventing other frames from being interleved in
# between HEADERS/CONTINUATION frames. However, if a CONTINUATION frame is
# received without a prior HEADERS frame, it *will* be passed to this state
# machine. The state machine should always reject that frame, either as an
# invalid transition or because the stream is closed.
#
# There is a confusing relationship around PUSH_PROMISE frames. The state
# machine above considers them to be frames belonging to the new stream,
# which is *somewhat* true. However, they are sent with the stream ID of
# their related stream, and are only sendable in some cases.
# For this reason, our state machine implementation below allows for
# PUSH_PROMISE frames both in the IDLE state (as in the diagram), but also
# in the OPEN, HALF_CLOSED_LOCAL, and HALF_CLOSED_REMOTE states.
# Essentially, for h2, PUSH_PROMISE frames are effectively sent on
# two streams.
#
# The _transitions dictionary contains a mapping of tuples of
# (state, input) to tuples of (side_effect_function, end_state). This
# map contains all allowed transitions: anything not in this map is
# invalid and immediately causes a transition to ``closed``.
_transitions = {
    # State: idle
    (StreamState.IDLE, StreamInputs.SEND_HEADERS):
        (H2StreamStateMachine.request_sent, StreamState.OPEN),
    (StreamState.IDLE, StreamInputs.RECV_HEADERS):
        (H2StreamStateMachine.request_received, StreamState.OPEN),
    (StreamState.IDLE, StreamInputs.RECV_DATA):
        (H2StreamStateMachine.reset_stream_on_error, StreamState.CLOSED),
    (StreamState.IDLE, StreamInputs.SEND_PUSH_PROMISE):
        (H2StreamStateMachine.send_new_pushed_stream,
            StreamState.RESERVED_LOCAL),
    (StreamState.IDLE, StreamInputs.RECV_PUSH_PROMISE):
        (H2StreamStateMachine.recv_new_pushed_stream,
            StreamState.RESERVED_REMOTE),
    (StreamState.IDLE, StreamInputs.RECV_ALTERNATIVE_SERVICE):
        (None, StreamState.IDLE),
    (StreamState.IDLE, StreamInputs.UPGRADE_CLIENT):
        (H2StreamStateMachine.request_sent, StreamState.HALF_CLOSED_LOCAL),
    (StreamState.IDLE, StreamInputs.UPGRADE_SERVER):
        (H2StreamStateMachine.request_received,
            StreamState.HALF_CLOSED_REMOTE),

    # State: reserved local
    (StreamState.RESERVED_LOCAL, StreamInputs.SEND_HEADERS):
        (H2StreamStateMachine.response_sent, StreamState.HALF_CLOSED_REMOTE),
    (StreamState.RESERVED_LOCAL, StreamInputs.RECV_DATA):
        (H2StreamStateMachine.reset_stream_on_error, StreamState.CLOSED),
    (StreamState.RESERVED_LOCAL, StreamInputs.SEND_WINDOW_UPDATE):
        (None, StreamState.RESERVED_LOCAL),
    (StreamState.RESERVED_LOCAL, StreamInputs.RECV_WINDOW_UPDATE):
        (H2StreamStateMachine.window_updated, StreamState.RESERVED_LOCAL),
    (StreamState.RESERVED_LOCAL, StreamInputs.SEND_RST_STREAM):
        (H2StreamStateMachine.send_reset_stream, StreamState.CLOSED),
    (StreamState.RESERVED_LOCAL, StreamInputs.RECV_RST_STREAM):
        (H2StreamStateMachine.stream_reset, StreamState.CLOSED),
    (StreamState.RESERVED_LOCAL, StreamInputs.SEND_ALTERNATIVE_SERVICE):
        (H2StreamStateMachine.send_alt_svc, StreamState.RESERVED_LOCAL),
    (StreamState.RESERVED_LOCAL, StreamInputs.RECV_ALTERNATIVE_SERVICE):
        (None, StreamState.RESERVED_LOCAL),

    # State: reserved remote
    (StreamState.RESERVED_REMOTE, StreamInputs.RECV_HEADERS):
        (H2StreamStateMachine.response_received,
            StreamState.HALF_CLOSED_LOCAL),
    (StreamState.RESERVED_REMOTE, StreamInputs.RECV_DATA):
        (H2StreamStateMachine.reset_stream_on_error, StreamState.CLOSED),
    (StreamState.RESERVED_REMOTE, StreamInputs.SEND_WINDOW_UPDATE):
        (None, StreamState.RESERVED_REMOTE),
    (StreamState.RESERVED_REMOTE, StreamInputs.RECV_WINDOW_UPDATE):
        (H2StreamStateMachine.window_updated, StreamState.RESERVED_REMOTE),
    (StreamState.RESERVED_REMOTE, StreamInputs.SEND_RST_STREAM):
        (H2StreamStateMachine.send_reset_stream, StreamState.CLOSED),
    (StreamState.RESERVED_REMOTE, StreamInputs.RECV_RST_STREAM):
        (H2StreamStateMachine.stream_reset, StreamState.CLOSED),
    (StreamState.RESERVED_REMOTE, StreamInputs.RECV_ALTERNATIVE_SERVICE):
        (H2StreamStateMachine.recv_alt_svc, StreamState.RESERVED_REMOTE),

    # State: open
    (StreamState.OPEN, StreamInputs.SEND_HEADERS):
        (H2StreamStateMachine.response_sent, StreamState.OPEN),
    (StreamState.OPEN, StreamInputs.RECV_HEADERS):
        (H2StreamStateMachine.response_received, StreamState.OPEN),
    (StreamState.OPEN, StreamInputs.SEND_DATA):
        (None, StreamState.OPEN),
    (StreamState.OPEN, StreamInputs.RECV_DATA):
        (H2StreamStateMachine.data_received, StreamState.OPEN),
    (StreamState.OPEN, StreamInputs.SEND_END_STREAM):
        (None, StreamState.HALF_CLOSED_LOCAL),
    (StreamState.OPEN, StreamInputs.RECV_END_STREAM):
        (H2StreamStateMachine.stream_half_closed,
         StreamState.HALF_CLOSED_REMOTE),
    (StreamState.OPEN, StreamInputs.SEND_WINDOW_UPDATE):
        (None, StreamState.OPEN),
    (StreamState.OPEN, StreamInputs.RECV_WINDOW_UPDATE):
        (H2StreamStateMachine.window_updated, StreamState.OPEN),
    (StreamState.OPEN, StreamInputs.SEND_RST_STREAM):
        (H2StreamStateMachine.send_reset_stream, StreamState.CLOSED),
    (StreamState.OPEN, StreamInputs.RECV_RST_STREAM):
        (H2StreamStateMachine.stream_reset, StreamState.CLOSED),
    (StreamState.OPEN, StreamInputs.SEND_PUSH_PROMISE):
        (H2StreamStateMachine.send_push_promise, StreamState.OPEN),
    (StreamState.OPEN, StreamInputs.RECV_PUSH_PROMISE):
        (H2StreamStateMachine.recv_push_promise, StreamState.OPEN),
    (StreamState.OPEN, StreamInputs.SEND_INFORMATIONAL_HEADERS):
        (H2StreamStateMachine.send_informational_response, StreamState.OPEN),
    (StreamState.OPEN, StreamInputs.RECV_INFORMATIONAL_HEADERS):
        (H2StreamStateMachine.recv_informational_response, StreamState.OPEN),
    (StreamState.OPEN, StreamInputs.SEND_ALTERNATIVE_SERVICE):
        (H2StreamStateMachine.send_alt_svc, StreamState.OPEN),
    (StreamState.OPEN, StreamInputs.RECV_ALTERNATIVE_SERVICE):
        (H2StreamStateMachine.recv_alt_svc, StreamState.OPEN),

    # State: half-closed remote
    (StreamState.HALF_CLOSED_REMOTE, StreamInputs.SEND_HEADERS):
        (H2StreamStateMachine.response_sent, StreamState.HALF_CLOSED_REMOTE),
    (StreamState.HALF_CLOSED_REMOTE, StreamInputs.RECV_HEADERS):
        (H2StreamStateMachine.reset_stream_on_error, StreamState.CLOSED),
    (StreamState.HALF_CLOSED_REMOTE, StreamInputs.SEND_DATA):
        (None, StreamState.HALF_CLOSED_REMOTE),
    (StreamState.HALF_CLOSED_REMOTE, StreamInputs.RECV_DATA):
        (H2StreamStateMachine.reset_stream_on_error, StreamState.CLOSED),
    (StreamState.HALF_CLOSED_REMOTE, StreamInputs.SEND_END_STREAM):
        (H2StreamStateMachine.send_end_stream, StreamState.CLOSED),
    (StreamState.HALF_CLOSED_REMOTE, StreamInputs.SEND_WINDOW_UPDATE):
        (None, StreamState.HALF_CLOSED_REMOTE),
    (StreamState.HALF_CLOSED_REMOTE, StreamInputs.RECV_WINDOW_UPDATE):
        (H2StreamStateMachine.window_updated, StreamState.HALF_CLOSED_REMOTE),
    (StreamState.HALF_CLOSED_REMOTE, StreamInputs.SEND_RST_STREAM):
        (H2StreamStateMachine.send_reset_stream, StreamState.CLOSED),
    (StreamState.HALF_CLOSED_REMOTE, StreamInputs.RECV_RST_STREAM):
        (H2StreamStateMachine.stream_reset, StreamState.CLOSED),
    (StreamState.HALF_CLOSED_REMOTE, StreamInputs.SEND_PUSH_PROMISE):
        (H2StreamStateMachine.send_push_promise,
            StreamState.HALF_CLOSED_REMOTE),
    (StreamState.HALF_CLOSED_REMOTE, StreamInputs.RECV_PUSH_PROMISE):
        (H2StreamStateMachine.reset_stream_on_error, StreamState.CLOSED),
    (StreamState.HALF_CLOSED_REMOTE, StreamInputs.SEND_INFORMATIONAL_HEADERS):
        (H2StreamStateMachine.send_informational_response,
            StreamState.HALF_CLOSED_REMOTE),
    (StreamState.HALF_CLOSED_REMOTE, StreamInputs.SEND_ALTERNATIVE_SERVICE):
        (H2StreamStateMachine.send_alt_svc, StreamState.HALF_CLOSED_REMOTE),
    (StreamState.HALF_CLOSED_REMOTE, StreamInputs.RECV_ALTERNATIVE_SERVICE):
        (H2StreamStateMachine.recv_alt_svc, StreamState.HALF_CLOSED_REMOTE),

    # State: half-closed local
    (StreamState.HALF_CLOSED_LOCAL, StreamInputs.RECV_HEADERS):
        (H2StreamStateMachine.response_received,
            StreamState.HALF_CLOSED_LOCAL),
    (StreamState.HALF_CLOSED_LOCAL, StreamInputs.RECV_DATA):
        (H2StreamStateMachine.data_received, StreamState.HALF_CLOSED_LOCAL),
    (StreamState.HALF_CLOSED_LOCAL, StreamInputs.RECV_END_STREAM):
        (H2StreamStateMachine.stream_ended, StreamState.CLOSED),
    (StreamState.HALF_CLOSED_LOCAL, StreamInputs.SEND_WINDOW_UPDATE):
        (None, StreamState.HALF_CLOSED_LOCAL),
    (StreamState.HALF_CLOSED_LOCAL, StreamInputs.RECV_WINDOW_UPDATE):
        (H2StreamStateMachine.window_updated, StreamState.HALF_CLOSED_LOCAL),
    (StreamState.HALF_CLOSED_LOCAL, StreamInputs.SEND_RST_STREAM):
        (H2StreamStateMachine.send_reset_stream, StreamState.CLOSED),
    (StreamState.HALF_CLOSED_LOCAL, StreamInputs.RECV_RST_STREAM):
        (H2StreamStateMachine.stream_reset, StreamState.CLOSED),
    (StreamState.HALF_CLOSED_LOCAL, StreamInputs.RECV_PUSH_PROMISE):
        (H2StreamStateMachine.recv_push_promise,
            StreamState.HALF_CLOSED_LOCAL),
    (StreamState.HALF_CLOSED_LOCAL, StreamInputs.RECV_INFORMATIONAL_HEADERS):
        (H2StreamStateMachine.recv_informational_response,
            StreamState.HALF_CLOSED_LOCAL),
    (StreamState.HALF_CLOSED_LOCAL, StreamInputs.SEND_ALTERNATIVE_SERVICE):
        (H2StreamStateMachine.send_alt_svc, StreamState.HALF_CLOSED_LOCAL),
    (StreamState.HALF_CLOSED_LOCAL, StreamInputs.RECV_ALTERNATIVE_SERVICE):
        (H2StreamStateMachine.recv_alt_svc, StreamState.HALF_CLOSED_LOCAL),

    # State: closed
    (StreamState.CLOSED, StreamInputs.RECV_END_STREAM):
        (None, StreamState.CLOSED),
    (StreamState.CLOSED, StreamInputs.RECV_ALTERNATIVE_SERVICE):
        (None, StreamState.CLOSED),

    # RFC 7540 Section 5.1 defines how the end point should react when
    # receiving a frame on a closed stream with the following statements:
    #
    # > An endpoint that receives any frame other than PRIORITY after receiving
    # > a RST_STREAM MUST treat that as a stream error of type STREAM_CLOSED.
    # > An endpoint that receives any frames after receiving a frame with the
    # > END_STREAM flag set MUST treat that as a connection error of type
    # > STREAM_CLOSED.
    (StreamState.CLOSED, StreamInputs.RECV_HEADERS):
        (H2StreamStateMachine.recv_on_closed_stream, StreamState.CLOSED),
    (StreamState.CLOSED, StreamInputs.RECV_DATA):
        (H2StreamStateMachine.recv_on_closed_stream, StreamState.CLOSED),

    # > WINDOW_UPDATE or RST_STREAM frames can be received in this state
    # > for a short period after a DATA or HEADERS frame containing a
    # > END_STREAM flag is sent, as instructed in RFC 7540 Section 5.1. But we
    # > don't have access to a clock so we just always allow it.
    (StreamState.CLOSED, StreamInputs.RECV_WINDOW_UPDATE):
        (None, StreamState.CLOSED),
    (StreamState.CLOSED, StreamInputs.RECV_RST_STREAM):
        (None, StreamState.CLOSED),

    # > A receiver MUST treat the receipt of a PUSH_PROMISE on a stream that is
    # > neither "open" nor "half-closed (local)" as a connection error of type
    # > PROTOCOL_ERROR.
    (StreamState.CLOSED, StreamInputs.RECV_PUSH_PROMISE):
        (H2StreamStateMachine.recv_push_on_closed_stream, StreamState.CLOSED),

    # Also, users should be forbidden from sending on closed streams.
    (StreamState.CLOSED, StreamInputs.SEND_HEADERS):
        (H2StreamStateMachine.send_on_closed_stream, StreamState.CLOSED),
    (StreamState.CLOSED, StreamInputs.SEND_PUSH_PROMISE):
        (H2StreamStateMachine.send_push_on_closed_stream, StreamState.CLOSED),
    (StreamState.CLOSED, StreamInputs.SEND_RST_STREAM):
        (H2StreamStateMachine.send_on_closed_stream, StreamState.CLOSED),
    (StreamState.CLOSED, StreamInputs.SEND_DATA):
        (H2StreamStateMachine.send_on_closed_stream, StreamState.CLOSED),
    (StreamState.CLOSED, StreamInputs.SEND_WINDOW_UPDATE):
        (H2StreamStateMachine.send_on_closed_stream, StreamState.CLOSED),
    (StreamState.CLOSED, StreamInputs.SEND_END_STREAM):
        (H2StreamStateMachine.send_on_closed_stream, StreamState.CLOSED),
}


class H2Stream:
    """
    A low-level HTTP/2 stream object. This handles building and receiving
    frames and maintains per-stream state.

    This wraps a HTTP/2 Stream state machine implementation, ensuring that
    frames can only be sent/received when the stream is in a valid state.
    Attempts to create frames that cannot be sent will raise a
    ``ProtocolError``.
    """
    def __init__(self,
                 stream_id,
                 config,
                 inbound_window_size,
                 outbound_window_size):
        self.state_machine = H2StreamStateMachine(stream_id)
        self.stream_id = stream_id
        self.max_outbound_frame_size = None
        self.request_method = None

        # The current value of the outbound stream flow control window
        self.outbound_flow_control_window = outbound_window_size

        # The flow control manager.
        self._inbound_window_manager = WindowManager(inbound_window_size)

        # The expected content length, if any.
        self._expected_content_length = None

        # The actual received content length. Always tracked.
        self._actual_content_length = 0

        # The authority we believe this stream belongs to.
        self._authority = None

        # The configuration for this stream.
        self.config = config

    def __repr__(self):
        return "<%s id:%d state:%r>" % (
            type(self).__name__,
            self.stream_id,
            self.state_machine.state
        )

    @property
    def inbound_flow_control_window(self):
        """
        The size of the inbound flow control window for the stream. This is
        rarely publicly useful: instead, use :meth:`remote_flow_control_window
        <h2.stream.H2Stream.remote_flow_control_window>`. This shortcut is
        largely present to provide a shortcut to this data.
        """
        return self._inbound_window_manager.current_window_size

    @property
    def open(self):
        """
        Whether the stream is 'open' in any sense: that is, whether it counts
        against the number of concurrent streams.
        """
        # RFC 7540 Section 5.1.2 defines 'open' for this purpose to mean either
        # the OPEN state or either of the HALF_CLOSED states. Perplexingly,
        # this excludes the reserved states.
        # For more detail on why we're doing this in this slightly weird way,
        # see the comment on ``STREAM_OPEN`` at the top of the file.
        return STREAM_OPEN[self.state_machine.state]

    @property
    def closed(self):
        """
        Whether the stream is closed.
        """
        return self.state_machine.state == StreamState.CLOSED

    @property
    def closed_by(self):
        """
        Returns how the stream was closed, as one of StreamClosedBy.
        """
        return self.state_machine.stream_closed_by

    def upgrade(self, client_side):
        """
        Called by the connection to indicate that this stream is the initial
        request/response of an upgraded connection. Places the stream into an
        appropriate state.
        """
        self.config.logger.debug("Upgrading %r", self)

        assert self.stream_id == 1
        input_ = (
            StreamInputs.UPGRADE_CLIENT if client_side
            else StreamInputs.UPGRADE_SERVER
        )

        # This may return events, we deliberately don't want them.
        self.state_machine.process_input(input_)
        return

    def send_headers(self, headers, encoder, end_stream=False):
        """
        Returns a list of HEADERS/CONTINUATION frames to emit as either headers
        or trailers.
        """
        self.config.logger.debug("Send headers %s on %r", headers, self)

        # Because encoding headers makes an irreversible change to the header
        # compression context, we make the state transition before we encode
        # them.

        # First, check if we're a client. If we are, no problem: if we aren't,
        # we need to scan the header block to see if this is an informational
        # response.
        input_ = StreamInputs.SEND_HEADERS
        if ((not self.state_machine.client) and
                is_informational_response(headers)):
            if end_stream:
                raise ProtocolError(
                    "Cannot set END_STREAM on informational responses."
                )

            input_ = StreamInputs.SEND_INFORMATIONAL_HEADERS

        events = self.state_machine.process_input(input_)

        hf = HeadersFrame(self.stream_id)
        hdr_validation_flags = self._build_hdr_validation_flags(events)
        frames = self._build_headers_frames(
            headers, encoder, hf, hdr_validation_flags
        )

        if end_stream:
            # Not a bug: the END_STREAM flag is valid on the initial HEADERS
            # frame, not the CONTINUATION frames that follow.
            self.state_machine.process_input(StreamInputs.SEND_END_STREAM)
            frames[0].flags.add('END_STREAM')

        if self.state_machine.trailers_sent and not end_stream:
            raise ProtocolError("Trailers must have END_STREAM set.")

        if self.state_machine.client and self._authority is None:
            self._authority = authority_from_headers(headers)

        # store request method for _initialize_content_length
        self.request_method = extract_method_header(headers)

        return frames

    def push_stream_in_band(self, related_stream_id, headers, encoder):
        """
        Returns a list of PUSH_PROMISE/CONTINUATION frames to emit as a pushed
        stream header. Called on the stream that has the PUSH_PROMISE frame
        sent on it.
        """
        self.config.logger.debug("Push stream %r", self)

        # Because encoding headers makes an irreversible change to the header
        # compression context, we make the state transition *first*.

        events = self.state_machine.process_input(
            StreamInputs.SEND_PUSH_PROMISE
        )

        ppf = PushPromiseFrame(self.stream_id)
        ppf.promised_stream_id = related_stream_id
        hdr_validation_flags = self._build_hdr_validation_flags(events)
        frames = self._build_headers_frames(
            headers, encoder, ppf, hdr_validation_flags
        )

        return frames

    def locally_pushed(self):
        """
        Mark this stream as one that was pushed by this peer. Must be called
        immediately after initialization. Sends no frames, simply updates the
        state machine.
        """
        # This does not trigger any events.
        events = self.state_machine.process_input(
            StreamInputs.SEND_PUSH_PROMISE
        )
        assert not events
        return []

    def send_data(self, data, end_stream=False, pad_length=None):
        """
        Prepare some data frames. Optionally end the stream.

        .. warning:: Does not perform flow control checks.
        """
        self.config.logger.debug(
            "Send data on %r with end stream set to %s", self, end_stream
        )

        self.state_machine.process_input(StreamInputs.SEND_DATA)

        df = DataFrame(self.stream_id)
        df.data = data
        if end_stream:
            self.state_machine.process_input(StreamInputs.SEND_END_STREAM)
            df.flags.add('END_STREAM')
        if pad_length is not None:
            df.flags.add('PADDED')
            df.pad_length = pad_length

        # Subtract flow_controlled_length to account for possible padding
        self.outbound_flow_control_window -= df.flow_controlled_length
        assert self.outbound_flow_control_window >= 0

        return [df]

    def end_stream(self):
        """
        End a stream without sending data.
        """
        self.config.logger.debug("End stream %r", self)

        self.state_machine.process_input(StreamInputs.SEND_END_STREAM)
        df = DataFrame(self.stream_id)
        df.flags.add('END_STREAM')
        return [df]

    def advertise_alternative_service(self, field_value):
        """
        Advertise an RFC 7838 alternative service. The semantics of this are
        better documented in the ``H2Connection`` class.
        """
        self.config.logger.debug(
            "Advertise alternative service of %r for %r", field_value, self
        )
        self.state_machine.process_input(StreamInputs.SEND_ALTERNATIVE_SERVICE)
        asf = AltSvcFrame(self.stream_id)
        asf.field = field_value
        return [asf]

    def increase_flow_control_window(self, increment):
        """
        Increase the size of the flow control window for the remote side.
        """
        self.config.logger.debug(
            "Increase flow control window for %r by %d",
            self, increment
        )
        self.state_machine.process_input(StreamInputs.SEND_WINDOW_UPDATE)
        self._inbound_window_manager.window_opened(increment)

        wuf = WindowUpdateFrame(self.stream_id)
        wuf.window_increment = increment
        return [wuf]

    def receive_push_promise_in_band(self,
                                     promised_stream_id,
                                     headers,
                                     header_encoding):
        """
        Receives a push promise frame sent on this stream, pushing a remote
        stream. This is called on the stream that has the PUSH_PROMISE sent
        on it.
        """
        self.config.logger.debug(
            "Receive Push Promise on %r for remote stream %d",
            self, promised_stream_id
        )
        events = self.state_machine.process_input(
            StreamInputs.RECV_PUSH_PROMISE
        )
        events[0].pushed_stream_id = promised_stream_id

        hdr_validation_flags = self._build_hdr_validation_flags(events)
        events[0].headers = self._process_received_headers(
            headers, hdr_validation_flags, header_encoding
        )
        return [], events

    def remotely_pushed(self, pushed_headers):
        """
        Mark this stream as one that was pushed by the remote peer. Must be
        called immediately after initialization. Sends no frames, simply
        updates the state machine.
        """
        self.config.logger.debug("%r pushed by remote peer", self)
        events = self.state_machine.process_input(
            StreamInputs.RECV_PUSH_PROMISE
        )
        self._authority = authority_from_headers(pushed_headers)
        return [], events

    def receive_headers(self, headers, end_stream, header_encoding):
        """
        Receive a set of headers (or trailers).
        """
        if is_informational_response(headers):
            if end_stream:
                raise ProtocolError(
                    "Cannot set END_STREAM on informational responses"
                )
            input_ = StreamInputs.RECV_INFORMATIONAL_HEADERS
        else:
            input_ = StreamInputs.RECV_HEADERS

        events = self.state_machine.process_input(input_)

        if end_stream:
            es_events = self.state_machine.process_input(
                StreamInputs.RECV_END_STREAM
            )
            events[0].stream_ended = es_events[0]
            events += es_events

        self._initialize_content_length(headers)

        if isinstance(events[0], TrailersReceived):
            if not end_stream:
                raise ProtocolError("Trailers must have END_STREAM set")

        hdr_validation_flags = self._build_hdr_validation_flags(events)
        events[0].headers = self._process_received_headers(
            headers, hdr_validation_flags, header_encoding
        )
        return [], events

    def receive_data(self, data, end_stream, flow_control_len):
        """
        Receive some data.
        """
        self.config.logger.debug(
            "Receive data on %r with end stream %s and flow control length "
            "set to %d", self, end_stream, flow_control_len
        )
        events = self.state_machine.process_input(StreamInputs.RECV_DATA)
        self._inbound_window_manager.window_consumed(flow_control_len)
        self._track_content_length(len(data), end_stream)

        if end_stream:
            es_events = self.state_machine.process_input(
                StreamInputs.RECV_END_STREAM
            )
            events[0].stream_ended = es_events[0]
            events.extend(es_events)

        events[0].data = data
        events[0].flow_controlled_length = flow_control_len
        return [], events

    def receive_window_update(self, increment):
        """
        Handle a WINDOW_UPDATE increment.
        """
        self.config.logger.debug(
            "Receive Window Update on %r for increment of %d",
            self, increment
        )
        events = self.state_machine.process_input(
            StreamInputs.RECV_WINDOW_UPDATE
        )
        frames = []

        # If we encounter a problem with incrementing the flow control window,
        # this should be treated as a *stream* error, not a *connection* error.
        # That means we need to catch the error and forcibly close the stream.
        if events:
            events[0].delta = increment
            try:
                self.outbound_flow_control_window = guard_increment_window(
                    self.outbound_flow_control_window,
                    increment
                )
            except FlowControlError:
                # Ok, this is bad. We're going to need to perform a local
                # reset.
                event = StreamReset()
                event.stream_id = self.stream_id
                event.error_code = ErrorCodes.FLOW_CONTROL_ERROR
                event.remote_reset = False

                events = [event]
                frames = self.reset_stream(event.error_code)

        return frames, events

    def receive_continuation(self):
        """
        A naked CONTINUATION frame has been received. This is always an error,
        but the type of error it is depends on the state of the stream and must
        transition the state of the stream, so we need to handle it.
        """
        self.config.logger.debug("Receive Continuation frame on %r", self)
        self.state_machine.process_input(
            StreamInputs.RECV_CONTINUATION
        )
        assert False, "Should not be reachable"

    def receive_alt_svc(self, frame):
        """
        An Alternative Service frame was received on the stream. This frame
        inherits the origin associated with this stream.
        """
        self.config.logger.debug(
            "Receive Alternative Service frame on stream %r", self
        )

        # If the origin is present, RFC 7838 says we have to ignore it.
        if frame.origin:
            return [], []

        events = self.state_machine.process_input(
            StreamInputs.RECV_ALTERNATIVE_SERVICE
        )

        # There are lots of situations where we want to ignore the ALTSVC
        # frame. If we need to pay attention, we'll have an event and should
        # fill it out.
        if events:
            assert isinstance(events[0], AlternativeServiceAvailable)
            events[0].origin = self._authority
            events[0].field_value = frame.field

        return [], events

    def reset_stream(self, error_code=0):
        """
        Close the stream locally. Reset the stream with an error code.
        """
        self.config.logger.debug(
            "Local reset %r with error code: %d", self, error_code
        )
        self.state_machine.process_input(StreamInputs.SEND_RST_STREAM)

        rsf = RstStreamFrame(self.stream_id)
        rsf.error_code = error_code
        return [rsf]

    def stream_reset(self, frame):
        """
        Handle a stream being reset remotely.
        """
        self.config.logger.debug(
            "Remote reset %r with error code: %d", self, frame.error_code
        )
        events = self.state_machine.process_input(StreamInputs.RECV_RST_STREAM)

        if events:
            # We don't fire an event if this stream is already closed.
            events[0].error_code = _error_code_from_int(frame.error_code)

        return [], events

    def acknowledge_received_data(self, acknowledged_size):
        """
        The user has informed us that they've processed some amount of data
        that was received on this stream. Pass that to the window manager and
        potentially return some WindowUpdate frames.
        """
        self.config.logger.debug(
            "Acknowledge received data with size %d on %r",
            acknowledged_size, self
        )
        increment = self._inbound_window_manager.process_bytes(
            acknowledged_size
        )
        if increment:
            f = WindowUpdateFrame(self.stream_id)
            f.window_increment = increment
            return [f]

        return []

    def _build_hdr_validation_flags(self, events):
        """
        Constructs a set of header validation flags for use when normalizing
        and validating header blocks.
        """
        is_trailer = isinstance(
            events[0], (_TrailersSent, TrailersReceived)
        )
        is_response_header = isinstance(
            events[0],
            (
                _ResponseSent,
                ResponseReceived,
                InformationalResponseReceived
            )
        )
        is_push_promise = isinstance(
            events[0], (PushedStreamReceived, _PushedRequestSent)
        )

        return HeaderValidationFlags(
            is_client=self.state_machine.client,
            is_trailer=is_trailer,
            is_response_header=is_response_header,
            is_push_promise=is_push_promise,
        )

    def _build_headers_frames(self,
                              headers,
                              encoder,
                              first_frame,
                              hdr_validation_flags):
        """
        Helper method to build headers or push promise frames.
        """
        # We need to lowercase the header names, and to ensure that secure
        # header fields are kept out of compression contexts.
        if self.config.normalize_outbound_headers:
            headers = normalize_outbound_headers(
                headers, hdr_validation_flags
            )
        if self.config.validate_outbound_headers:
            headers = validate_outbound_headers(
                headers, hdr_validation_flags
            )

        encoded_headers = encoder.encode(headers)

        # Slice into blocks of max_outbound_frame_size. Be careful with this:
        # it only works right because we never send padded frames or priority
        # information on the frames. Revisit this if we do.
        header_blocks = [
            encoded_headers[i:i+self.max_outbound_frame_size]
            for i in range(
                0, len(encoded_headers), self.max_outbound_frame_size
            )
        ]

        frames = []
        first_frame.data = header_blocks[0]
        frames.append(first_frame)

        for block in header_blocks[1:]:
            cf = ContinuationFrame(self.stream_id)
            cf.data = block
            frames.append(cf)

        frames[-1].flags.add('END_HEADERS')
        return frames

    def _process_received_headers(self,
                                  headers,
                                  header_validation_flags,
                                  header_encoding):
        """
        When headers have been received from the remote peer, run a processing
        pipeline on them to transform them into the appropriate form for
        attaching to an event.
        """
        if self.config.normalize_inbound_headers:
            headers = normalize_inbound_headers(
                headers, header_validation_flags
            )

        if self.config.validate_inbound_headers:
            headers = validate_headers(headers, header_validation_flags)

        if header_encoding:
            headers = _decode_headers(headers, header_encoding)

        # The above steps are all generators, so we need to concretize the
        # headers now.
        return list(headers)

    def _initialize_content_length(self, headers):
        """
        Checks the headers for a content-length header and initializes the
        _expected_content_length field from it. It's not an error for no
        Content-Length header to be present.
        """
        if self.request_method == b'HEAD':
            self._expected_content_length = 0
            return

        for n, v in headers:
            if n == b'content-length':
                try:
                    self._expected_content_length = int(v, 10)
                except ValueError:
                    raise ProtocolError(
                        "Invalid content-length header: %s" % v
                    )

                return

    def _track_content_length(self, length, end_stream):
        """
        Update the expected content length in response to data being received.
        Validates that the appropriate amount of data is sent. Always updates
        the received data, but only validates the length against the
        content-length header if one was sent.

        :param length: The length of the body chunk received.
        :param end_stream: If this is the last body chunk received.
        """
        self._actual_content_length += length
        actual = self._actual_content_length
        expected = self._expected_content_length

        if expected is not None:
            if expected < actual:
                raise InvalidBodyLengthError(expected, actual)

            if end_stream and expected != actual:
                raise InvalidBodyLengthError(expected, actual)

    def _inbound_flow_control_change_from_settings(self, delta):
        """
        We changed SETTINGS_INITIAL_WINDOW_SIZE, which means we need to
        update the target window size for flow control. For our flow control
        strategy, this means we need to do two things: we need to adjust the
        current window size, but we also need to set the target maximum window
        size to the new value.
        """
        new_max_size = self._inbound_window_manager.max_window_size + delta
        self._inbound_window_manager.window_opened(delta)
        self._inbound_window_manager.max_window_size = new_max_size


def _decode_headers(headers, encoding):
    """
    Given an iterable of header two-tuples and an encoding, decodes those
    headers using that encoding while preserving the type of the header tuple.
    This ensures that the use of ``HeaderTuple`` is preserved.
    """
    for header in headers:
        # This function expects to work on decoded headers, which are always
        # HeaderTuple objects.
        assert isinstance(header, HeaderTuple)

        name, value = header
        name = name.decode(encoding)
        value = value.decode(encoding)
        yield header.__class__(name, value)
