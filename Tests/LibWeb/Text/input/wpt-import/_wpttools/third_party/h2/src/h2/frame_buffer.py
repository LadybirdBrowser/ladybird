# -*- coding: utf-8 -*-
"""
h2/frame_buffer
~~~~~~~~~~~~~~~

A data structure that provides a way to iterate over a byte buffer in terms of
frames.
"""
from hyperframe.exceptions import InvalidFrameError, InvalidDataError
from hyperframe.frame import (
    Frame, HeadersFrame, ContinuationFrame, PushPromiseFrame
)

from .exceptions import (
    ProtocolError, FrameTooLargeError, FrameDataMissingError
)

# To avoid a DOS attack based on sending loads of continuation frames, we limit
# the maximum number we're perpared to receive. In this case, we'll set the
# limit to 64, which means the largest encoded header block we can receive by
# default is 262144 bytes long, and the largest possible *at all* is 1073741760
# bytes long.
#
# This value seems reasonable for now, but in future we may want to evaluate
# making it configurable.
CONTINUATION_BACKLOG = 64


class FrameBuffer:
    """
    This is a data structure that expects to act as a buffer for HTTP/2 data
    that allows iteraton in terms of H2 frames.
    """
    def __init__(self, server=False):
        self.data = b''
        self.max_frame_size = 0
        self._preamble = b'PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n' if server else b''
        self._preamble_len = len(self._preamble)
        self._headers_buffer = []

    def add_data(self, data):
        """
        Add more data to the frame buffer.

        :param data: A bytestring containing the byte buffer.
        """
        if self._preamble_len:
            data_len = len(data)
            of_which_preamble = min(self._preamble_len, data_len)

            if self._preamble[:of_which_preamble] != data[:of_which_preamble]:
                raise ProtocolError("Invalid HTTP/2 preamble.")

            data = data[of_which_preamble:]
            self._preamble_len -= of_which_preamble
            self._preamble = self._preamble[of_which_preamble:]

        self.data += data

    def _validate_frame_length(self, length):
        """
        Confirm that the frame is an appropriate length.
        """
        if length > self.max_frame_size:
            raise FrameTooLargeError(
                "Received overlong frame: length %d, max %d" %
                (length, self.max_frame_size)
            )

    def _update_header_buffer(self, f):
        """
        Updates the internal header buffer. Returns a frame that should replace
        the current one. May throw exceptions if this frame is invalid.
        """
        # Check if we're in the middle of a headers block. If we are, this
        # frame *must* be a CONTINUATION frame with the same stream ID as the
        # leading HEADERS or PUSH_PROMISE frame. Anything else is a
        # ProtocolError. If the frame *is* valid, append it to the header
        # buffer.
        if self._headers_buffer:
            stream_id = self._headers_buffer[0].stream_id
            valid_frame = (
                f is not None and
                isinstance(f, ContinuationFrame) and
                f.stream_id == stream_id
            )
            if not valid_frame:
                raise ProtocolError("Invalid frame during header block.")

            # Append the frame to the buffer.
            self._headers_buffer.append(f)
            if len(self._headers_buffer) > CONTINUATION_BACKLOG:
                raise ProtocolError("Too many continuation frames received.")

            # If this is the end of the header block, then we want to build a
            # mutant HEADERS frame that's massive. Use the original one we got,
            # then set END_HEADERS and set its data appopriately. If it's not
            # the end of the block, lose the current frame: we can't yield it.
            if 'END_HEADERS' in f.flags:
                f = self._headers_buffer[0]
                f.flags.add('END_HEADERS')
                f.data = b''.join(x.data for x in self._headers_buffer)
                self._headers_buffer = []
            else:
                f = None
        elif (isinstance(f, (HeadersFrame, PushPromiseFrame)) and
                'END_HEADERS' not in f.flags):
            # This is the start of a headers block! Save the frame off and then
            # act like we didn't receive one.
            self._headers_buffer.append(f)
            f = None

        return f

    # The methods below support the iterator protocol.
    def __iter__(self):
        return self

    def __next__(self):
        # First, check that we have enough data to successfully parse the
        # next frame header. If not, bail. Otherwise, parse it.
        if len(self.data) < 9:
            raise StopIteration()

        try:
            f, length = Frame.parse_frame_header(self.data[:9])
        except (InvalidDataError, InvalidFrameError) as e:  # pragma: no cover
            raise ProtocolError(
                "Received frame with invalid header: %s" % str(e)
            )

        # Next, check that we have enough length to parse the frame body. If
        # not, bail, leaving the frame header data in the buffer for next time.
        if len(self.data) < length + 9:
            raise StopIteration()

        # Confirm the frame has an appropriate length.
        self._validate_frame_length(length)

        # Try to parse the frame body
        try:
            f.parse_body(memoryview(self.data[9:9+length]))
        except InvalidDataError:
            raise ProtocolError("Received frame with non-compliant data")
        except InvalidFrameError:
            raise FrameDataMissingError("Frame data missing or invalid")

        # At this point, as we know we'll use or discard the entire frame, we
        # can update the data.
        self.data = self.data[9+length:]

        # Pass the frame through the header buffer.
        f = self._update_header_buffer(f)

        # If we got a frame we didn't understand or shouldn't yield, rather
        # than return None it'd be better if we just tried to get the next
        # frame in the sequence instead. Recurse back into ourselves to do
        # that. This is safe because the amount of work we have to do here is
        # strictly bounded by the length of the buffer.
        return f if f is not None else self.__next__()
