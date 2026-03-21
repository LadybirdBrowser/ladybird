# -*- coding: utf-8 -*-
"""
h2/windows
~~~~~~~~~~

Defines tools for managing HTTP/2 flow control windows.

The objects defined in this module are used to automatically manage HTTP/2
flow control windows. Specifically, they keep track of what the size of the
window is, how much data has been consumed from that window, and how much data
the user has already used. It then implements a basic algorithm that attempts
to manage the flow control window without user input, trying to ensure that it
does not emit too many WINDOW_UPDATE frames.
"""
from __future__ import division

from .exceptions import FlowControlError


# The largest acceptable value for a HTTP/2 flow control window.
LARGEST_FLOW_CONTROL_WINDOW = 2**31 - 1


class WindowManager:
    """
    A basic HTTP/2 window manager.

    :param max_window_size: The maximum size of the flow control window.
    :type max_window_size: ``int``
    """
    def __init__(self, max_window_size):
        assert max_window_size <= LARGEST_FLOW_CONTROL_WINDOW
        self.max_window_size = max_window_size
        self.current_window_size = max_window_size
        self._bytes_processed = 0

    def window_consumed(self, size):
        """
        We have received a certain number of bytes from the remote peer. This
        necessarily shrinks the flow control window!

        :param size: The number of flow controlled bytes we received from the
            remote peer.
        :type size: ``int``
        :returns: Nothing.
        :rtype: ``None``
        """
        self.current_window_size -= size
        if self.current_window_size < 0:
            raise FlowControlError("Flow control window shrunk below 0")

    def window_opened(self, size):
        """
        The flow control window has been incremented, either because of manual
        flow control management or because of the user changing the flow
        control settings. This can have the effect of increasing what we
        consider to be the "maximum" flow control window size.

        This does not increase our view of how many bytes have been processed,
        only of how much space is in the window.

        :param size: The increment to the flow control window we received.
        :type size: ``int``
        :returns: Nothing
        :rtype: ``None``
        """
        self.current_window_size += size

        if self.current_window_size > LARGEST_FLOW_CONTROL_WINDOW:
            raise FlowControlError(
                "Flow control window mustn't exceed %d" %
                LARGEST_FLOW_CONTROL_WINDOW
            )

        if self.current_window_size > self.max_window_size:
            self.max_window_size = self.current_window_size

    def process_bytes(self, size):
        """
        The application has informed us that it has processed a certain number
        of bytes. This may cause us to want to emit a window update frame. If
        we do want to emit a window update frame, this method will return the
        number of bytes that we should increment the window by.

        :param size: The number of flow controlled bytes that the application
            has processed.
        :type size: ``int``
        :returns: The number of bytes to increment the flow control window by,
            or ``None``.
        :rtype: ``int`` or ``None``
        """
        self._bytes_processed += size
        return self._maybe_update_window()

    def _maybe_update_window(self):
        """
        Run the algorithm.

        Our current algorithm can be described like this.

        1. If no bytes have been processed, we immediately return 0. There is
           no meaningful way for us to hand space in the window back to the
           remote peer, so let's not even try.
        2. If there is no space in the flow control window, and we have
           processed at least 1024 bytes (or 1/4 of the window, if the window
           is smaller), we will emit a window update frame. This is to avoid
           the risk of blocking a stream altogether.
        3. If there is space in the flow control window, and we have processed
           at least 1/2 of the window worth of bytes, we will emit a window
           update frame. This is to minimise the number of window update frames
           we have to emit.

        In a healthy system with large flow control windows, this will
        irregularly emit WINDOW_UPDATE frames. This prevents us starving the
        connection by emitting eleventy bajillion WINDOW_UPDATE frames,
        especially in situations where the remote peer is sending a lot of very
        small DATA frames.
        """
        # TODO: Can the window be smaller than 1024 bytes? If not, we can
        # streamline this algorithm.
        if not self._bytes_processed:
            return None

        max_increment = (self.max_window_size - self.current_window_size)
        increment = 0

        # Note that, even though we may increment less than _bytes_processed,
        # we still want to set it to zero whenever we emit an increment. This
        # is because we'll always increment up to the maximum we can.
        if (self.current_window_size == 0) and (
                self._bytes_processed > min(1024, self.max_window_size // 4)):
            increment = min(self._bytes_processed, max_increment)
            self._bytes_processed = 0
        elif self._bytes_processed >= (self.max_window_size // 2):
            increment = min(self._bytes_processed, max_increment)
            self._bytes_processed = 0

        self.current_window_size += increment
        return increment
