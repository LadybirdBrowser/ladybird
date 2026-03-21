# -*- coding: utf-8 -*-
"""
h2/connection
~~~~~~~~~~~~~

An implementation of a HTTP/2 connection.
"""
import base64

from enum import Enum, IntEnum

from hyperframe.exceptions import InvalidPaddingError
from hyperframe.frame import (
    GoAwayFrame, WindowUpdateFrame, HeadersFrame, DataFrame, PingFrame,
    PushPromiseFrame, SettingsFrame, RstStreamFrame, PriorityFrame,
    ContinuationFrame, AltSvcFrame, ExtensionFrame
)
from hpack.hpack import Encoder, Decoder
from hpack.exceptions import HPACKError, OversizedHeaderListError

from .config import H2Configuration
from .errors import ErrorCodes, _error_code_from_int
from .events import (
    WindowUpdated, RemoteSettingsChanged, PingReceived, PingAckReceived,
    SettingsAcknowledged, ConnectionTerminated, PriorityUpdated,
    AlternativeServiceAvailable, UnknownFrameReceived
)
from .exceptions import (
    ProtocolError, NoSuchStreamError, FlowControlError, FrameTooLargeError,
    TooManyStreamsError, StreamClosedError, StreamIDTooLowError,
    NoAvailableStreamIDError, RFC1122Error, DenialOfServiceError
)
from .frame_buffer import FrameBuffer
from .settings import Settings, SettingCodes
from .stream import H2Stream, StreamClosedBy
from .utilities import SizeLimitDict, guard_increment_window
from .windows import WindowManager


class ConnectionState(Enum):
    IDLE = 0
    CLIENT_OPEN = 1
    SERVER_OPEN = 2
    CLOSED = 3


class ConnectionInputs(Enum):
    SEND_HEADERS = 0
    SEND_PUSH_PROMISE = 1
    SEND_DATA = 2
    SEND_GOAWAY = 3
    SEND_WINDOW_UPDATE = 4
    SEND_PING = 5
    SEND_SETTINGS = 6
    SEND_RST_STREAM = 7
    SEND_PRIORITY = 8
    RECV_HEADERS = 9
    RECV_PUSH_PROMISE = 10
    RECV_DATA = 11
    RECV_GOAWAY = 12
    RECV_WINDOW_UPDATE = 13
    RECV_PING = 14
    RECV_SETTINGS = 15
    RECV_RST_STREAM = 16
    RECV_PRIORITY = 17
    SEND_ALTERNATIVE_SERVICE = 18  # Added in 2.3.0
    RECV_ALTERNATIVE_SERVICE = 19  # Added in 2.3.0


class AllowedStreamIDs(IntEnum):
    EVEN = 0
    ODD = 1


class H2ConnectionStateMachine:
    """
    A single HTTP/2 connection state machine.

    This state machine, while defined in its own class, is logically part of
    the H2Connection class also defined in this file. The state machine itself
    maintains very little state directly, instead focusing entirely on managing
    state transitions.
    """
    # For the purposes of this state machine we treat HEADERS and their
    # associated CONTINUATION frames as a single jumbo frame. The protocol
    # allows/requires this by preventing other frames from being interleved in
    # between HEADERS/CONTINUATION frames.
    #
    # The _transitions dictionary contains a mapping of tuples of
    # (state, input) to tuples of (side_effect_function, end_state). This map
    # contains all allowed transitions: anything not in this map is invalid
    # and immediately causes a transition to ``closed``.

    _transitions = {
        # State: idle
        (ConnectionState.IDLE, ConnectionInputs.SEND_HEADERS):
            (None, ConnectionState.CLIENT_OPEN),
        (ConnectionState.IDLE, ConnectionInputs.RECV_HEADERS):
            (None, ConnectionState.SERVER_OPEN),
        (ConnectionState.IDLE, ConnectionInputs.SEND_SETTINGS):
            (None, ConnectionState.IDLE),
        (ConnectionState.IDLE, ConnectionInputs.RECV_SETTINGS):
            (None, ConnectionState.IDLE),
        (ConnectionState.IDLE, ConnectionInputs.SEND_WINDOW_UPDATE):
            (None, ConnectionState.IDLE),
        (ConnectionState.IDLE, ConnectionInputs.RECV_WINDOW_UPDATE):
            (None, ConnectionState.IDLE),
        (ConnectionState.IDLE, ConnectionInputs.SEND_PING):
            (None, ConnectionState.IDLE),
        (ConnectionState.IDLE, ConnectionInputs.RECV_PING):
            (None, ConnectionState.IDLE),
        (ConnectionState.IDLE, ConnectionInputs.SEND_GOAWAY):
            (None, ConnectionState.CLOSED),
        (ConnectionState.IDLE, ConnectionInputs.RECV_GOAWAY):
            (None, ConnectionState.CLOSED),
        (ConnectionState.IDLE, ConnectionInputs.SEND_PRIORITY):
            (None, ConnectionState.IDLE),
        (ConnectionState.IDLE, ConnectionInputs.RECV_PRIORITY):
            (None, ConnectionState.IDLE),
        (ConnectionState.IDLE, ConnectionInputs.SEND_ALTERNATIVE_SERVICE):
            (None, ConnectionState.SERVER_OPEN),
        (ConnectionState.IDLE, ConnectionInputs.RECV_ALTERNATIVE_SERVICE):
            (None, ConnectionState.CLIENT_OPEN),

        # State: open, client side.
        (ConnectionState.CLIENT_OPEN, ConnectionInputs.SEND_HEADERS):
            (None, ConnectionState.CLIENT_OPEN),
        (ConnectionState.CLIENT_OPEN, ConnectionInputs.SEND_DATA):
            (None, ConnectionState.CLIENT_OPEN),
        (ConnectionState.CLIENT_OPEN, ConnectionInputs.SEND_GOAWAY):
            (None, ConnectionState.CLOSED),
        (ConnectionState.CLIENT_OPEN, ConnectionInputs.SEND_WINDOW_UPDATE):
            (None, ConnectionState.CLIENT_OPEN),
        (ConnectionState.CLIENT_OPEN, ConnectionInputs.SEND_PING):
            (None, ConnectionState.CLIENT_OPEN),
        (ConnectionState.CLIENT_OPEN, ConnectionInputs.SEND_SETTINGS):
            (None, ConnectionState.CLIENT_OPEN),
        (ConnectionState.CLIENT_OPEN, ConnectionInputs.SEND_PRIORITY):
            (None, ConnectionState.CLIENT_OPEN),
        (ConnectionState.CLIENT_OPEN, ConnectionInputs.RECV_HEADERS):
            (None, ConnectionState.CLIENT_OPEN),
        (ConnectionState.CLIENT_OPEN, ConnectionInputs.RECV_PUSH_PROMISE):
            (None, ConnectionState.CLIENT_OPEN),
        (ConnectionState.CLIENT_OPEN, ConnectionInputs.RECV_DATA):
            (None, ConnectionState.CLIENT_OPEN),
        (ConnectionState.CLIENT_OPEN, ConnectionInputs.RECV_GOAWAY):
            (None, ConnectionState.CLOSED),
        (ConnectionState.CLIENT_OPEN, ConnectionInputs.RECV_WINDOW_UPDATE):
            (None, ConnectionState.CLIENT_OPEN),
        (ConnectionState.CLIENT_OPEN, ConnectionInputs.RECV_PING):
            (None, ConnectionState.CLIENT_OPEN),
        (ConnectionState.CLIENT_OPEN, ConnectionInputs.RECV_SETTINGS):
            (None, ConnectionState.CLIENT_OPEN),
        (ConnectionState.CLIENT_OPEN, ConnectionInputs.SEND_RST_STREAM):
            (None, ConnectionState.CLIENT_OPEN),
        (ConnectionState.CLIENT_OPEN, ConnectionInputs.RECV_RST_STREAM):
            (None, ConnectionState.CLIENT_OPEN),
        (ConnectionState.CLIENT_OPEN, ConnectionInputs.RECV_PRIORITY):
            (None, ConnectionState.CLIENT_OPEN),
        (ConnectionState.CLIENT_OPEN,
            ConnectionInputs.RECV_ALTERNATIVE_SERVICE):
                (None, ConnectionState.CLIENT_OPEN),

        # State: open, server side.
        (ConnectionState.SERVER_OPEN, ConnectionInputs.SEND_HEADERS):
            (None, ConnectionState.SERVER_OPEN),
        (ConnectionState.SERVER_OPEN, ConnectionInputs.SEND_PUSH_PROMISE):
            (None, ConnectionState.SERVER_OPEN),
        (ConnectionState.SERVER_OPEN, ConnectionInputs.SEND_DATA):
            (None, ConnectionState.SERVER_OPEN),
        (ConnectionState.SERVER_OPEN, ConnectionInputs.SEND_GOAWAY):
            (None, ConnectionState.CLOSED),
        (ConnectionState.SERVER_OPEN, ConnectionInputs.SEND_WINDOW_UPDATE):
            (None, ConnectionState.SERVER_OPEN),
        (ConnectionState.SERVER_OPEN, ConnectionInputs.SEND_PING):
            (None, ConnectionState.SERVER_OPEN),
        (ConnectionState.SERVER_OPEN, ConnectionInputs.SEND_SETTINGS):
            (None, ConnectionState.SERVER_OPEN),
        (ConnectionState.SERVER_OPEN, ConnectionInputs.SEND_PRIORITY):
            (None, ConnectionState.SERVER_OPEN),
        (ConnectionState.SERVER_OPEN, ConnectionInputs.RECV_HEADERS):
            (None, ConnectionState.SERVER_OPEN),
        (ConnectionState.SERVER_OPEN, ConnectionInputs.RECV_DATA):
            (None, ConnectionState.SERVER_OPEN),
        (ConnectionState.SERVER_OPEN, ConnectionInputs.RECV_GOAWAY):
            (None, ConnectionState.CLOSED),
        (ConnectionState.SERVER_OPEN, ConnectionInputs.RECV_WINDOW_UPDATE):
            (None, ConnectionState.SERVER_OPEN),
        (ConnectionState.SERVER_OPEN, ConnectionInputs.RECV_PING):
            (None, ConnectionState.SERVER_OPEN),
        (ConnectionState.SERVER_OPEN, ConnectionInputs.RECV_SETTINGS):
            (None, ConnectionState.SERVER_OPEN),
        (ConnectionState.SERVER_OPEN, ConnectionInputs.RECV_PRIORITY):
            (None, ConnectionState.SERVER_OPEN),
        (ConnectionState.SERVER_OPEN, ConnectionInputs.SEND_RST_STREAM):
            (None, ConnectionState.SERVER_OPEN),
        (ConnectionState.SERVER_OPEN, ConnectionInputs.RECV_RST_STREAM):
            (None, ConnectionState.SERVER_OPEN),
        (ConnectionState.SERVER_OPEN,
            ConnectionInputs.SEND_ALTERNATIVE_SERVICE):
                (None, ConnectionState.SERVER_OPEN),
        (ConnectionState.SERVER_OPEN,
            ConnectionInputs.RECV_ALTERNATIVE_SERVICE):
                (None, ConnectionState.SERVER_OPEN),

        # State: closed
        (ConnectionState.CLOSED, ConnectionInputs.SEND_GOAWAY):
            (None, ConnectionState.CLOSED),
        (ConnectionState.CLOSED, ConnectionInputs.RECV_GOAWAY):
            (None, ConnectionState.CLOSED),
    }

    def __init__(self):
        self.state = ConnectionState.IDLE

    def process_input(self, input_):
        """
        Process a specific input in the state machine.
        """
        if not isinstance(input_, ConnectionInputs):
            raise ValueError("Input must be an instance of ConnectionInputs")

        try:
            func, target_state = self._transitions[(self.state, input_)]
        except KeyError:
            old_state = self.state
            self.state = ConnectionState.CLOSED
            raise ProtocolError(
                "Invalid input %s in state %s" % (input_, old_state)
            )
        else:
            self.state = target_state
            if func is not None:  # pragma: no cover
                return func()

            return []


class H2Connection:
    """
    A low-level HTTP/2 connection object. This handles building and receiving
    frames and maintains both connection and per-stream state for all streams
    on this connection.

    This wraps a HTTP/2 Connection state machine implementation, ensuring that
    frames can only be sent/received when the connection is in a valid state.
    It also builds stream state machines on demand to ensure that the
    constraints of those state machines are met as well. Attempts to create
    frames that cannot be sent will raise a ``ProtocolError``.

    .. versionchanged:: 2.3.0
       Added the ``header_encoding`` keyword argument.

    .. versionchanged:: 2.5.0
       Added the ``config`` keyword argument. Deprecated the ``client_side``
       and ``header_encoding`` parameters.

    .. versionchanged:: 3.0.0
       Removed deprecated parameters and properties.

    :param config: The configuration for the HTTP/2 connection.

        .. versionadded:: 2.5.0

    :type config: :class:`H2Configuration <h2.config.H2Configuration>`
    """
    # The initial maximum outbound frame size. This can be changed by receiving
    # a settings frame.
    DEFAULT_MAX_OUTBOUND_FRAME_SIZE = 65535

    # The initial maximum inbound frame size. This is somewhat arbitrarily
    # chosen.
    DEFAULT_MAX_INBOUND_FRAME_SIZE = 2**24

    # The highest acceptable stream ID.
    HIGHEST_ALLOWED_STREAM_ID = 2**31 - 1

    # The largest acceptable window increment.
    MAX_WINDOW_INCREMENT = 2**31 - 1

    # The initial default value of SETTINGS_MAX_HEADER_LIST_SIZE.
    DEFAULT_MAX_HEADER_LIST_SIZE = 2**16

    # Keep in memory limited amount of results for streams closes
    MAX_CLOSED_STREAMS = 2**16

    def __init__(self, config=None):
        self.state_machine = H2ConnectionStateMachine()
        self.streams = {}
        self.highest_inbound_stream_id = 0
        self.highest_outbound_stream_id = 0
        self.encoder = Encoder()
        self.decoder = Decoder()

        # This won't always actually do anything: for versions of HPACK older
        # than 2.3.0 it does nothing. However, we have to try!
        self.decoder.max_header_list_size = self.DEFAULT_MAX_HEADER_LIST_SIZE

        #: The configuration for this HTTP/2 connection object.
        #:
        #: .. versionadded:: 2.5.0
        self.config = config
        if self.config is None:
            self.config = H2Configuration(
                client_side=True,
            )

        # Objects that store settings, including defaults.
        #
        # We set the MAX_CONCURRENT_STREAMS value to 100 because its default is
        # unbounded, and that's a dangerous default because it allows
        # essentially unbounded resources to be allocated regardless of how
        # they will be used. 100 should be suitable for the average
        # application. This default obviously does not apply to the remote
        # peer's settings: the remote peer controls them!
        #
        # We also set MAX_HEADER_LIST_SIZE to a reasonable value. This is to
        # advertise our defence against CVE-2016-6581. However, not all
        # versions of HPACK will let us do it. That's ok: we should at least
        # suggest that we're not vulnerable.
        self.local_settings = Settings(
            client=self.config.client_side,
            initial_values={
                SettingCodes.MAX_CONCURRENT_STREAMS: 100,
                SettingCodes.MAX_HEADER_LIST_SIZE:
                    self.DEFAULT_MAX_HEADER_LIST_SIZE,
            }
        )
        self.remote_settings = Settings(client=not self.config.client_side)

        # The current value of the connection flow control windows on the
        # connection.
        self.outbound_flow_control_window = (
            self.remote_settings.initial_window_size
        )

        #: The maximum size of a frame that can be emitted by this peer, in
        #: bytes.
        self.max_outbound_frame_size = self.remote_settings.max_frame_size

        #: The maximum size of a frame that can be received by this peer, in
        #: bytes.
        self.max_inbound_frame_size = self.local_settings.max_frame_size

        # Buffer for incoming data.
        self.incoming_buffer = FrameBuffer(server=not self.config.client_side)

        # A private variable to store a sequence of received header frames
        # until completion.
        self._header_frames = []

        # Data that needs to be sent.
        self._data_to_send = bytearray()

        # Keeps track of how streams are closed.
        # Used to ensure that we don't blow up in the face of frames that were
        # in flight when a RST_STREAM was sent.
        # Also used to determine whether we should consider a frame received
        # while a stream is closed as either a stream error or a connection
        # error.
        self._closed_streams = SizeLimitDict(
            size_limit=self.MAX_CLOSED_STREAMS
        )

        # The flow control window manager for the connection.
        self._inbound_flow_control_window_manager = WindowManager(
            max_window_size=self.local_settings.initial_window_size
        )

        # When in doubt use dict-dispatch.
        self._frame_dispatch_table = {
            HeadersFrame: self._receive_headers_frame,
            PushPromiseFrame: self._receive_push_promise_frame,
            SettingsFrame: self._receive_settings_frame,
            DataFrame: self._receive_data_frame,
            WindowUpdateFrame: self._receive_window_update_frame,
            PingFrame: self._receive_ping_frame,
            RstStreamFrame: self._receive_rst_stream_frame,
            PriorityFrame: self._receive_priority_frame,
            GoAwayFrame: self._receive_goaway_frame,
            ContinuationFrame: self._receive_naked_continuation,
            AltSvcFrame: self._receive_alt_svc_frame,
            ExtensionFrame: self._receive_unknown_frame
        }

    def _prepare_for_sending(self, frames):
        if not frames:
            return
        self._data_to_send += b''.join(f.serialize() for f in frames)
        assert all(f.body_len <= self.max_outbound_frame_size for f in frames)

    def _open_streams(self, remainder):
        """
        A common method of counting number of open streams. Returns the number
        of streams that are open *and* that have (stream ID % 2) == remainder.
        While it iterates, also deletes any closed streams.
        """
        count = 0
        to_delete = []

        for stream_id, stream in self.streams.items():
            if stream.open and (stream_id % 2 == remainder):
                count += 1
            elif stream.closed:
                to_delete.append(stream_id)

        for stream_id in to_delete:
            stream = self.streams.pop(stream_id)
            self._closed_streams[stream_id] = stream.closed_by

        return count

    @property
    def open_outbound_streams(self):
        """
        The current number of open outbound streams.
        """
        outbound_numbers = int(self.config.client_side)
        return self._open_streams(outbound_numbers)

    @property
    def open_inbound_streams(self):
        """
        The current number of open inbound streams.
        """
        inbound_numbers = int(not self.config.client_side)
        return self._open_streams(inbound_numbers)

    @property
    def inbound_flow_control_window(self):
        """
        The size of the inbound flow control window for the connection. This is
        rarely publicly useful: instead, use :meth:`remote_flow_control_window
        <h2.connection.H2Connection.remote_flow_control_window>`. This
        shortcut is largely present to provide a shortcut to this data.
        """
        return self._inbound_flow_control_window_manager.current_window_size

    def _begin_new_stream(self, stream_id, allowed_ids):
        """
        Initiate a new stream.

        .. versionchanged:: 2.0.0
           Removed this function from the public API.

        :param stream_id: The ID of the stream to open.
        :param allowed_ids: What kind of stream ID is allowed.
        """
        self.config.logger.debug(
            "Attempting to initiate stream ID %d", stream_id
        )
        outbound = self._stream_id_is_outbound(stream_id)
        highest_stream_id = (
            self.highest_outbound_stream_id if outbound else
            self.highest_inbound_stream_id
        )

        if stream_id <= highest_stream_id:
            raise StreamIDTooLowError(stream_id, highest_stream_id)

        if (stream_id % 2) != int(allowed_ids):
            raise ProtocolError(
                "Invalid stream ID for peer."
            )

        s = H2Stream(
            stream_id,
            config=self.config,
            inbound_window_size=self.local_settings.initial_window_size,
            outbound_window_size=self.remote_settings.initial_window_size
        )
        self.config.logger.debug("Stream ID %d created", stream_id)
        s.max_inbound_frame_size = self.max_inbound_frame_size
        s.max_outbound_frame_size = self.max_outbound_frame_size

        self.streams[stream_id] = s
        self.config.logger.debug("Current streams: %s", self.streams.keys())

        if outbound:
            self.highest_outbound_stream_id = stream_id
        else:
            self.highest_inbound_stream_id = stream_id

        return s

    def initiate_connection(self):
        """
        Provides any data that needs to be sent at the start of the connection.
        Must be called for both clients and servers.
        """
        self.config.logger.debug("Initializing connection")
        self.state_machine.process_input(ConnectionInputs.SEND_SETTINGS)
        if self.config.client_side:
            preamble = b'PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n'
        else:
            preamble = b''

        f = SettingsFrame(0)
        for setting, value in self.local_settings.items():
            f.settings[setting] = value
        self.config.logger.debug(
            "Send Settings frame: %s", self.local_settings
        )

        self._data_to_send += preamble + f.serialize()

    def initiate_upgrade_connection(self, settings_header=None):
        """
        Call to initialise the connection object for use with an upgraded
        HTTP/2 connection (i.e. a connection negotiated using the
        ``Upgrade: h2c`` HTTP header).

        This method differs from :meth:`initiate_connection
        <h2.connection.H2Connection.initiate_connection>` in several ways.
        Firstly, it handles the additional SETTINGS frame that is sent in the
        ``HTTP2-Settings`` header field. When called on a client connection,
        this method will return a bytestring that the caller can put in the
        ``HTTP2-Settings`` field they send on their initial request. When
        called on a server connection, the user **must** provide the value they
        received from the client in the ``HTTP2-Settings`` header field to the
        ``settings_header`` argument, which will be used appropriately.

        Additionally, this method sets up stream 1 in a half-closed state
        appropriate for this side of the connection, to reflect the fact that
        the request is already complete.

        Finally, this method also prepares the appropriate preamble to be sent
        after the upgrade.

        .. versionadded:: 2.3.0

        :param settings_header: (optional, server-only): The value of the
             ``HTTP2-Settings`` header field received from the client.
        :type settings_header: ``bytes``

        :returns: For clients, a bytestring to put in the ``HTTP2-Settings``.
            For servers, returns nothing.
        :rtype: ``bytes`` or ``None``
        """
        self.config.logger.debug(
            "Upgrade connection. Current settings: %s", self.local_settings
        )

        frame_data = None
        # Begin by getting the preamble in place.
        self.initiate_connection()

        if self.config.client_side:
            f = SettingsFrame(0)
            for setting, value in self.local_settings.items():
                f.settings[setting] = value

            frame_data = f.serialize_body()
            frame_data = base64.urlsafe_b64encode(frame_data)
        elif settings_header:
            # We have a settings header from the client. This needs to be
            # applied, but we want to throw away the ACK. We do this by
            # inserting the data into a Settings frame and then passing it to
            # the state machine, but ignoring the return value.
            settings_header = base64.urlsafe_b64decode(settings_header)
            f = SettingsFrame(0)
            f.parse_body(settings_header)
            self._receive_settings_frame(f)

        # Set up appropriate state. Stream 1 in a half-closed state:
        # half-closed(local) for clients, half-closed(remote) for servers.
        # Additionally, we need to set up the Connection state machine.
        connection_input = (
            ConnectionInputs.SEND_HEADERS if self.config.client_side
            else ConnectionInputs.RECV_HEADERS
        )
        self.config.logger.debug("Process input %s", connection_input)
        self.state_machine.process_input(connection_input)

        # Set up stream 1.
        self._begin_new_stream(stream_id=1, allowed_ids=AllowedStreamIDs.ODD)
        self.streams[1].upgrade(self.config.client_side)
        return frame_data

    def _get_or_create_stream(self, stream_id, allowed_ids):
        """
        Gets a stream by its stream ID. Will create one if one does not already
        exist. Use allowed_ids to circumvent the usual stream ID rules for
        clients and servers.

        .. versionchanged:: 2.0.0
           Removed this function from the public API.
        """
        try:
            return self.streams[stream_id]
        except KeyError:
            return self._begin_new_stream(stream_id, allowed_ids)

    def _get_stream_by_id(self, stream_id):
        """
        Gets a stream by its stream ID. Raises NoSuchStreamError if the stream
        ID does not correspond to a known stream and is higher than the current
        maximum: raises if it is lower than the current maximum.

        .. versionchanged:: 2.0.0
           Removed this function from the public API.
        """
        try:
            return self.streams[stream_id]
        except KeyError:
            outbound = self._stream_id_is_outbound(stream_id)
            highest_stream_id = (
                self.highest_outbound_stream_id if outbound else
                self.highest_inbound_stream_id
            )

            if stream_id > highest_stream_id:
                raise NoSuchStreamError(stream_id)
            else:
                raise StreamClosedError(stream_id)

    def get_next_available_stream_id(self):
        """
        Returns an integer suitable for use as the stream ID for the next
        stream created by this endpoint. For server endpoints, this stream ID
        will be even. For client endpoints, this stream ID will be odd. If no
        stream IDs are available, raises :class:`NoAvailableStreamIDError
        <h2.exceptions.NoAvailableStreamIDError>`.

        .. warning:: The return value from this function does not change until
                     the stream ID has actually been used by sending or pushing
                     headers on that stream. For that reason, it should be
                     called as close as possible to the actual use of the
                     stream ID.

        .. versionadded:: 2.0.0

        :raises: :class:`NoAvailableStreamIDError
            <h2.exceptions.NoAvailableStreamIDError>`
        :returns: The next free stream ID this peer can use to initiate a
            stream.
        :rtype: ``int``
        """
        # No streams have been opened yet, so return the lowest allowed stream
        # ID.
        if not self.highest_outbound_stream_id:
            next_stream_id = 1 if self.config.client_side else 2
        else:
            next_stream_id = self.highest_outbound_stream_id + 2
        self.config.logger.debug(
            "Next available stream ID %d", next_stream_id
        )
        if next_stream_id > self.HIGHEST_ALLOWED_STREAM_ID:
            raise NoAvailableStreamIDError("Exhausted allowed stream IDs")

        return next_stream_id

    def send_headers(self, stream_id, headers, end_stream=False,
                     priority_weight=None, priority_depends_on=None,
                     priority_exclusive=None):
        """
        Send headers on a given stream.

        This function can be used to send request or response headers: the kind
        that are sent depends on whether this connection has been opened as a
        client or server connection, and whether the stream was opened by the
        remote peer or not.

        If this is a client connection, calling ``send_headers`` will send the
        headers as a request. It will also implicitly open the stream being
        used. If this is a client connection and ``send_headers`` has *already*
        been called, this will send trailers instead.

        If this is a server connection, calling ``send_headers`` will send the
        headers as a response. It is a protocol error for a server to open a
        stream by sending headers. If this is a server connection and
        ``send_headers`` has *already* been called, this will send trailers
        instead.

        When acting as a server, you may call ``send_headers`` any number of
        times allowed by the following rules, in this order:

        - zero or more times with ``(':status', '1XX')`` (where ``1XX`` is a
          placeholder for any 100-level status code).
        - once with any other status header.
        - zero or one time for trailers.

        That is, you are allowed to send as many informational responses as you
        like, followed by one complete response and zero or one HTTP trailer
        blocks.

        Clients may send one or two header blocks: one request block, and
        optionally one trailer block.

        If it is important to send HPACK "never indexed" header fields (as
        defined in `RFC 7451 Section 7.1.3
        <https://tools.ietf.org/html/rfc7541#section-7.1.3>`_), the user may
        instead provide headers using the HPACK library's :class:`HeaderTuple
        <hpack:hpack.HeaderTuple>` and :class:`NeverIndexedHeaderTuple
        <hpack:hpack.NeverIndexedHeaderTuple>` objects.

        This method also allows users to prioritize the stream immediately,
        by sending priority information on the HEADERS frame directly. To do
        this, any one of ``priority_weight``, ``priority_depends_on``, or
        ``priority_exclusive`` must be set to a value that is not ``None``. For
        more information on the priority fields, see :meth:`prioritize
        <h2.connection.H2Connection.prioritize>`.

        .. warning:: In HTTP/2, it is mandatory that all the HTTP/2 special
            headers (that is, ones whose header keys begin with ``:``) appear
            at the start of the header block, before any normal headers.

        .. versionchanged:: 2.3.0
           Added support for using :class:`HeaderTuple
           <hpack:hpack.HeaderTuple>` objects to store headers.

        .. versionchanged:: 2.4.0
           Added the ability to provide priority keyword arguments:
           ``priority_weight``, ``priority_depends_on``, and
           ``priority_exclusive``.

        :param stream_id: The stream ID to send the headers on. If this stream
            does not currently exist, it will be created.
        :type stream_id: ``int``

        :param headers: The request/response headers to send.
        :type headers: An iterable of two tuples of bytestrings or
            :class:`HeaderTuple <hpack:hpack.HeaderTuple>` objects.

        :param end_stream: Whether this headers frame should end the stream
            immediately (that is, whether no more data will be sent after this
            frame). Defaults to ``False``.
        :type end_stream: ``bool``

        :param priority_weight: Sets the priority weight of the stream. See
            :meth:`prioritize <h2.connection.H2Connection.prioritize>` for more
            about how this field works. Defaults to ``None``, which means that
            no priority information will be sent.
        :type priority_weight: ``int`` or ``None``

        :param priority_depends_on: Sets which stream this one depends on for
            priority purposes. See :meth:`prioritize
            <h2.connection.H2Connection.prioritize>` for more about how this
            field works. Defaults to ``None``, which means that no priority
            information will be sent.
        :type priority_depends_on: ``int`` or ``None``

        :param priority_exclusive: Sets whether this stream exclusively depends
            on the stream given in ``priority_depends_on`` for priority
            purposes. See :meth:`prioritize
            <h2.connection.H2Connection.prioritize>` for more about how this
            field workds. Defaults to ``None``, which means that no priority
            information will be sent.
        :type priority_depends_on: ``bool`` or ``None``

        :returns: Nothing
        """
        self.config.logger.debug(
            "Send headers on stream ID %d", stream_id
        )

        # Check we can open the stream.
        if stream_id not in self.streams:
            max_open_streams = self.remote_settings.max_concurrent_streams
            if (self.open_outbound_streams + 1) > max_open_streams:
                raise TooManyStreamsError(
                    "Max outbound streams is %d, %d open" %
                    (max_open_streams, self.open_outbound_streams)
                )

        self.state_machine.process_input(ConnectionInputs.SEND_HEADERS)
        stream = self._get_or_create_stream(
            stream_id, AllowedStreamIDs(self.config.client_side)
        )
        frames = stream.send_headers(
            headers, self.encoder, end_stream
        )

        # We may need to send priority information.
        priority_present = (
            (priority_weight is not None) or
            (priority_depends_on is not None) or
            (priority_exclusive is not None)
        )

        if priority_present:
            if not self.config.client_side:
                raise RFC1122Error("Servers SHOULD NOT prioritize streams.")

            headers_frame = frames[0]
            headers_frame.flags.add('PRIORITY')
            frames[0] = _add_frame_priority(
                headers_frame,
                priority_weight,
                priority_depends_on,
                priority_exclusive
            )

        self._prepare_for_sending(frames)

    def send_data(self, stream_id, data, end_stream=False, pad_length=None):
        """
        Send data on a given stream.

        This method does no breaking up of data: if the data is larger than the
        value returned by :meth:`local_flow_control_window
        <h2.connection.H2Connection.local_flow_control_window>` for this stream
        then a :class:`FlowControlError <h2.exceptions.FlowControlError>` will
        be raised. If the data is larger than :data:`max_outbound_frame_size
        <h2.connection.H2Connection.max_outbound_frame_size>` then a
        :class:`FrameTooLargeError <h2.exceptions.FrameTooLargeError>` will be
        raised.

        h2 does this to avoid buffering the data internally. If the user
        has more data to send than h2 will allow, consider breaking it up
        and buffering it externally.

        :param stream_id: The ID of the stream on which to send the data.
        :type stream_id: ``int``
        :param data: The data to send on the stream.
        :type data: ``bytes``
        :param end_stream: (optional) Whether this is the last data to be sent
            on the stream. Defaults to ``False``.
        :type end_stream: ``bool``
        :param pad_length: (optional) Length of the padding to apply to the
            data frame. Defaults to ``None`` for no use of padding. Note that
            a value of ``0`` results in padding of length ``0``
            (with the "padding" flag set on the frame).

            .. versionadded:: 2.6.0

        :type pad_length: ``int``
        :returns: Nothing
        """
        self.config.logger.debug(
            "Send data on stream ID %d with len %d", stream_id, len(data)
        )
        frame_size = len(data)
        if pad_length is not None:
            if not isinstance(pad_length, int):
                raise TypeError("pad_length must be an int")
            if pad_length < 0 or pad_length > 255:
                raise ValueError("pad_length must be within range: [0, 255]")
            # Account for padding bytes plus the 1-byte padding length field.
            frame_size += pad_length + 1
        self.config.logger.debug(
            "Frame size on stream ID %d is %d", stream_id, frame_size
        )

        if frame_size > self.local_flow_control_window(stream_id):
            raise FlowControlError(
                "Cannot send %d bytes, flow control window is %d." %
                (frame_size, self.local_flow_control_window(stream_id))
            )
        elif frame_size > self.max_outbound_frame_size:
            raise FrameTooLargeError(
                "Cannot send frame size %d, max frame size is %d" %
                (frame_size, self.max_outbound_frame_size)
            )

        self.state_machine.process_input(ConnectionInputs.SEND_DATA)
        frames = self.streams[stream_id].send_data(
            data, end_stream, pad_length=pad_length
        )

        self._prepare_for_sending(frames)

        self.outbound_flow_control_window -= frame_size
        self.config.logger.debug(
            "Outbound flow control window size is %d",
            self.outbound_flow_control_window
        )
        assert self.outbound_flow_control_window >= 0

    def end_stream(self, stream_id):
        """
        Cleanly end a given stream.

        This method ends a stream by sending an empty DATA frame on that stream
        with the ``END_STREAM`` flag set.

        :param stream_id: The ID of the stream to end.
        :type stream_id: ``int``
        :returns: Nothing
        """
        self.config.logger.debug("End stream ID %d", stream_id)
        self.state_machine.process_input(ConnectionInputs.SEND_DATA)
        frames = self.streams[stream_id].end_stream()
        self._prepare_for_sending(frames)

    def increment_flow_control_window(self, increment, stream_id=None):
        """
        Increment a flow control window, optionally for a single stream. Allows
        the remote peer to send more data.

        .. versionchanged:: 2.0.0
           Rejects attempts to increment the flow control window by out of
           range values with a ``ValueError``.

        :param increment: The amount to increment the flow control window by.
        :type increment: ``int``
        :param stream_id: (optional) The ID of the stream that should have its
            flow control window opened. If not present or ``None``, the
            connection flow control window will be opened instead.
        :type stream_id: ``int`` or ``None``
        :returns: Nothing
        :raises: ``ValueError``
        """
        if not (1 <= increment <= self.MAX_WINDOW_INCREMENT):
            raise ValueError(
                "Flow control increment must be between 1 and %d" %
                self.MAX_WINDOW_INCREMENT
            )

        self.state_machine.process_input(ConnectionInputs.SEND_WINDOW_UPDATE)

        if stream_id is not None:
            stream = self.streams[stream_id]
            frames = stream.increase_flow_control_window(
                increment
            )

            self.config.logger.debug(
                "Increase stream ID %d flow control window by %d",
                stream_id, increment
            )
        else:
            self._inbound_flow_control_window_manager.window_opened(increment)
            f = WindowUpdateFrame(0)
            f.window_increment = increment
            frames = [f]

            self.config.logger.debug(
                "Increase connection flow control window by %d", increment
            )

        self._prepare_for_sending(frames)

    def push_stream(self, stream_id, promised_stream_id, request_headers):
        """
        Push a response to the client by sending a PUSH_PROMISE frame.

        If it is important to send HPACK "never indexed" header fields (as
        defined in `RFC 7451 Section 7.1.3
        <https://tools.ietf.org/html/rfc7541#section-7.1.3>`_), the user may
        instead provide headers using the HPACK library's :class:`HeaderTuple
        <hpack:hpack.HeaderTuple>` and :class:`NeverIndexedHeaderTuple
        <hpack:hpack.NeverIndexedHeaderTuple>` objects.

        :param stream_id: The ID of the stream that this push is a response to.
        :type stream_id: ``int``
        :param promised_stream_id: The ID of the stream that the pushed
            response will be sent on.
        :type promised_stream_id: ``int``
        :param request_headers: The headers of the request that the pushed
            response will be responding to.
        :type request_headers: An iterable of two tuples of bytestrings or
            :class:`HeaderTuple <hpack:hpack.HeaderTuple>` objects.
        :returns: Nothing
        """
        self.config.logger.debug(
            "Send Push Promise frame on stream ID %d", stream_id
        )

        if not self.remote_settings.enable_push:
            raise ProtocolError("Remote peer has disabled stream push")

        self.state_machine.process_input(ConnectionInputs.SEND_PUSH_PROMISE)
        stream = self._get_stream_by_id(stream_id)

        # We need to prevent users pushing streams in response to streams that
        # they themselves have already pushed: see #163 and RFC 7540 § 6.6. The
        # easiest way to do that is to assert that the stream_id is not even:
        # this shortcut works because only servers can push and the state
        # machine will enforce this.
        if (stream_id % 2) == 0:
            raise ProtocolError("Cannot recursively push streams.")

        new_stream = self._begin_new_stream(
            promised_stream_id, AllowedStreamIDs.EVEN
        )
        self.streams[promised_stream_id] = new_stream

        frames = stream.push_stream_in_band(
            promised_stream_id, request_headers, self.encoder
        )
        new_frames = new_stream.locally_pushed()
        self._prepare_for_sending(frames + new_frames)

    def ping(self, opaque_data):
        """
        Send a PING frame.

        :param opaque_data: A bytestring of length 8 that will be sent in the
                            PING frame.
        :returns: Nothing
        """
        self.config.logger.debug("Send Ping frame")

        if not isinstance(opaque_data, bytes) or len(opaque_data) != 8:
            raise ValueError("Invalid value for ping data: %r" % opaque_data)

        self.state_machine.process_input(ConnectionInputs.SEND_PING)
        f = PingFrame(0)
        f.opaque_data = opaque_data
        self._prepare_for_sending([f])

    def reset_stream(self, stream_id, error_code=0):
        """
        Reset a stream.

        This method forcibly closes a stream by sending a RST_STREAM frame for
        a given stream. This is not a graceful closure. To gracefully end a
        stream, try the :meth:`end_stream
        <h2.connection.H2Connection.end_stream>` method.

        :param stream_id: The ID of the stream to reset.
        :type stream_id: ``int``
        :param error_code: (optional) The error code to use to reset the
            stream. Defaults to :data:`ErrorCodes.NO_ERROR
            <h2.errors.ErrorCodes.NO_ERROR>`.
        :type error_code: ``int``
        :returns: Nothing
        """
        self.config.logger.debug("Reset stream ID %d", stream_id)
        self.state_machine.process_input(ConnectionInputs.SEND_RST_STREAM)
        stream = self._get_stream_by_id(stream_id)
        frames = stream.reset_stream(error_code)
        self._prepare_for_sending(frames)

    def close_connection(self, error_code=0, additional_data=None,
                         last_stream_id=None):

        """
        Close a connection, emitting a GOAWAY frame.

        .. versionchanged:: 2.4.0
           Added ``additional_data`` and ``last_stream_id`` arguments.

        :param error_code: (optional) The error code to send in the GOAWAY
            frame.
        :param additional_data: (optional) Additional debug data indicating
            a reason for closing the connection. Must be a bytestring.
        :param last_stream_id: (optional) The last stream which was processed
            by the sender. Defaults to ``highest_inbound_stream_id``.
        :returns: Nothing
        """
        self.config.logger.debug("Close connection")
        self.state_machine.process_input(ConnectionInputs.SEND_GOAWAY)

        # Additional_data must be bytes
        if additional_data is not None:
            assert isinstance(additional_data, bytes)

        if last_stream_id is None:
            last_stream_id = self.highest_inbound_stream_id

        f = GoAwayFrame(
            stream_id=0,
            last_stream_id=last_stream_id,
            error_code=error_code,
            additional_data=(additional_data or b'')
        )
        self._prepare_for_sending([f])

    def update_settings(self, new_settings):
        """
        Update the local settings. This will prepare and emit the appropriate
        SETTINGS frame.

        :param new_settings: A dictionary of {setting: new value}
        """
        self.config.logger.debug(
            "Update connection settings to %s", new_settings
        )
        self.state_machine.process_input(ConnectionInputs.SEND_SETTINGS)
        self.local_settings.update(new_settings)
        s = SettingsFrame(0)
        s.settings = new_settings
        self._prepare_for_sending([s])

    def advertise_alternative_service(self,
                                      field_value,
                                      origin=None,
                                      stream_id=None):
        """
        Notify a client about an available Alternative Service.

        An Alternative Service is defined in `RFC 7838
        <https://tools.ietf.org/html/rfc7838>`_. An Alternative Service
        notification informs a client that a given origin is also available
        elsewhere.

        Alternative Services can be advertised in two ways. Firstly, they can
        be advertised explicitly: that is, a server can say "origin X is also
        available at Y". To advertise like this, set the ``origin`` argument
        and not the ``stream_id`` argument. Alternatively, they can be
        advertised implicitly: that is, a server can say "the origin you're
        contacting on stream X is also available at Y". To advertise like this,
        set the ``stream_id`` argument and not the ``origin`` argument.

        The explicit method of advertising can be done as long as the
        connection is active. The implicit method can only be done after the
        client has sent the request headers and before the server has sent the
        response headers: outside of those points, h2 will forbid sending
        the Alternative Service advertisement by raising a ProtocolError.

        The ``field_value`` parameter is specified in RFC 7838. h2 does
        not validate or introspect this argument: the user is required to
        ensure that it's well-formed. ``field_value`` corresponds to RFC 7838's
        "Alternative Service Field Value".

        .. note:: It is strongly preferred to use the explicit method of
                  advertising Alternative Services. The implicit method of
                  advertising Alternative Services has a number of subtleties
                  and can lead to inconsistencies between the server and
                  client. h2 allows both mechanisms, but caution is
                  strongly advised.

        .. versionadded:: 2.3.0

        :param field_value: The RFC 7838 Alternative Service Field Value. This
            argument is not introspected by h2: the user is responsible
            for ensuring that it is well-formed.
        :type field_value: ``bytes``

        :param origin: The origin/authority to which the Alternative Service
            being advertised applies. Must not be provided at the same time as
            ``stream_id``.
        :type origin: ``bytes`` or ``None``

        :param stream_id: The ID of the stream which was sent to the authority
            for which this Alternative Service advertisement applies. Must not
            be provided at the same time as ``origin``.
        :type stream_id: ``int`` or ``None``

        :returns: Nothing.
        """
        if not isinstance(field_value, bytes):
            raise ValueError("Field must be bytestring.")

        if origin is not None and stream_id is not None:
            raise ValueError("Must not provide both origin and stream_id")

        self.state_machine.process_input(
            ConnectionInputs.SEND_ALTERNATIVE_SERVICE
        )

        if origin is not None:
            # This ALTSVC is sent on stream zero.
            f = AltSvcFrame(stream_id=0)
            f.origin = origin
            f.field = field_value
            frames = [f]
        else:
            stream = self._get_stream_by_id(stream_id)
            frames = stream.advertise_alternative_service(field_value)

        self._prepare_for_sending(frames)

    def prioritize(self, stream_id, weight=None, depends_on=None,
                   exclusive=None):
        """
        Notify a server about the priority of a stream.

        Stream priorities are a form of guidance to a remote server: they
        inform the server about how important a given response is, so that the
        server may allocate its resources (e.g. bandwidth, CPU time, etc.)
        accordingly. This exists to allow clients to ensure that the most
        important data arrives earlier, while less important data does not
        starve out the more important data.

        Stream priorities are explained in depth in `RFC 7540 Section 5.3
        <https://tools.ietf.org/html/rfc7540#section-5.3>`_.

        This method updates the priority information of a single stream. It may
        be called well before a stream is actively in use, or well after a
        stream is closed.

        .. warning:: RFC 7540 allows for servers to change the priority of
                     streams. However, h2 **does not** allow server
                     stacks to do this. This is because most clients do not
                     adequately know how to respond when provided conflicting
                     priority information, and relatively little utility is
                     provided by making that functionality available.

        .. note:: h2 **does not** maintain any information about the
                  RFC 7540 priority tree. That means that h2 does not
                  prevent incautious users from creating invalid priority
                  trees, particularly by creating priority loops. While some
                  basic error checking is provided by h2, users are
                  strongly recommended to understand their prioritisation
                  strategies before using the priority tools here.

        .. note:: Priority information is strictly advisory. Servers are
                  allowed to disregard it entirely. Avoid relying on the idea
                  that your priority signaling will definitely be obeyed.

        .. versionadded:: 2.4.0

        :param stream_id: The ID of the stream to prioritize.
        :type stream_id: ``int``

        :param weight: The weight to give the stream. Defaults to ``16``, the
             default weight of any stream. May be any value between ``1`` and
             ``256`` inclusive. The relative weight of a stream indicates what
             proportion of available resources will be allocated to that
             stream.
        :type weight: ``int``

        :param depends_on: The ID of the stream on which this stream depends.
             This stream will only be progressed if it is impossible to
             progress the parent stream (the one on which this one depends).
             Passing the value ``0`` means that this stream does not depend on
             any other. Defaults to ``0``.
        :type depends_on: ``int``

        :param exclusive: Whether this stream is an exclusive dependency of its
            "parent" stream (i.e. the stream given by ``depends_on``). If a
            stream is an exclusive dependency of another, that means that all
            previously-set children of the parent are moved to become children
            of the new exclusively-dependent stream. Defaults to ``False``.
        :type exclusive: ``bool``
        """
        if not self.config.client_side:
            raise RFC1122Error("Servers SHOULD NOT prioritize streams.")

        self.state_machine.process_input(
            ConnectionInputs.SEND_PRIORITY
        )

        frame = PriorityFrame(stream_id)
        frame = _add_frame_priority(frame, weight, depends_on, exclusive)

        self._prepare_for_sending([frame])

    def local_flow_control_window(self, stream_id):
        """
        Returns the maximum amount of data that can be sent on stream
        ``stream_id``.

        This value will never be larger than the total data that can be sent on
        the connection: even if the given stream allows more data, the
        connection window provides a logical maximum to the amount of data that
        can be sent.

        The maximum data that can be sent in a single data frame on a stream
        is either this value, or the maximum frame size, whichever is
        *smaller*.

        :param stream_id: The ID of the stream whose flow control window is
            being queried.
        :type stream_id: ``int``
        :returns: The amount of data in bytes that can be sent on the stream
            before the flow control window is exhausted.
        :rtype: ``int``
        """
        stream = self._get_stream_by_id(stream_id)
        return min(
            self.outbound_flow_control_window,
            stream.outbound_flow_control_window
        )

    def remote_flow_control_window(self, stream_id):
        """
        Returns the maximum amount of data the remote peer can send on stream
        ``stream_id``.

        This value will never be larger than the total data that can be sent on
        the connection: even if the given stream allows more data, the
        connection window provides a logical maximum to the amount of data that
        can be sent.

        The maximum data that can be sent in a single data frame on a stream
        is either this value, or the maximum frame size, whichever is
        *smaller*.

        :param stream_id: The ID of the stream whose flow control window is
            being queried.
        :type stream_id: ``int``
        :returns: The amount of data in bytes that can be received on the
            stream before the flow control window is exhausted.
        :rtype: ``int``
        """
        stream = self._get_stream_by_id(stream_id)
        return min(
            self.inbound_flow_control_window,
            stream.inbound_flow_control_window
        )

    def acknowledge_received_data(self, acknowledged_size, stream_id):
        """
        Inform the :class:`H2Connection <h2.connection.H2Connection>` that a
        certain number of flow-controlled bytes have been processed, and that
        the space should be handed back to the remote peer at an opportune
        time.

        .. versionadded:: 2.5.0

        :param acknowledged_size: The total *flow-controlled size* of the data
            that has been processed. Note that this must include the amount of
            padding that was sent with that data.
        :type acknowledged_size: ``int``
        :param stream_id: The ID of the stream on which this data was received.
        :type stream_id: ``int``
        :returns: Nothing
        :rtype: ``None``
        """
        self.config.logger.debug(
            "Ack received data on stream ID %d with size %d",
            stream_id, acknowledged_size
        )
        if stream_id <= 0:
            raise ValueError(
                "Stream ID %d is not valid for acknowledge_received_data" %
                stream_id
            )
        if acknowledged_size < 0:
            raise ValueError("Cannot acknowledge negative data")

        frames = []

        conn_manager = self._inbound_flow_control_window_manager
        conn_increment = conn_manager.process_bytes(acknowledged_size)
        if conn_increment:
            f = WindowUpdateFrame(0)
            f.window_increment = conn_increment
            frames.append(f)

        try:
            stream = self._get_stream_by_id(stream_id)
        except StreamClosedError:
            # The stream is already gone. We're not worried about incrementing
            # the window in this case.
            pass
        else:
            # No point incrementing the windows of closed streams.
            if stream.open:
                frames.extend(
                    stream.acknowledge_received_data(acknowledged_size)
                )

        self._prepare_for_sending(frames)

    def data_to_send(self, amount=None):
        """
        Returns some data for sending out of the internal data buffer.

        This method is analogous to ``read`` on a file-like object, but it
        doesn't block. Instead, it returns as much data as the user asks for,
        or less if that much data is not available. It does not perform any
        I/O, and so uses a different name.

        :param amount: (optional) The maximum amount of data to return. If not
            set, or set to ``None``, will return as much data as possible.
        :type amount: ``int``
        :returns: A bytestring containing the data to send on the wire.
        :rtype: ``bytes``
        """
        if amount is None:
            data = bytes(self._data_to_send)
            self._data_to_send = bytearray()
            return data
        else:
            data = bytes(self._data_to_send[:amount])
            self._data_to_send = self._data_to_send[amount:]
            return data

    def clear_outbound_data_buffer(self):
        """
        Clears the outbound data buffer, such that if this call was immediately
        followed by a call to
        :meth:`data_to_send <h2.connection.H2Connection.data_to_send>`, that
        call would return no data.

        This method should not normally be used, but is made available to avoid
        exposing implementation details.
        """
        self._data_to_send = bytearray()

    def _acknowledge_settings(self):
        """
        Acknowledge settings that have been received.

        .. versionchanged:: 2.0.0
           Removed from public API, removed useless ``event`` parameter, made
           automatic.

        :returns: Nothing
        """
        self.state_machine.process_input(ConnectionInputs.SEND_SETTINGS)

        changes = self.remote_settings.acknowledge()

        if SettingCodes.INITIAL_WINDOW_SIZE in changes:
            setting = changes[SettingCodes.INITIAL_WINDOW_SIZE]
            self._flow_control_change_from_settings(
                setting.original_value,
                setting.new_value,
            )

        # HEADER_TABLE_SIZE changes by the remote part affect our encoder: cf.
        # RFC 7540 Section 6.5.2.
        if SettingCodes.HEADER_TABLE_SIZE in changes:
            setting = changes[SettingCodes.HEADER_TABLE_SIZE]
            self.encoder.header_table_size = setting.new_value

        if SettingCodes.MAX_FRAME_SIZE in changes:
            setting = changes[SettingCodes.MAX_FRAME_SIZE]
            self.max_outbound_frame_size = setting.new_value
            for stream in self.streams.values():
                stream.max_outbound_frame_size = setting.new_value

        f = SettingsFrame(0)
        f.flags.add('ACK')
        return [f]

    def _flow_control_change_from_settings(self, old_value, new_value):
        """
        Update flow control windows in response to a change in the value of
        SETTINGS_INITIAL_WINDOW_SIZE.

        When this setting is changed, it automatically updates all flow control
        windows by the delta in the settings values. Note that it does not
        increment the *connection* flow control window, per section 6.9.2 of
        RFC 7540.
        """
        delta = new_value - old_value

        for stream in self.streams.values():
            stream.outbound_flow_control_window = guard_increment_window(
                stream.outbound_flow_control_window,
                delta
            )

    def _inbound_flow_control_change_from_settings(self, old_value, new_value):
        """
        Update remote flow control windows in response to a change in the value
        of SETTINGS_INITIAL_WINDOW_SIZE.

        When this setting is changed, it automatically updates all remote flow
        control windows by the delta in the settings values.
        """
        delta = new_value - old_value

        for stream in self.streams.values():
            stream._inbound_flow_control_change_from_settings(delta)

    def receive_data(self, data):
        """
        Pass some received HTTP/2 data to the connection for handling.

        :param data: The data received from the remote peer on the network.
        :type data: ``bytes``
        :returns: A list of events that the remote peer triggered by sending
            this data.
        """
        self.config.logger.trace(
            "Process received data on connection. Received data: %r", data
        )

        events = []
        self.incoming_buffer.add_data(data)
        self.incoming_buffer.max_frame_size = self.max_inbound_frame_size

        try:
            for frame in self.incoming_buffer:
                events.extend(self._receive_frame(frame))
        except InvalidPaddingError:
            self._terminate_connection(ErrorCodes.PROTOCOL_ERROR)
            raise ProtocolError("Received frame with invalid padding.")
        except ProtocolError as e:
            # For whatever reason, receiving the frame caused a protocol error.
            # We should prepare to emit a GoAway frame before throwing the
            # exception up further. No need for an event: the exception will
            # do fine.
            self._terminate_connection(e.error_code)
            raise

        return events

    def _receive_frame(self, frame):
        """
        Handle a frame received on the connection.

        .. versionchanged:: 2.0.0
           Removed from the public API.
        """
        self.config.logger.trace("Received frame: %s", repr(frame))
        try:
            # I don't love using __class__ here, maybe reconsider it.
            frames, events = self._frame_dispatch_table[frame.__class__](frame)
        except StreamClosedError as e:
            # If the stream was closed by RST_STREAM, we just send a RST_STREAM
            # to the remote peer. Otherwise, this is a connection error, and so
            # we will re-raise to trigger one.
            if self._stream_is_closed_by_reset(e.stream_id):
                f = RstStreamFrame(e.stream_id)
                f.error_code = e.error_code
                self._prepare_for_sending([f])
                events = e._events
            else:
                raise
        except StreamIDTooLowError as e:
            # The stream ID seems invalid. This may happen when the closed
            # stream has been cleaned up, or when the remote peer has opened a
            # new stream with a higher stream ID than this one, forcing it
            # closed implicitly.
            #
            # Check how the stream was closed: depending on the mechanism, it
            # is either a stream error or a connection error.
            if self._stream_is_closed_by_reset(e.stream_id):
                # Closed by RST_STREAM is a stream error.
                f = RstStreamFrame(e.stream_id)
                f.error_code = ErrorCodes.STREAM_CLOSED
                self._prepare_for_sending([f])
                events = []
            elif self._stream_is_closed_by_end(e.stream_id):
                # Closed by END_STREAM is a connection error.
                raise StreamClosedError(e.stream_id)
            else:
                # Closed implicitly, also a connection error, but of type
                # PROTOCOL_ERROR.
                raise
        else:
            self._prepare_for_sending(frames)

        return events

    def _terminate_connection(self, error_code):
        """
        Terminate the connection early. Used in error handling blocks to send
        GOAWAY frames.
        """
        f = GoAwayFrame(0)
        f.last_stream_id = self.highest_inbound_stream_id
        f.error_code = error_code
        self.state_machine.process_input(ConnectionInputs.SEND_GOAWAY)
        self._prepare_for_sending([f])

    def _receive_headers_frame(self, frame):
        """
        Receive a headers frame on the connection.
        """
        # If necessary, check we can open the stream. Also validate that the
        # stream ID is valid.
        if frame.stream_id not in self.streams:
            max_open_streams = self.local_settings.max_concurrent_streams
            if (self.open_inbound_streams + 1) > max_open_streams:
                raise TooManyStreamsError(
                    "Max outbound streams is %d, %d open" %
                    (max_open_streams, self.open_outbound_streams)
                )

        # Let's decode the headers. We handle headers as bytes internally up
        # until we hang them off the event, at which point we may optionally
        # convert them to unicode.
        headers = _decode_headers(self.decoder, frame.data)

        events = self.state_machine.process_input(
            ConnectionInputs.RECV_HEADERS
        )
        stream = self._get_or_create_stream(
            frame.stream_id, AllowedStreamIDs(not self.config.client_side)
        )
        frames, stream_events = stream.receive_headers(
            headers,
            'END_STREAM' in frame.flags,
            self.config.header_encoding
        )

        if 'PRIORITY' in frame.flags:
            p_frames, p_events = self._receive_priority_frame(frame)
            stream_events[0].priority_updated = p_events[0]
            stream_events.extend(p_events)
            assert not p_frames

        return frames, events + stream_events

    def _receive_push_promise_frame(self, frame):
        """
        Receive a push-promise frame on the connection.
        """
        if not self.local_settings.enable_push:
            raise ProtocolError("Received pushed stream")

        pushed_headers = _decode_headers(self.decoder, frame.data)

        events = self.state_machine.process_input(
            ConnectionInputs.RECV_PUSH_PROMISE
        )

        try:
            stream = self._get_stream_by_id(frame.stream_id)
        except NoSuchStreamError:
            # We need to check if the parent stream was reset by us. If it was
            # then we presume that the PUSH_PROMISE was in flight when we reset
            # the parent stream. Rather than accept the new stream, just reset
            # it.
            #
            # If this was closed naturally, however, we should call this a
            # PROTOCOL_ERROR: pushing a stream on a naturally closed stream is
            # a real problem because it creates a brand new stream that the
            # remote peer now believes exists.
            if (self._stream_closed_by(frame.stream_id) ==
                    StreamClosedBy.SEND_RST_STREAM):
                f = RstStreamFrame(frame.promised_stream_id)
                f.error_code = ErrorCodes.REFUSED_STREAM
                return [f], events

            raise ProtocolError("Attempted to push on closed stream.")

        # We need to prevent peers pushing streams in response to streams that
        # they themselves have already pushed: see #163 and RFC 7540 § 6.6. The
        # easiest way to do that is to assert that the stream_id is not even:
        # this shortcut works because only servers can push and the state
        # machine will enforce this.
        if (frame.stream_id % 2) == 0:
            raise ProtocolError("Cannot recursively push streams.")

        try:
            frames, stream_events = stream.receive_push_promise_in_band(
                frame.promised_stream_id,
                pushed_headers,
                self.config.header_encoding,
            )
        except StreamClosedError:
            # The parent stream was reset by us, so we presume that
            # PUSH_PROMISE was in flight when we reset the parent stream.
            # So we just reset the new stream.
            f = RstStreamFrame(frame.promised_stream_id)
            f.error_code = ErrorCodes.REFUSED_STREAM
            return [f], events

        new_stream = self._begin_new_stream(
            frame.promised_stream_id, AllowedStreamIDs.EVEN
        )
        self.streams[frame.promised_stream_id] = new_stream
        new_stream.remotely_pushed(pushed_headers)

        return frames, events + stream_events

    def _handle_data_on_closed_stream(self, events, exc, frame):
        # This stream is already closed - and yet we received a DATA frame.
        # The received DATA frame counts towards the connection flow window.
        # We need to manually to acknowledge the DATA frame to update the flow
        # window of the connection. Otherwise the whole connection stalls due
        # the inbound flow window being 0.
        frames = []
        conn_manager = self._inbound_flow_control_window_manager
        conn_increment = conn_manager.process_bytes(
            frame.flow_controlled_length
        )
        if conn_increment:
            f = WindowUpdateFrame(0)
            f.window_increment = conn_increment
            frames.append(f)
            self.config.logger.debug(
                "Received DATA frame on closed stream %d - "
                "auto-emitted a WINDOW_UPDATE by %d",
                frame.stream_id, conn_increment
            )
        f = RstStreamFrame(exc.stream_id)
        f.error_code = exc.error_code
        frames.append(f)
        self.config.logger.debug(
            "Stream %d already CLOSED or cleaned up - "
            "auto-emitted a RST_FRAME" % frame.stream_id
        )
        return frames, events + exc._events

    def _receive_data_frame(self, frame):
        """
        Receive a data frame on the connection.
        """
        flow_controlled_length = frame.flow_controlled_length

        events = self.state_machine.process_input(
            ConnectionInputs.RECV_DATA
        )
        self._inbound_flow_control_window_manager.window_consumed(
            flow_controlled_length
        )

        try:
            stream = self._get_stream_by_id(frame.stream_id)
            frames, stream_events = stream.receive_data(
                frame.data,
                'END_STREAM' in frame.flags,
                flow_controlled_length
            )
        except StreamClosedError as e:
            # This stream is either marked as CLOSED or already gone from our
            # internal state.
            return self._handle_data_on_closed_stream(events, e, frame)

        return frames, events + stream_events

    def _receive_settings_frame(self, frame):
        """
        Receive a SETTINGS frame on the connection.
        """
        events = self.state_machine.process_input(
            ConnectionInputs.RECV_SETTINGS
        )

        # This is an ack of the local settings.
        if 'ACK' in frame.flags:
            changed_settings = self._local_settings_acked()
            ack_event = SettingsAcknowledged()
            ack_event.changed_settings = changed_settings
            events.append(ack_event)
            return [], events

        # Add the new settings.
        self.remote_settings.update(frame.settings)
        events.append(
            RemoteSettingsChanged.from_settings(
                self.remote_settings, frame.settings
            )
        )
        frames = self._acknowledge_settings()

        return frames, events

    def _receive_window_update_frame(self, frame):
        """
        Receive a WINDOW_UPDATE frame on the connection.
        """
        # hyperframe will take care of validating the window_increment.
        # If we reach in here, we can assume a valid value.

        events = self.state_machine.process_input(
            ConnectionInputs.RECV_WINDOW_UPDATE
        )

        if frame.stream_id:
            try:
                stream = self._get_stream_by_id(frame.stream_id)
                frames, stream_events = stream.receive_window_update(
                    frame.window_increment
                )
            except StreamClosedError:
                return [], events
        else:
            # Increment our local flow control window.
            self.outbound_flow_control_window = guard_increment_window(
                self.outbound_flow_control_window,
                frame.window_increment
            )

            # FIXME: Should we split this into one event per active stream?
            window_updated_event = WindowUpdated()
            window_updated_event.stream_id = 0
            window_updated_event.delta = frame.window_increment
            stream_events = [window_updated_event]
            frames = []

        return frames, events + stream_events

    def _receive_ping_frame(self, frame):
        """
        Receive a PING frame on the connection.
        """
        events = self.state_machine.process_input(
            ConnectionInputs.RECV_PING
        )
        flags = []

        if 'ACK' in frame.flags:
            evt = PingAckReceived()
        else:
            evt = PingReceived()

            # automatically ACK the PING with the same 'opaque data'
            f = PingFrame(0)
            f.flags = {'ACK'}
            f.opaque_data = frame.opaque_data
            flags.append(f)

        evt.ping_data = frame.opaque_data
        events.append(evt)

        return flags, events

    def _receive_rst_stream_frame(self, frame):
        """
        Receive a RST_STREAM frame on the connection.
        """
        events = self.state_machine.process_input(
            ConnectionInputs.RECV_RST_STREAM
        )
        try:
            stream = self._get_stream_by_id(frame.stream_id)
        except NoSuchStreamError:
            # The stream is missing. That's ok, we just do nothing here.
            stream_frames = []
            stream_events = []
        else:
            stream_frames, stream_events = stream.stream_reset(frame)

        return stream_frames, events + stream_events

    def _receive_priority_frame(self, frame):
        """
        Receive a PRIORITY frame on the connection.
        """
        events = self.state_machine.process_input(
            ConnectionInputs.RECV_PRIORITY
        )

        event = PriorityUpdated()
        event.stream_id = frame.stream_id
        event.depends_on = frame.depends_on
        event.exclusive = frame.exclusive

        # Weight is an integer between 1 and 256, but the byte only allows
        # 0 to 255: add one.
        event.weight = frame.stream_weight + 1

        # A stream may not depend on itself.
        if event.depends_on == frame.stream_id:
            raise ProtocolError(
                "Stream %d may not depend on itself" % frame.stream_id
            )
        events.append(event)

        return [], events

    def _receive_goaway_frame(self, frame):
        """
        Receive a GOAWAY frame on the connection.
        """
        events = self.state_machine.process_input(
            ConnectionInputs.RECV_GOAWAY
        )

        # Clear the outbound data buffer: we cannot send further data now.
        self.clear_outbound_data_buffer()

        # Fire an appropriate ConnectionTerminated event.
        new_event = ConnectionTerminated()
        new_event.error_code = _error_code_from_int(frame.error_code)
        new_event.last_stream_id = frame.last_stream_id
        new_event.additional_data = (frame.additional_data
                                     if frame.additional_data else None)
        events.append(new_event)

        return [], events

    def _receive_naked_continuation(self, frame):
        """
        A naked CONTINUATION frame has been received. This is always an error,
        but the type of error it is depends on the state of the stream and must
        transition the state of the stream, so we need to pass it to the
        appropriate stream.
        """
        stream = self._get_stream_by_id(frame.stream_id)
        stream.receive_continuation()
        assert False, "Should not be reachable"

    def _receive_alt_svc_frame(self, frame):
        """
        An ALTSVC frame has been received. This frame, specified in RFC 7838,
        is used to advertise alternative places where the same service can be
        reached.

        This frame can optionally be received either on a stream or on stream
        0, and its semantics are different in each case.
        """
        events = self.state_machine.process_input(
            ConnectionInputs.RECV_ALTERNATIVE_SERVICE
        )
        frames = []

        if frame.stream_id:
            # Given that it makes no sense to receive ALTSVC on a stream
            # before that stream has been opened with a HEADERS frame, the
            # ALTSVC frame cannot create a stream. If the stream is not
            # present, we simply ignore the frame.
            try:
                stream = self._get_stream_by_id(frame.stream_id)
            except (NoSuchStreamError, StreamClosedError):
                pass
            else:
                stream_frames, stream_events = stream.receive_alt_svc(frame)
                frames.extend(stream_frames)
                events.extend(stream_events)
        else:
            # This frame is sent on stream 0. The origin field on the frame
            # must be present, though if it isn't it's not a ProtocolError
            # (annoyingly), we just need to ignore it.
            if not frame.origin:
                return frames, events

            # If we're a server, we want to ignore this (RFC 7838 says so).
            if not self.config.client_side:
                return frames, events

            event = AlternativeServiceAvailable()
            event.origin = frame.origin
            event.field_value = frame.field
            events.append(event)

        return frames, events

    def _receive_unknown_frame(self, frame):
        """
        We have received a frame that we do not understand. This is almost
        certainly an extension frame, though it's impossible to be entirely
        sure.

        RFC 7540 § 5.5 says that we MUST ignore unknown frame types: so we
        do. We do notify the user that we received one, however.
        """
        # All we do here is log.
        self.config.logger.debug(
            "Received unknown extension frame (ID %d)", frame.stream_id
        )
        event = UnknownFrameReceived()
        event.frame = frame
        return [], [event]

    def _local_settings_acked(self):
        """
        Handle the local settings being ACKed, update internal state.
        """
        changes = self.local_settings.acknowledge()

        if SettingCodes.INITIAL_WINDOW_SIZE in changes:
            setting = changes[SettingCodes.INITIAL_WINDOW_SIZE]
            self._inbound_flow_control_change_from_settings(
                setting.original_value,
                setting.new_value,
            )

        if SettingCodes.MAX_HEADER_LIST_SIZE in changes:
            setting = changes[SettingCodes.MAX_HEADER_LIST_SIZE]
            self.decoder.max_header_list_size = setting.new_value

        if SettingCodes.MAX_FRAME_SIZE in changes:
            setting = changes[SettingCodes.MAX_FRAME_SIZE]
            self.max_inbound_frame_size = setting.new_value

        if SettingCodes.HEADER_TABLE_SIZE in changes:
            setting = changes[SettingCodes.HEADER_TABLE_SIZE]
            # This is safe across all hpack versions: some versions just won't
            # respect it.
            self.decoder.max_allowed_table_size = setting.new_value

        return changes

    def _stream_id_is_outbound(self, stream_id):
        """
        Returns ``True`` if the stream ID corresponds to an outbound stream
        (one initiated by this peer), returns ``False`` otherwise.
        """
        return (stream_id % 2 == int(self.config.client_side))

    def _stream_closed_by(self, stream_id):
        """
        Returns how the stream was closed.

        The return value will be either a member of
        ``h2.stream.StreamClosedBy`` or ``None``. If ``None``, the stream was
        closed implicitly by the peer opening a stream with a higher stream ID
        before opening this one.
        """
        if stream_id in self.streams:
            return self.streams[stream_id].closed_by
        if stream_id in self._closed_streams:
            return self._closed_streams[stream_id]
        return None

    def _stream_is_closed_by_reset(self, stream_id):
        """
        Returns ``True`` if the stream was closed by sending or receiving a
        RST_STREAM frame. Returns ``False`` otherwise.
        """
        return self._stream_closed_by(stream_id) in (
            StreamClosedBy.RECV_RST_STREAM, StreamClosedBy.SEND_RST_STREAM
        )

    def _stream_is_closed_by_end(self, stream_id):
        """
        Returns ``True`` if the stream was closed by sending or receiving an
        END_STREAM flag in a HEADERS or DATA frame. Returns ``False``
        otherwise.
        """
        return self._stream_closed_by(stream_id) in (
            StreamClosedBy.RECV_END_STREAM, StreamClosedBy.SEND_END_STREAM
        )


def _add_frame_priority(frame, weight=None, depends_on=None, exclusive=None):
    """
    Adds priority data to a given frame. Does not change any flags set on that
    frame: if the caller is adding priority information to a HEADERS frame they
    must set that themselves.

    This method also deliberately sets defaults for anything missing.

    This method validates the input values.
    """
    # A stream may not depend on itself.
    if depends_on == frame.stream_id:
        raise ProtocolError(
            "Stream %d may not depend on itself" % frame.stream_id
        )

    # Weight must be between 1 and 256.
    if weight is not None:
        if weight > 256 or weight < 1:
            raise ProtocolError(
                "Weight must be between 1 and 256, not %d" % weight
            )
        else:
            # Weight is an integer between 1 and 256, but the byte only allows
            # 0 to 255: subtract one.
            weight -= 1

    # Set defaults for anything not provided.
    weight = weight if weight is not None else 15
    depends_on = depends_on if depends_on is not None else 0
    exclusive = exclusive if exclusive is not None else False

    frame.stream_weight = weight
    frame.depends_on = depends_on
    frame.exclusive = exclusive

    return frame


def _decode_headers(decoder, encoded_header_block):
    """
    Decode a HPACK-encoded header block, translating HPACK exceptions into
    sensible h2 errors.

    This only ever returns bytestring headers: h2 may emit them as
    unicode later, but internally it processes them as bytestrings only.
    """
    try:
        return decoder.decode(encoded_header_block, raw=True)
    except OversizedHeaderListError as e:
        # This is a symptom of a HPACK bomb attack: the user has
        # disregarded our requirements on how large a header block we'll
        # accept.
        raise DenialOfServiceError("Oversized header block: %s" % e)
    except (HPACKError, IndexError, TypeError, UnicodeDecodeError) as e:
        # We should only need HPACKError here, but versions of HPACK older
        # than 2.1.0 throw all three others as well. For maximum
        # compatibility, catch all of them.
        raise ProtocolError("Error decoding header block: %s" % e)
