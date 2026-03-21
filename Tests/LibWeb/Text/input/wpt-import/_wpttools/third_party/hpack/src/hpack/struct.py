# -*- coding: utf-8 -*-
"""
hpack/struct
~~~~~~~~~~~~

Contains structures for representing header fields with associated metadata.
"""


class HeaderTuple(tuple):
    """
    A data structure that stores a single header field.

    HTTP headers can be thought of as tuples of ``(field name, field value)``.
    A single header block is a sequence of such tuples.

    In HTTP/2, however, certain bits of additional information are required for
    compressing these headers: in particular, whether the header field can be
    safely added to the HPACK compression context.

    This class stores a header that can be added to the compression context. In
    all other ways it behaves exactly like a tuple.
    """
    __slots__ = ()

    indexable = True

    def __new__(cls, *args):
        return tuple.__new__(cls, args)


class NeverIndexedHeaderTuple(HeaderTuple):
    """
    A data structure that stores a single header field that cannot be added to
    a HTTP/2 header compression context.
    """
    __slots__ = ()

    indexable = False
