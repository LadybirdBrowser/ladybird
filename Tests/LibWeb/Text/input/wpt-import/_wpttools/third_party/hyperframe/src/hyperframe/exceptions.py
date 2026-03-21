# -*- coding: utf-8 -*-
"""
hyperframe/exceptions
~~~~~~~~~~~~~~~~~~~~~

Defines the exceptions that can be thrown by hyperframe.
"""


class HyperframeError(Exception):
    """
    The base class for all exceptions for the hyperframe module.

    .. versionadded:: 6.0.0
    """


class UnknownFrameError(HyperframeError):
    """
    A frame of unknown type was received.

    .. versionchanged:: 6.0.0
        Changed base class from `ValueError` to :class:`HyperframeError`
    """
    def __init__(self, frame_type: int, length: int) -> None:
        #: The type byte of the unknown frame that was received.
        self.frame_type = frame_type

        #: The length of the data portion of the unknown frame.
        self.length = length

    def __str__(self) -> str:
        return (
            "UnknownFrameError: Unknown frame type 0x%X received, "
            "length %d bytes" % (self.frame_type, self.length)
        )


class InvalidPaddingError(HyperframeError):
    """
    A frame with invalid padding was received.

    .. versionchanged:: 6.0.0
        Changed base class from `ValueError` to :class:`HyperframeError`
    """
    pass


class InvalidFrameError(HyperframeError):
    """
    Parsing a frame failed because the data was not laid out appropriately.

    .. versionadded:: 3.0.2

    .. versionchanged:: 6.0.0
        Changed base class from `ValueError` to :class:`HyperframeError`
    """
    pass


class InvalidDataError(HyperframeError):
    """
    Content or data of a frame was is invalid or violates the specification.

    .. versionadded:: 6.0.0
    """
    pass
