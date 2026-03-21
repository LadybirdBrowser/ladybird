# -*- coding: utf-8 -*-
"""
h2/errors
~~~~~~~~~

Global error code registry containing the established HTTP/2 error codes.

The current registry is available at:
https://tools.ietf.org/html/rfc7540#section-11.4
"""
import enum


class ErrorCodes(enum.IntEnum):
    """
    All known HTTP/2 error codes.

    .. versionadded:: 2.5.0
    """
    #: Graceful shutdown.
    NO_ERROR = 0x0

    #: Protocol error detected.
    PROTOCOL_ERROR = 0x1

    #: Implementation fault.
    INTERNAL_ERROR = 0x2

    #: Flow-control limits exceeded.
    FLOW_CONTROL_ERROR = 0x3

    #: Settings not acknowledged.
    SETTINGS_TIMEOUT = 0x4

    #: Frame received for closed stream.
    STREAM_CLOSED = 0x5

    #: Frame size incorrect.
    FRAME_SIZE_ERROR = 0x6

    #: Stream not processed.
    REFUSED_STREAM = 0x7

    #: Stream cancelled.
    CANCEL = 0x8

    #: Compression state not updated.
    COMPRESSION_ERROR = 0x9

    #: TCP connection error for CONNECT method.
    CONNECT_ERROR = 0xa

    #: Processing capacity exceeded.
    ENHANCE_YOUR_CALM = 0xb

    #: Negotiated TLS parameters not acceptable.
    INADEQUATE_SECURITY = 0xc

    #: Use HTTP/1.1 for the request.
    HTTP_1_1_REQUIRED = 0xd


def _error_code_from_int(code):
    """
    Given an integer error code, returns either one of :class:`ErrorCodes
    <h2.errors.ErrorCodes>` or, if not present in the known set of codes,
    returns the integer directly.
    """
    try:
        return ErrorCodes(code)
    except ValueError:
        return code


__all__ = ['ErrorCodes']
