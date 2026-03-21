# -*- coding: utf-8 -*-
"""
hyperframe/frame
~~~~~~~~~~~~~~~~

Defines framing logic for HTTP/2. Provides both classes to represent framed
data and logic for aiding the connection when it comes to reading from the
socket.
"""
import struct
import binascii

from .exceptions import (
    UnknownFrameError, InvalidPaddingError, InvalidFrameError, InvalidDataError
)
from .flags import Flag, Flags
from typing import Optional, Tuple, List, Iterable, Any, Dict, Type


# The maximum initial length of a frame. Some frames have shorter maximum
# lengths.
FRAME_MAX_LEN = (2 ** 14)

# The maximum allowed length of a frame.
FRAME_MAX_ALLOWED_LEN = (2 ** 24) - 1

# Stream association enumerations.
_STREAM_ASSOC_HAS_STREAM = "has-stream"
_STREAM_ASSOC_NO_STREAM = "no-stream"
_STREAM_ASSOC_EITHER = "either"

# Structs for packing and unpacking
_STRUCT_HBBBL = struct.Struct(">HBBBL")
_STRUCT_LL = struct.Struct(">LL")
_STRUCT_HL = struct.Struct(">HL")
_STRUCT_LB = struct.Struct(">LB")
_STRUCT_L = struct.Struct(">L")
_STRUCT_H = struct.Struct(">H")
_STRUCT_B = struct.Struct(">B")


class Frame:
    """
    The base class for all HTTP/2 frames.
    """
    #: The flags defined on this type of frame.
    defined_flags: List[Flag] = []

    #: The byte used to define the type of the frame.
    type: Optional[int] = None

    # If 'has-stream', the frame's stream_id must be non-zero. If 'no-stream',
    # it must be zero. If 'either', it's not checked.
    stream_association: Optional[str] = None

    def __init__(self, stream_id: int, flags: Iterable[str] = ()) -> None:
        #: The stream identifier for the stream this frame was received on.
        #: Set to 0 for frames sent on the connection (stream-id 0).
        self.stream_id = stream_id

        #: The flags set for this frame.
        self.flags = Flags(self.defined_flags)

        #: The frame length, excluding the nine-byte header.
        self.body_len = 0

        for flag in flags:
            self.flags.add(flag)

        if (not self.stream_id and
           self.stream_association == _STREAM_ASSOC_HAS_STREAM):
            raise InvalidDataError(
                'Stream ID must be non-zero for {}'.format(
                    type(self).__name__,
                )
            )
        if (self.stream_id and
           self.stream_association == _STREAM_ASSOC_NO_STREAM):
            raise InvalidDataError(
                'Stream ID must be zero for {} with stream_id={}'.format(
                    type(self).__name__,
                    self.stream_id,
                )
            )

    def __repr__(self) -> str:
        return (
            "{}(stream_id={}, flags={}): {}"
        ).format(
            type(self).__name__,
            self.stream_id,
            repr(self.flags),
            self._body_repr(),
        )

    def _body_repr(self) -> str:
        # More specific implementation may be provided by subclasses of Frame.
        # This fallback shows the serialized (and truncated) body content.
        return _raw_data_repr(self.serialize_body())

    @staticmethod
    def explain(data: memoryview) -> Tuple["Frame", int]:
        """
        Takes a bytestring and tries to parse a single frame and print it.

        This function is only provided for debugging purposes.

        :param data: A memoryview object containing the raw data of at least
                     one complete frame (header and body).

        .. versionadded:: 6.0.0
        """
        frame, length = Frame.parse_frame_header(data[:9])
        frame.parse_body(data[9:9 + length])
        print(frame)
        return frame, length

    @staticmethod
    def parse_frame_header(header: memoryview, strict: bool = False) -> Tuple["Frame", int]:
        """
        Takes a 9-byte frame header and returns a tuple of the appropriate
        Frame object and the length that needs to be read from the socket.

        This populates the flags field, and determines how long the body is.

        :param header: A memoryview object containing the 9-byte frame header
                       data of a frame. Must not contain more or less.

        :param strict: Whether to raise an exception when encountering a frame
            not defined by spec and implemented by hyperframe.

        :raises hyperframe.exceptions.UnknownFrameError: If a frame of unknown
            type is received.

        .. versionchanged:: 5.0.0
            Added :param:`strict` to accommodate :class:`ExtensionFrame`
        """
        try:
            fields = _STRUCT_HBBBL.unpack(header)
        except struct.error:
            raise InvalidFrameError("Invalid frame header")

        # First 24 bits are frame length.
        length = (fields[0] << 8) + fields[1]
        type = fields[2]
        flags = fields[3]
        stream_id = fields[4] & 0x7FFFFFFF

        try:
            frame = FRAMES[type](stream_id)
        except KeyError:
            if strict:
                raise UnknownFrameError(type, length)
            frame = ExtensionFrame(type=type, stream_id=stream_id)

        frame.parse_flags(flags)
        return (frame, length)

    def parse_flags(self, flag_byte: int) -> Flags:
        for flag, flag_bit in self.defined_flags:
            if flag_byte & flag_bit:
                self.flags.add(flag)

        return self.flags

    def serialize(self) -> bytes:
        """
        Convert a frame into a bytestring, representing the serialized form of
        the frame.
        """
        body = self.serialize_body()
        self.body_len = len(body)

        # Build the common frame header.
        # First, get the flags.
        flags = 0

        for flag, flag_bit in self.defined_flags:
            if flag in self.flags:
                flags |= flag_bit

        header = _STRUCT_HBBBL.pack(
            (self.body_len >> 8) & 0xFFFF,  # Length spread over top 24 bits
            self.body_len & 0xFF,
            self.type,
            flags,
            self.stream_id & 0x7FFFFFFF  # Stream ID is 32 bits.
        )

        return header + body

    def serialize_body(self) -> bytes:
        raise NotImplementedError()

    def parse_body(self, data: memoryview) -> None:
        """
        Given the body of a frame, parses it into frame data. This populates
        the non-header parts of the frame: that is, it does not populate the
        stream ID or flags.

        :param data: A memoryview object containing the body data of the frame.
                     Must not contain *more* data than the length returned by
                     :meth:`parse_frame_header
                     <hyperframe.frame.Frame.parse_frame_header>`.
        """
        raise NotImplementedError()


class Padding:
    """
    Mixin for frames that contain padding. Defines extra fields that can be
    used and set by frames that can be padded.
    """
    def __init__(self, stream_id: int, pad_length: int = 0, **kwargs: Any) -> None:
        super().__init__(stream_id, **kwargs)  # type: ignore

        #: The length of the padding to use.
        self.pad_length = pad_length

    def serialize_padding_data(self) -> bytes:
        if 'PADDED' in self.flags:  # type: ignore
            return _STRUCT_B.pack(self.pad_length)
        return b''

    def parse_padding_data(self, data: memoryview) -> int:
        if 'PADDED' in self.flags:  # type: ignore
            try:
                self.pad_length = struct.unpack('!B', data[:1])[0]
            except struct.error:
                raise InvalidFrameError("Invalid Padding data")
            return 1
        return 0

    #: .. deprecated:: 5.2.1
    #:    Use self.pad_length instead.
    @property
    def total_padding(self) -> int:  # pragma: no cover
        import warnings
        warnings.warn(
            "total_padding contains the same information as pad_length.",
            DeprecationWarning
        )
        return self.pad_length


class Priority:
    """
    Mixin for frames that contain priority data. Defines extra fields that can
    be used and set by frames that contain priority data.
    """
    def __init__(self,
                 stream_id: int,
                 depends_on: int = 0x0,
                 stream_weight: int = 0x0,
                 exclusive: bool = False,
                 **kwargs: Any) -> None:
        super().__init__(stream_id, **kwargs)  # type: ignore

        #: The stream ID of the stream on which this stream depends.
        self.depends_on = depends_on

        #: The weight of the stream. This is an integer between 0 and 256.
        self.stream_weight = stream_weight

        #: Whether the exclusive bit was set.
        self.exclusive = exclusive

    def serialize_priority_data(self) -> bytes:
        return _STRUCT_LB.pack(
            self.depends_on + (0x80000000 if self.exclusive else 0),
            self.stream_weight
        )

    def parse_priority_data(self, data: memoryview) -> int:
        try:
            self.depends_on, self.stream_weight = _STRUCT_LB.unpack(data[:5])
        except struct.error:
            raise InvalidFrameError("Invalid Priority data")

        self.exclusive = True if self.depends_on >> 31 else False
        self.depends_on &= 0x7FFFFFFF
        return 5


class DataFrame(Padding, Frame):
    """
    DATA frames convey arbitrary, variable-length sequences of octets
    associated with a stream. One or more DATA frames are used, for instance,
    to carry HTTP request or response payloads.
    """
    #: The flags defined for DATA frames.
    defined_flags = [
        Flag('END_STREAM', 0x01),
        Flag('PADDED', 0x08),
    ]

    #: The type byte for data frames.
    type = 0x0

    stream_association = _STREAM_ASSOC_HAS_STREAM

    def __init__(self, stream_id: int, data: bytes = b'', **kwargs: Any) -> None:
        super().__init__(stream_id, **kwargs)

        #: The data contained on this frame.
        self.data = data

    def serialize_body(self) -> bytes:
        padding_data = self.serialize_padding_data()
        padding = b'\0' * self.pad_length
        if isinstance(self.data, memoryview):
            self.data = self.data.tobytes()
        return b''.join([padding_data, self.data, padding])

    def parse_body(self, data: memoryview) -> None:
        padding_data_length = self.parse_padding_data(data)
        self.data = (
            data[padding_data_length:len(data)-self.pad_length].tobytes()
        )
        self.body_len = len(data)

        if self.pad_length and self.pad_length >= self.body_len:
            raise InvalidPaddingError("Padding is too long.")

    @property
    def flow_controlled_length(self) -> int:
        """
        The length of the frame that needs to be accounted for when considering
        flow control.
        """
        padding_len = 0
        if 'PADDED' in self.flags:
            # Account for extra 1-byte padding length field, which is still
            # present if possibly zero-valued.
            padding_len = self.pad_length + 1
        return len(self.data) + padding_len


class PriorityFrame(Priority, Frame):
    """
    The PRIORITY frame specifies the sender-advised priority of a stream. It
    can be sent at any time for an existing stream. This enables
    reprioritisation of existing streams.
    """
    #: The flags defined for PRIORITY frames.
    defined_flags: List[Flag] = []

    #: The type byte defined for PRIORITY frames.
    type = 0x02

    stream_association = _STREAM_ASSOC_HAS_STREAM

    def _body_repr(self) -> str:
        return "exclusive={}, depends_on={}, stream_weight={}".format(
            self.exclusive,
            self.depends_on,
            self.stream_weight
        )

    def serialize_body(self) -> bytes:
        return self.serialize_priority_data()

    def parse_body(self, data: memoryview) -> None:
        if len(data) > 5:
            raise InvalidFrameError(
                "PRIORITY must have 5 byte body: actual length %s." %
                len(data)
            )

        self.parse_priority_data(data)
        self.body_len = 5


class RstStreamFrame(Frame):
    """
    The RST_STREAM frame allows for abnormal termination of a stream. When sent
    by the initiator of a stream, it indicates that they wish to cancel the
    stream or that an error condition has occurred. When sent by the receiver
    of a stream, it indicates that either the receiver is rejecting the stream,
    requesting that the stream be cancelled or that an error condition has
    occurred.
    """
    #: The flags defined for RST_STREAM frames.
    defined_flags: List[Flag] = []

    #: The type byte defined for RST_STREAM frames.
    type = 0x03

    stream_association = _STREAM_ASSOC_HAS_STREAM

    def __init__(self, stream_id: int, error_code: int = 0, **kwargs: Any) -> None:
        super().__init__(stream_id, **kwargs)

        #: The error code used when resetting the stream.
        self.error_code = error_code

    def _body_repr(self) -> str:
        return "error_code={}".format(
            self.error_code,
        )

    def serialize_body(self) -> bytes:
        return _STRUCT_L.pack(self.error_code)

    def parse_body(self, data: memoryview) -> None:
        if len(data) != 4:
            raise InvalidFrameError(
                "RST_STREAM must have 4 byte body: actual length %s." %
                len(data)
            )

        try:
            self.error_code = _STRUCT_L.unpack(data)[0]
        except struct.error:  # pragma: no cover
            raise InvalidFrameError("Invalid RST_STREAM body")

        self.body_len = 4


class SettingsFrame(Frame):
    """
    The SETTINGS frame conveys configuration parameters that affect how
    endpoints communicate. The parameters are either constraints on peer
    behavior or preferences.

    Settings are not negotiated. Settings describe characteristics of the
    sending peer, which are used by the receiving peer. Different values for
    the same setting can be advertised by each peer. For example, a client
    might set a high initial flow control window, whereas a server might set a
    lower value to conserve resources.
    """
    #: The flags defined for SETTINGS frames.
    defined_flags = [Flag('ACK', 0x01)]

    #: The type byte defined for SETTINGS frames.
    type = 0x04

    stream_association = _STREAM_ASSOC_NO_STREAM

    # We need to define the known settings, they may as well be class
    # attributes.
    #: The byte that signals the SETTINGS_HEADER_TABLE_SIZE setting.
    HEADER_TABLE_SIZE = 0x01
    #: The byte that signals the SETTINGS_ENABLE_PUSH setting.
    ENABLE_PUSH = 0x02
    #: The byte that signals the SETTINGS_MAX_CONCURRENT_STREAMS setting.
    MAX_CONCURRENT_STREAMS = 0x03
    #: The byte that signals the SETTINGS_INITIAL_WINDOW_SIZE setting.
    INITIAL_WINDOW_SIZE = 0x04
    #: The byte that signals the SETTINGS_MAX_FRAME_SIZE setting.
    MAX_FRAME_SIZE = 0x05
    #: The byte that signals the SETTINGS_MAX_HEADER_LIST_SIZE setting.
    MAX_HEADER_LIST_SIZE = 0x06
    #: The byte that signals SETTINGS_ENABLE_CONNECT_PROTOCOL setting.
    ENABLE_CONNECT_PROTOCOL = 0x08

    def __init__(self, stream_id: int = 0, settings: Optional[Dict[int, int]] = None, **kwargs: Any) -> None:
        super().__init__(stream_id, **kwargs)

        if settings and "ACK" in kwargs.get("flags", ()):
            raise InvalidDataError(
                "Settings must be empty if ACK flag is set."
            )

        #: A dictionary of the setting type byte to the value of the setting.
        self.settings = settings or {}

    def _body_repr(self) -> str:
        return "settings={}".format(
            self.settings,
        )

    def serialize_body(self) -> bytes:
        return b''.join([_STRUCT_HL.pack(setting & 0xFF, value)
                         for setting, value in self.settings.items()])

    def parse_body(self, data: memoryview) -> None:
        if 'ACK' in self.flags and len(data) > 0:
            raise InvalidDataError(
                "SETTINGS ack frame must not have payload: got %s bytes" %
                len(data)
            )

        body_len = 0
        for i in range(0, len(data), 6):
            try:
                name, value = _STRUCT_HL.unpack(data[i:i+6])
            except struct.error:
                raise InvalidFrameError("Invalid SETTINGS body")

            self.settings[name] = value
            body_len += 6

        self.body_len = body_len


class PushPromiseFrame(Padding, Frame):
    """
    The PUSH_PROMISE frame is used to notify the peer endpoint in advance of
    streams the sender intends to initiate.
    """
    #: The flags defined for PUSH_PROMISE frames.
    defined_flags = [
        Flag('END_HEADERS', 0x04),
        Flag('PADDED', 0x08)
    ]

    #: The type byte defined for PUSH_PROMISE frames.
    type = 0x05

    stream_association = _STREAM_ASSOC_HAS_STREAM

    def __init__(self, stream_id: int, promised_stream_id: int = 0, data: bytes = b'', **kwargs: Any) -> None:
        super().__init__(stream_id, **kwargs)

        #: The stream ID that is promised by this frame.
        self.promised_stream_id = promised_stream_id

        #: The HPACK-encoded header block for the simulated request on the new
        #: stream.
        self.data = data

    def _body_repr(self) -> str:
        return "promised_stream_id={}, data={}".format(
            self.promised_stream_id,
            _raw_data_repr(self.data),
        )

    def serialize_body(self) -> bytes:
        padding_data = self.serialize_padding_data()
        padding = b'\0' * self.pad_length
        data = _STRUCT_L.pack(self.promised_stream_id)
        return b''.join([padding_data, data, self.data, padding])

    def parse_body(self, data: memoryview) -> None:
        padding_data_length = self.parse_padding_data(data)

        try:
            self.promised_stream_id = _STRUCT_L.unpack(
                data[padding_data_length:padding_data_length + 4]
            )[0]
        except struct.error:
            raise InvalidFrameError("Invalid PUSH_PROMISE body")

        self.data = (
            data[padding_data_length + 4:len(data)-self.pad_length].tobytes()
        )
        self.body_len = len(data)

        if self.promised_stream_id == 0 or self.promised_stream_id % 2 != 0:
            raise InvalidDataError(
                "Invalid PUSH_PROMISE promised stream id: %s" %
                self.promised_stream_id
            )

        if self.pad_length and self.pad_length >= self.body_len:
            raise InvalidPaddingError("Padding is too long.")


class PingFrame(Frame):
    """
    The PING frame is a mechanism for measuring a minimal round-trip time from
    the sender, as well as determining whether an idle connection is still
    functional. PING frames can be sent from any endpoint.
    """
    #: The flags defined for PING frames.
    defined_flags = [Flag('ACK', 0x01)]

    #: The type byte defined for PING frames.
    type = 0x06

    stream_association = _STREAM_ASSOC_NO_STREAM

    def __init__(self, stream_id: int = 0, opaque_data: bytes = b'', **kwargs: Any) -> None:
        super().__init__(stream_id, **kwargs)

        #: The opaque data sent in this PING frame, as a bytestring.
        self.opaque_data = opaque_data

    def _body_repr(self) -> str:
        return "opaque_data={!r}".format(
            self.opaque_data,
        )

    def serialize_body(self) -> bytes:
        if len(self.opaque_data) > 8:
            raise InvalidFrameError(
                "PING frame may not have more than 8 bytes of data, got %r" %
                self.opaque_data
            )

        data = self.opaque_data
        data += b'\x00' * (8 - len(self.opaque_data))
        return data

    def parse_body(self, data: memoryview) -> None:
        if len(data) != 8:
            raise InvalidFrameError(
                "PING frame must have 8 byte length: got %s" % len(data)
            )

        self.opaque_data = data.tobytes()
        self.body_len = 8


class GoAwayFrame(Frame):
    """
    The GOAWAY frame informs the remote peer to stop creating streams on this
    connection. It can be sent from the client or the server. Once sent, the
    sender will ignore frames sent on new streams for the remainder of the
    connection.
    """
    #: The flags defined for GOAWAY frames.
    defined_flags: List[Flag] = []

    #: The type byte defined for GOAWAY frames.
    type = 0x07

    stream_association = _STREAM_ASSOC_NO_STREAM

    def __init__(self,
                 stream_id: int = 0,
                 last_stream_id: int = 0,
                 error_code: int = 0,
                 additional_data: bytes = b'',
                 **kwargs: Any) -> None:
        super().__init__(stream_id, **kwargs)

        #: The last stream ID definitely seen by the remote peer.
        self.last_stream_id = last_stream_id

        #: The error code for connection teardown.
        self.error_code = error_code

        #: Any additional data sent in the GOAWAY.
        self.additional_data = additional_data

    def _body_repr(self) -> str:
        return "last_stream_id={}, error_code={}, additional_data={!r}".format(
            self.last_stream_id,
            self.error_code,
            self.additional_data,
        )

    def serialize_body(self) -> bytes:
        data = _STRUCT_LL.pack(
            self.last_stream_id & 0x7FFFFFFF,
            self.error_code
        )
        data += self.additional_data

        return data

    def parse_body(self, data: memoryview) -> None:
        try:
            self.last_stream_id, self.error_code = _STRUCT_LL.unpack(
                data[:8]
            )
        except struct.error:
            raise InvalidFrameError("Invalid GOAWAY body.")

        self.body_len = len(data)

        if len(data) > 8:
            self.additional_data = data[8:].tobytes()


class WindowUpdateFrame(Frame):
    """
    The WINDOW_UPDATE frame is used to implement flow control.

    Flow control operates at two levels: on each individual stream and on the
    entire connection.

    Both types of flow control are hop by hop; that is, only between the two
    endpoints. Intermediaries do not forward WINDOW_UPDATE frames between
    dependent connections. However, throttling of data transfer by any receiver
    can indirectly cause the propagation of flow control information toward the
    original sender.
    """
    #: The flags defined for WINDOW_UPDATE frames.
    defined_flags: List[Flag] = []

    #: The type byte defined for WINDOW_UPDATE frames.
    type = 0x08

    stream_association = _STREAM_ASSOC_EITHER

    def __init__(self, stream_id: int, window_increment: int = 0, **kwargs: Any) -> None:
        super().__init__(stream_id, **kwargs)

        #: The amount the flow control window is to be incremented.
        self.window_increment = window_increment

    def _body_repr(self) -> str:
        return "window_increment={}".format(
            self.window_increment,
        )

    def serialize_body(self) -> bytes:
        return _STRUCT_L.pack(self.window_increment & 0x7FFFFFFF)

    def parse_body(self, data: memoryview) -> None:
        if len(data) > 4:
            raise InvalidFrameError(
                "WINDOW_UPDATE frame must have 4 byte length: got %s" %
                len(data)
            )

        try:
            self.window_increment = _STRUCT_L.unpack(data)[0]
        except struct.error:
            raise InvalidFrameError("Invalid WINDOW_UPDATE body")

        if not 1 <= self.window_increment <= 2**31-1:
            raise InvalidDataError(
                "WINDOW_UPDATE increment must be between 1 to 2^31-1"
            )

        self.body_len = 4


class HeadersFrame(Padding, Priority, Frame):
    """
    The HEADERS frame carries name-value pairs. It is used to open a stream.
    HEADERS frames can be sent on a stream in the "open" or "half closed
    (remote)" states.

    The HeadersFrame class is actually basically a data frame in this
    implementation, because of the requirement to control the sizes of frames.
    A header block fragment that doesn't fit in an entire HEADERS frame needs
    to be followed with CONTINUATION frames. From the perspective of the frame
    building code the header block is an opaque data segment.
    """
    #: The flags defined for HEADERS frames.
    defined_flags = [
        Flag('END_STREAM', 0x01),
        Flag('END_HEADERS', 0x04),
        Flag('PADDED', 0x08),
        Flag('PRIORITY', 0x20),
    ]

    #: The type byte defined for HEADERS frames.
    type = 0x01

    stream_association = _STREAM_ASSOC_HAS_STREAM

    def __init__(self, stream_id: int, data: bytes = b'', **kwargs: Any) -> None:
        super().__init__(stream_id, **kwargs)

        #: The HPACK-encoded header block.
        self.data = data

    def _body_repr(self) -> str:
        return "exclusive={}, depends_on={}, stream_weight={}, data={}".format(
            self.exclusive,
            self.depends_on,
            self.stream_weight,
            _raw_data_repr(self.data),
        )

    def serialize_body(self) -> bytes:
        padding_data = self.serialize_padding_data()
        padding = b'\0' * self.pad_length

        if 'PRIORITY' in self.flags:
            priority_data = self.serialize_priority_data()
        else:
            priority_data = b''

        return b''.join([padding_data, priority_data, self.data, padding])

    def parse_body(self, data: memoryview) -> None:
        padding_data_length = self.parse_padding_data(data)
        data = data[padding_data_length:]

        if 'PRIORITY' in self.flags:
            priority_data_length = self.parse_priority_data(data)
        else:
            priority_data_length = 0

        self.body_len = len(data)
        self.data = (
            data[priority_data_length:len(data)-self.pad_length].tobytes()
        )

        if self.pad_length and self.pad_length >= self.body_len:
            raise InvalidPaddingError("Padding is too long.")


class ContinuationFrame(Frame):
    """
    The CONTINUATION frame is used to continue a sequence of header block
    fragments. Any number of CONTINUATION frames can be sent on an existing
    stream, as long as the preceding frame on the same stream is one of
    HEADERS, PUSH_PROMISE or CONTINUATION without the END_HEADERS flag set.

    Much like the HEADERS frame, hyper treats this as an opaque data frame with
    different flags and a different type.
    """
    #: The flags defined for CONTINUATION frames.
    defined_flags = [Flag('END_HEADERS', 0x04)]

    #: The type byte defined for CONTINUATION frames.
    type = 0x09

    stream_association = _STREAM_ASSOC_HAS_STREAM

    def __init__(self, stream_id: int, data: bytes = b'', **kwargs: Any) -> None:
        super().__init__(stream_id, **kwargs)

        #: The HPACK-encoded header block.
        self.data = data

    def _body_repr(self) -> str:
        return "data={}".format(
            _raw_data_repr(self.data),
        )

    def serialize_body(self) -> bytes:
        return self.data

    def parse_body(self, data: memoryview) -> None:
        self.data = data.tobytes()
        self.body_len = len(data)


class AltSvcFrame(Frame):
    """
    The ALTSVC frame is used to advertise alternate services that the current
    host, or a different one, can understand. This frame is standardised as
    part of RFC 7838.

    This frame does no work to validate that the ALTSVC field parameter is
    acceptable per the rules of RFC 7838.

    .. note:: If the ``stream_id`` of this frame is nonzero, the origin field
              must have zero length. Conversely, if the ``stream_id`` of this
              frame is zero, the origin field must have nonzero length. Put
              another way, a valid ALTSVC frame has ``stream_id != 0`` XOR
              ``len(origin) != 0``.
    """
    type = 0xA

    stream_association = _STREAM_ASSOC_EITHER

    def __init__(self, stream_id: int, origin: bytes = b'', field: bytes = b'', **kwargs: Any) -> None:
        super().__init__(stream_id, **kwargs)

        if not isinstance(origin, bytes):
            raise InvalidDataError("AltSvc origin must be bytestring.")
        if not isinstance(field, bytes):
            raise InvalidDataError("AltSvc field must be a bytestring.")
        self.origin = origin
        self.field = field

    def _body_repr(self) -> str:
        return "origin={!r}, field={!r}".format(
            self.origin,
            self.field,
        )

    def serialize_body(self) -> bytes:
        origin_len = _STRUCT_H.pack(len(self.origin))
        return b''.join([origin_len, self.origin, self.field])

    def parse_body(self, data: memoryview) -> None:
        try:
            origin_len = _STRUCT_H.unpack(data[0:2])[0]
            self.origin = data[2:2+origin_len].tobytes()

            if len(self.origin) != origin_len:
                raise InvalidFrameError("Invalid ALTSVC frame body.")

            self.field = data[2+origin_len:].tobytes()
        except (struct.error, ValueError):
            raise InvalidFrameError("Invalid ALTSVC frame body.")

        self.body_len = len(data)


class ExtensionFrame(Frame):
    """
    ExtensionFrame is used to wrap frames which are not natively interpretable
    by hyperframe.

    Although certain byte prefixes are ordained by specification to have
    certain contextual meanings, frames with other prefixes are not prohibited,
    and may be used to communicate arbitrary meaning between HTTP/2 peers.

    Thus, hyperframe, rather than raising an exception when such a frame is
    encountered, wraps it in a generic frame to be properly acted upon by
    upstream consumers which might have additional context on how to use it.

    .. versionadded:: 5.0.0
    """

    stream_association = _STREAM_ASSOC_EITHER

    def __init__(self, type: int, stream_id: int, flag_byte: int = 0x0, body: bytes = b'', **kwargs: Any) -> None:
        super().__init__(stream_id, **kwargs)
        self.type = type
        self.flag_byte = flag_byte
        self.body = body

    def _body_repr(self) -> str:
        return "type={}, flag_byte={}, body={}".format(
            self.type,
            self.flag_byte,
            _raw_data_repr(self.body),
        )

    def parse_flags(self, flag_byte: int) -> None:  # type: ignore
        """
        For extension frames, we parse the flags by just storing a flag byte.
        """
        self.flag_byte = flag_byte

    def parse_body(self, data: memoryview) -> None:
        self.body = data.tobytes()
        self.body_len = len(data)

    def serialize(self) -> bytes:
        """
        A broad override of the serialize method that ensures that the data
        comes back out exactly as it came in. This should not be used in most
        user code: it exists only as a helper method if frames need to be
        reconstituted.
        """
        # Build the frame header.
        # First, get the flags.
        flags = self.flag_byte

        header = _STRUCT_HBBBL.pack(
            (self.body_len >> 8) & 0xFFFF,  # Length spread over top 24 bits
            self.body_len & 0xFF,
            self.type,
            flags,
            self.stream_id & 0x7FFFFFFF  # Stream ID is 32 bits.
        )

        return header + self.body


def _raw_data_repr(data: Optional[bytes]) -> str:
    if not data:
        return "None"
    r = binascii.hexlify(data).decode('ascii')
    if len(r) > 20:
        r = r[:20] + "..."
    return "<hex:" + r + ">"


_FRAME_CLASSES: List[Type[Frame]] = [
    DataFrame,
    HeadersFrame,
    PriorityFrame,
    RstStreamFrame,
    SettingsFrame,
    PushPromiseFrame,
    PingFrame,
    GoAwayFrame,
    WindowUpdateFrame,
    ContinuationFrame,
    AltSvcFrame,
]
#: FRAMES maps the type byte for each frame to the class used to represent that
#: frame.
FRAMES = {cls.type: cls for cls in _FRAME_CLASSES}
