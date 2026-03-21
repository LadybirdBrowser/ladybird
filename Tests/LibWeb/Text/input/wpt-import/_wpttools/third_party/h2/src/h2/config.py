# -*- coding: utf-8 -*-
"""
h2/config
~~~~~~~~~

Objects for controlling the configuration of the HTTP/2 stack.
"""

import sys


class _BooleanConfigOption:
    """
    Descriptor for handling a boolean config option.  This will block
    attempts to set boolean config options to non-bools.
    """
    def __init__(self, name):
        self.name = name
        self.attr_name = '_%s' % self.name

    def __get__(self, instance, owner):
        return getattr(instance, self.attr_name)

    def __set__(self, instance, value):
        if not isinstance(value, bool):
            raise ValueError("%s must be a bool" % self.name)
        setattr(instance, self.attr_name, value)


class DummyLogger:
    """
    A Logger object that does not actual logging, hence a DummyLogger.

    For the class the log operation is merely a no-op. The intent is to avoid
    conditionals being sprinkled throughout the h2 code for calls to
    logging functions when no logger is passed into the corresponding object.
    """
    def __init__(self, *vargs):
        pass

    def debug(self, *vargs, **kwargs):
        """
        No-op logging. Only level needed for now.
        """
        pass

    def trace(self, *vargs, **kwargs):
        """
        No-op logging. Only level needed for now.
        """
        pass


class OutputLogger:
    """
    A Logger object that prints to stderr or any other file-like object.

    This class is provided for convenience and not part of the stable API.

    :param file: A file-like object passed to the print function.
        Defaults to ``sys.stderr``.
    :param trace: Enables trace-level output. Defaults to ``False``.
    """
    def __init__(self, file=None, trace_level=False):
        super().__init__()
        self.file = file or sys.stderr
        self.trace_level = trace_level

    def debug(self, fmtstr, *args):
        print(f"h2 (debug): {fmtstr % args}", file=self.file)

    def trace(self, fmtstr, *args):
        if self.trace_level:
            print(f"h2 (trace): {fmtstr % args}", file=self.file)


class H2Configuration:
    """
    An object that controls the way a single HTTP/2 connection behaves.

    This object allows the users to customize behaviour. In particular, it
    allows users to enable or disable optional features, or to otherwise handle
    various unusual behaviours.

    This object has very little behaviour of its own: it mostly just ensures
    that configuration is self-consistent.

    :param client_side: Whether this object is to be used on the client side of
        a connection, or on the server side. Affects the logic used by the
        state machine, the default settings values, the allowable stream IDs,
        and several other properties. Defaults to ``True``.
    :type client_side: ``bool``

    :param header_encoding: Controls whether the headers emitted by this object
        in events are transparently decoded to ``unicode`` strings, and what
        encoding is used to do that decoding. This defaults to ``None``,
        meaning that headers will be returned as bytes. To automatically
        decode headers (that is, to return them as unicode strings), this can
        be set to the string name of any encoding, e.g. ``'utf-8'``.

        .. versionchanged:: 3.0.0
           Changed default value from ``'utf-8'`` to ``None``

    :type header_encoding: ``str``, ``False``, or ``None``

    :param validate_outbound_headers: Controls whether the headers emitted
        by this object are validated against the rules in RFC 7540.
        Disabling this setting will cause outbound header validation to
        be skipped, and allow the object to emit headers that may be illegal
        according to RFC 7540. Defaults to ``True``.
    :type validate_outbound_headers: ``bool``

    :param normalize_outbound_headers: Controls whether the headers emitted
        by this object are normalized before sending.  Disabling this setting
        will cause outbound header normalization to be skipped, and allow
        the object to emit headers that may be illegal according to
        RFC 7540. Defaults to ``True``.
    :type normalize_outbound_headers: ``bool``

    :param validate_inbound_headers: Controls whether the headers received
        by this object are validated against the rules in RFC 7540.
        Disabling this setting will cause inbound header validation to
        be skipped, and allow the object to receive headers that may be illegal
        according to RFC 7540. Defaults to ``True``.
    :type validate_inbound_headers: ``bool``

    :param normalize_inbound_headers: Controls whether the headers received by
        this object are normalized according to the rules of RFC 7540.
        Disabling this setting may lead to h2 emitting header blocks that
        some RFCs forbid, e.g. with multiple cookie fields.

        .. versionadded:: 3.0.0

    :type normalize_inbound_headers: ``bool``

    :param logger: A logger that conforms to the requirements for this module,
        those being no I/O and no context switches, which is needed in order
        to run in asynchronous operation.

        .. versionadded:: 2.6.0

    :type logger: ``logging.Logger``
    """
    client_side = _BooleanConfigOption('client_side')
    validate_outbound_headers = _BooleanConfigOption(
        'validate_outbound_headers'
    )
    normalize_outbound_headers = _BooleanConfigOption(
        'normalize_outbound_headers'
    )
    validate_inbound_headers = _BooleanConfigOption(
        'validate_inbound_headers'
    )
    normalize_inbound_headers = _BooleanConfigOption(
        'normalize_inbound_headers'
    )

    def __init__(self,
                 client_side=True,
                 header_encoding=None,
                 validate_outbound_headers=True,
                 normalize_outbound_headers=True,
                 validate_inbound_headers=True,
                 normalize_inbound_headers=True,
                 logger=None):
        self.client_side = client_side
        self.header_encoding = header_encoding
        self.validate_outbound_headers = validate_outbound_headers
        self.normalize_outbound_headers = normalize_outbound_headers
        self.validate_inbound_headers = validate_inbound_headers
        self.normalize_inbound_headers = normalize_inbound_headers
        self.logger = logger or DummyLogger(__name__)

    @property
    def header_encoding(self):
        """
        Controls whether the headers emitted by this object in events are
        transparently decoded to ``unicode`` strings, and what encoding is used
        to do that decoding. This defaults to ``None``, meaning that headers
        will be returned as bytes. To automatically decode headers (that is, to
        return them as unicode strings), this can be set to the string name of
        any encoding, e.g. ``'utf-8'``.
        """
        return self._header_encoding

    @header_encoding.setter
    def header_encoding(self, value):
        """
        Enforces constraints on the value of header encoding.
        """
        if not isinstance(value, (bool, str, type(None))):
            raise ValueError("header_encoding must be bool, string, or None")
        if value is True:
            raise ValueError("header_encoding cannot be True")
        self._header_encoding = value
