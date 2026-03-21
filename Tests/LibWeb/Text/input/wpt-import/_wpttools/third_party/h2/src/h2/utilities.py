# -*- coding: utf-8 -*-
"""
h2/utilities
~~~~~~~~~~~~

Utility functions that do not belong in a separate module.
"""
import collections
import re
from string import whitespace

from hpack import HeaderTuple, NeverIndexedHeaderTuple

from .exceptions import ProtocolError, FlowControlError

UPPER_RE = re.compile(b"[A-Z]")

# A set of headers that are hop-by-hop or connection-specific and thus
# forbidden in HTTP/2. This list comes from RFC 7540 § 8.1.2.2.
CONNECTION_HEADERS = frozenset([
    b'connection', u'connection',
    b'proxy-connection', u'proxy-connection',
    b'keep-alive', u'keep-alive',
    b'transfer-encoding', u'transfer-encoding',
    b'upgrade', u'upgrade',
])


_ALLOWED_PSEUDO_HEADER_FIELDS = frozenset([
    b':method', u':method',
    b':scheme', u':scheme',
    b':authority', u':authority',
    b':path', u':path',
    b':status', u':status',
    b':protocol', u':protocol',
])


_SECURE_HEADERS = frozenset([
    # May have basic credentials which are vulnerable to dictionary attacks.
    b'authorization', u'authorization',
    b'proxy-authorization', u'proxy-authorization',
])


_REQUEST_ONLY_HEADERS = frozenset([
    b':scheme', u':scheme',
    b':path', u':path',
    b':authority', u':authority',
    b':method', u':method',
    b':protocol', u':protocol',
])


_RESPONSE_ONLY_HEADERS = frozenset([b':status', u':status'])


# A Set of pseudo headers that are only valid if the method is
# CONNECT, see RFC 8441 § 5
_CONNECT_REQUEST_ONLY_HEADERS = frozenset([b':protocol', u':protocol'])


_WHITESPACE = frozenset(map(ord, whitespace))


def _secure_headers(headers, hdr_validation_flags):
    """
    Certain headers are at risk of being attacked during the header compression
    phase, and so need to be kept out of header compression contexts. This
    function automatically transforms certain specific headers into HPACK
    never-indexed fields to ensure they don't get added to header compression
    contexts.

    This function currently implements two rules:

    - 'authorization' and 'proxy-authorization' fields are automatically made
      never-indexed.
    - Any 'cookie' header field shorter than 20 bytes long is made
      never-indexed.

    These fields are the most at-risk. These rules are inspired by Firefox
    and nghttp2.
    """
    for header in headers:
        if header[0] in _SECURE_HEADERS:
            yield NeverIndexedHeaderTuple(*header)
        elif header[0] in (b'cookie', u'cookie') and len(header[1]) < 20:
            yield NeverIndexedHeaderTuple(*header)
        else:
            yield header


def extract_method_header(headers):
    """
    Extracts the request method from the headers list.
    """
    for k, v in headers:
        if k in (b':method', u':method'):
            if not isinstance(v, bytes):
                return v.encode('utf-8')
            else:
                return v


def is_informational_response(headers):
    """
    Searches a header block for a :status header to confirm that a given
    collection of headers are an informational response. Assumes the header
    block is well formed: that is, that the HTTP/2 special headers are first
    in the block, and so that it can stop looking when it finds the first
    header field whose name does not begin with a colon.

    :param headers: The HTTP/2 header block.
    :returns: A boolean indicating if this is an informational response.
    """
    for n, v in headers:
        if isinstance(n, bytes):
            sigil = b':'
            status = b':status'
            informational_start = b'1'
        else:
            sigil = u':'
            status = u':status'
            informational_start = u'1'

        # If we find a non-special header, we're done here: stop looping.
        if not n.startswith(sigil):
            return False

        # This isn't the status header, bail.
        if n != status:
            continue

        # If the first digit is a 1, we've got informational headers.
        return v.startswith(informational_start)


def guard_increment_window(current, increment):
    """
    Increments a flow control window, guarding against that window becoming too
    large.

    :param current: The current value of the flow control window.
    :param increment: The increment to apply to that window.
    :returns: The new value of the window.
    :raises: ``FlowControlError``
    """
    # The largest value the flow control window may take.
    LARGEST_FLOW_CONTROL_WINDOW = 2**31 - 1

    new_size = current + increment

    if new_size > LARGEST_FLOW_CONTROL_WINDOW:
        raise FlowControlError(
            "May not increment flow control window past %d" %
            LARGEST_FLOW_CONTROL_WINDOW
        )

    return new_size


def authority_from_headers(headers):
    """
    Given a header set, searches for the authority header and returns the
    value.

    Note that this doesn't terminate early, so should only be called if the
    headers are for a client request. Otherwise, will loop over the entire
    header set, which is potentially unwise.

    :param headers: The HTTP header set.
    :returns: The value of the authority header, or ``None``.
    :rtype: ``bytes`` or ``None``.
    """
    for n, v in headers:
        # This gets run against headers that come both from HPACK and from the
        # user, so we may have unicode floating around in here. We only want
        # bytes.
        if n in (b':authority', u':authority'):
            return v.encode('utf-8') if not isinstance(v, bytes) else v

    return None


# Flags used by the validate_headers pipeline to determine which checks
# should be applied to a given set of headers.
HeaderValidationFlags = collections.namedtuple(
    'HeaderValidationFlags',
    ['is_client', 'is_trailer', 'is_response_header', 'is_push_promise']
)


def validate_headers(headers, hdr_validation_flags):
    """
    Validates a header sequence against a set of constraints from RFC 7540.

    :param headers: The HTTP header set.
    :param hdr_validation_flags: An instance of HeaderValidationFlags.
    """
    # This validation logic is built on a sequence of generators that are
    # iterated over to provide the final header list. This reduces some of the
    # overhead of doing this checking. However, it's worth noting that this
    # checking remains somewhat expensive, and attempts should be made wherever
    # possible to reduce the time spent doing them.
    #
    # For example, we avoid tuple unpacking in loops because it represents a
    # fixed cost that we don't want to spend, instead indexing into the header
    # tuples.
    headers = _reject_empty_header_names(
        headers, hdr_validation_flags
    )
    headers = _reject_uppercase_header_fields(
        headers, hdr_validation_flags
    )
    headers = _reject_surrounding_whitespace(
        headers, hdr_validation_flags
    )
    headers = _reject_te(
        headers, hdr_validation_flags
    )
    headers = _reject_connection_header(
        headers, hdr_validation_flags
    )
    headers = _reject_pseudo_header_fields(
        headers, hdr_validation_flags
    )
    headers = _check_host_authority_header(
        headers, hdr_validation_flags
    )
    headers = _check_path_header(headers, hdr_validation_flags)

    return headers


def _reject_empty_header_names(headers, hdr_validation_flags):
    """
    Raises a ProtocolError if any header names are empty (length 0).
    While hpack decodes such headers without errors, they are semantically
    forbidden in HTTP, see RFC 7230, stating that they must be at least one
    character long.
    """
    for header in headers:
        if len(header[0]) == 0:
            raise ProtocolError("Received header name with zero length.")
        yield header


def _reject_uppercase_header_fields(headers, hdr_validation_flags):
    """
    Raises a ProtocolError if any uppercase character is found in a header
    block.
    """
    for header in headers:
        if UPPER_RE.search(header[0]):
            raise ProtocolError(
                "Received uppercase header name %s." % header[0])
        yield header


def _reject_surrounding_whitespace(headers, hdr_validation_flags):
    """
    Raises a ProtocolError if any header name or value is surrounded by
    whitespace characters.
    """
    # For compatibility with RFC 7230 header fields, we need to allow the field
    # value to be an empty string. This is ludicrous, but technically allowed.
    # The field name may not be empty, though, so we can safely assume that it
    # must have at least one character in it and throw exceptions if it
    # doesn't.
    for header in headers:
        if header[0][0] in _WHITESPACE or header[0][-1] in _WHITESPACE:
            raise ProtocolError(
                "Received header name surrounded by whitespace %r" % header[0])
        if header[1] and ((header[1][0] in _WHITESPACE) or
           (header[1][-1] in _WHITESPACE)):
            raise ProtocolError(
                "Received header value surrounded by whitespace %r" % header[1]
            )
        yield header


def _reject_te(headers, hdr_validation_flags):
    """
    Raises a ProtocolError if the TE header is present in a header block and
    its value is anything other than "trailers".
    """
    for header in headers:
        if header[0] in (b'te', u'te'):
            if header[1].lower() not in (b'trailers', u'trailers'):
                raise ProtocolError(
                    "Invalid value for TE header: %s" %
                    header[1]
                )

        yield header


def _reject_connection_header(headers, hdr_validation_flags):
    """
    Raises a ProtocolError if the Connection header is present in a header
    block.
    """
    for header in headers:
        if header[0] in CONNECTION_HEADERS:
            raise ProtocolError(
                "Connection-specific header field present: %s." % header[0]
            )

        yield header


def _custom_startswith(test_string, bytes_prefix, unicode_prefix):
    """
    Given a string that might be a bytestring or a Unicode string,
    return True if it starts with the appropriate prefix.
    """
    if isinstance(test_string, bytes):
        return test_string.startswith(bytes_prefix)
    else:
        return test_string.startswith(unicode_prefix)


def _assert_header_in_set(string_header, bytes_header, header_set):
    """
    Given a set of header names, checks whether the string or byte version of
    the header name is present. Raises a Protocol error with the appropriate
    error if it's missing.
    """
    if not (string_header in header_set or bytes_header in header_set):
        raise ProtocolError(
            "Header block missing mandatory %s header" % string_header
        )


def _reject_pseudo_header_fields(headers, hdr_validation_flags):
    """
    Raises a ProtocolError if duplicate pseudo-header fields are found in a
    header block or if a pseudo-header field appears in a block after an
    ordinary header field.

    Raises a ProtocolError if pseudo-header fields are found in trailers.
    """
    seen_pseudo_header_fields = set()
    seen_regular_header = False
    method = None

    for header in headers:
        if _custom_startswith(header[0], b':', u':'):
            if header[0] in seen_pseudo_header_fields:
                raise ProtocolError(
                    "Received duplicate pseudo-header field %s" % header[0]
                )

            seen_pseudo_header_fields.add(header[0])

            if seen_regular_header:
                raise ProtocolError(
                    "Received pseudo-header field out of sequence: %s" %
                    header[0]
                )

            if header[0] not in _ALLOWED_PSEUDO_HEADER_FIELDS:
                raise ProtocolError(
                    "Received custom pseudo-header field %s" % header[0]
                )

            if header[0] in (b':method', u':method'):
                if not isinstance(header[1], bytes):
                    method = header[1].encode('utf-8')
                else:
                    method = header[1]

        else:
            seen_regular_header = True

        yield header

    # Check the pseudo-headers we got to confirm they're acceptable.
    _check_pseudo_header_field_acceptability(
        seen_pseudo_header_fields, method, hdr_validation_flags
    )


def _check_pseudo_header_field_acceptability(pseudo_headers,
                                             method,
                                             hdr_validation_flags):
    """
    Given the set of pseudo-headers present in a header block and the
    validation flags, confirms that RFC 7540 allows them.
    """
    # Pseudo-header fields MUST NOT appear in trailers - RFC 7540 § 8.1.2.1
    if hdr_validation_flags.is_trailer and pseudo_headers:
        raise ProtocolError(
            "Received pseudo-header in trailer %s" % pseudo_headers
        )

    # If ':status' pseudo-header is not there in a response header, reject it.
    # Similarly, if ':path', ':method', or ':scheme' are not there in a request
    # header, reject it. Additionally, if a response contains any request-only
    # headers or vice-versa, reject it.
    # Relevant RFC section: RFC 7540 § 8.1.2.4
    # https://tools.ietf.org/html/rfc7540#section-8.1.2.4
    if hdr_validation_flags.is_response_header:
        _assert_header_in_set(u':status', b':status', pseudo_headers)
        invalid_response_headers = pseudo_headers & _REQUEST_ONLY_HEADERS
        if invalid_response_headers:
            raise ProtocolError(
                "Encountered request-only headers %s" %
                invalid_response_headers
            )
    elif (not hdr_validation_flags.is_response_header and
          not hdr_validation_flags.is_trailer):
        # This is a request, so we need to have seen :path, :method, and
        # :scheme.
        _assert_header_in_set(u':path', b':path', pseudo_headers)
        _assert_header_in_set(u':method', b':method', pseudo_headers)
        _assert_header_in_set(u':scheme', b':scheme', pseudo_headers)
        invalid_request_headers = pseudo_headers & _RESPONSE_ONLY_HEADERS
        if invalid_request_headers:
            raise ProtocolError(
                "Encountered response-only headers %s" %
                invalid_request_headers
            )
        if method != b'CONNECT':
            invalid_headers = pseudo_headers & _CONNECT_REQUEST_ONLY_HEADERS
            if invalid_headers:
                raise ProtocolError(
                    "Encountered connect-request-only headers %s" %
                    invalid_headers
                )


def _validate_host_authority_header(headers):
    """
    Given the :authority and Host headers from a request block that isn't
    a trailer, check that:
     1. At least one of these headers is set.
     2. If both headers are set, they match.

    :param headers: The HTTP header set.
    :raises: ``ProtocolError``
    """
    # We use None as a sentinel value.  Iterate over the list of headers,
    # and record the value of these headers (if present).  We don't need
    # to worry about receiving duplicate :authority headers, as this is
    # enforced by the _reject_pseudo_header_fields() pipeline.
    #
    # TODO: We should also guard against receiving duplicate Host headers,
    # and against sending duplicate headers.
    authority_header_val = None
    host_header_val = None

    for header in headers:
        if header[0] in (b':authority', u':authority'):
            authority_header_val = header[1]
        elif header[0] in (b'host', u'host'):
            host_header_val = header[1]

        yield header

    # If we have not-None values for these variables, then we know we saw
    # the corresponding header.
    authority_present = (authority_header_val is not None)
    host_present = (host_header_val is not None)

    # It is an error for a request header block to contain neither
    # an :authority header nor a Host header.
    if not authority_present and not host_present:
        raise ProtocolError(
            "Request header block does not have an :authority or Host header."
        )

    # If we receive both headers, they should definitely match.
    if authority_present and host_present:
        if authority_header_val != host_header_val:
            raise ProtocolError(
                "Request header block has mismatched :authority and "
                "Host headers: %r / %r"
                % (authority_header_val, host_header_val)
            )


def _check_host_authority_header(headers, hdr_validation_flags):
    """
    Raises a ProtocolError if a header block arrives that does not contain an
    :authority or a Host header, or if a header block contains both fields,
    but their values do not match.
    """
    # We only expect to see :authority and Host headers on request header
    # blocks that aren't trailers, so skip this validation if this is a
    # response header or we're looking at trailer blocks.
    skip_validation = (
        hdr_validation_flags.is_response_header or
        hdr_validation_flags.is_trailer
    )
    if skip_validation:
        return headers

    return _validate_host_authority_header(headers)


def _check_path_header(headers, hdr_validation_flags):
    """
    Raise a ProtocolError if a header block arrives or is sent that contains an
    empty :path header.
    """
    def inner():
        for header in headers:
            if header[0] in (b':path', u':path'):
                if not header[1]:
                    raise ProtocolError("An empty :path header is forbidden")

            yield header

    # We only expect to see :authority and Host headers on request header
    # blocks that aren't trailers, so skip this validation if this is a
    # response header or we're looking at trailer blocks.
    skip_validation = (
        hdr_validation_flags.is_response_header or
        hdr_validation_flags.is_trailer
    )
    if skip_validation:
        return headers
    else:
        return inner()


def _lowercase_header_names(headers, hdr_validation_flags):
    """
    Given an iterable of header two-tuples, rebuilds that iterable with the
    header names lowercased. This generator produces tuples that preserve the
    original type of the header tuple for tuple and any ``HeaderTuple``.
    """
    for header in headers:
        if isinstance(header, HeaderTuple):
            yield header.__class__(header[0].lower(), header[1])
        else:
            yield (header[0].lower(), header[1])


def _strip_surrounding_whitespace(headers, hdr_validation_flags):
    """
    Given an iterable of header two-tuples, strip both leading and trailing
    whitespace from both header names and header values. This generator
    produces tuples that preserve the original type of the header tuple for
    tuple and any ``HeaderTuple``.
    """
    for header in headers:
        if isinstance(header, HeaderTuple):
            yield header.__class__(header[0].strip(), header[1].strip())
        else:
            yield (header[0].strip(), header[1].strip())


def _strip_connection_headers(headers, hdr_validation_flags):
    """
    Strip any connection headers as per RFC7540 § 8.1.2.2.
    """
    for header in headers:
        if header[0] not in CONNECTION_HEADERS:
            yield header


def _check_sent_host_authority_header(headers, hdr_validation_flags):
    """
    Raises an InvalidHeaderBlockError if we try to send a header block
    that does not contain an :authority or a Host header, or if
    the header block contains both fields, but their values do not match.
    """
    # We only expect to see :authority and Host headers on request header
    # blocks that aren't trailers, so skip this validation if this is a
    # response header or we're looking at trailer blocks.
    skip_validation = (
        hdr_validation_flags.is_response_header or
        hdr_validation_flags.is_trailer
    )
    if skip_validation:
        return headers

    return _validate_host_authority_header(headers)


def _combine_cookie_fields(headers, hdr_validation_flags):
    """
    RFC 7540 § 8.1.2.5 allows HTTP/2 clients to split the Cookie header field,
    which must normally appear only once, into multiple fields for better
    compression. However, they MUST be joined back up again when received.
    This normalization step applies that transform. The side-effect is that
    all cookie fields now appear *last* in the header block.
    """
    # There is a problem here about header indexing. Specifically, it's
    # possible that all these cookies are sent with different header indexing
    # values. At this point it shouldn't matter too much, so we apply our own
    # logic and make them never-indexed.
    cookies = []
    for header in headers:
        if header[0] == b'cookie':
            cookies.append(header[1])
        else:
            yield header
    if cookies:
        cookie_val = b'; '.join(cookies)
        yield NeverIndexedHeaderTuple(b'cookie', cookie_val)


def normalize_outbound_headers(headers, hdr_validation_flags):
    """
    Normalizes a header sequence that we are about to send.

    :param headers: The HTTP header set.
    :param hdr_validation_flags: An instance of HeaderValidationFlags.
    """
    headers = _lowercase_header_names(headers, hdr_validation_flags)
    headers = _strip_surrounding_whitespace(headers, hdr_validation_flags)
    headers = _strip_connection_headers(headers, hdr_validation_flags)
    headers = _secure_headers(headers, hdr_validation_flags)

    return headers


def normalize_inbound_headers(headers, hdr_validation_flags):
    """
    Normalizes a header sequence that we have received.

    :param headers: The HTTP header set.
    :param hdr_validation_flags: An instance of HeaderValidationFlags
    """
    headers = _combine_cookie_fields(headers, hdr_validation_flags)
    return headers


def validate_outbound_headers(headers, hdr_validation_flags):
    """
    Validates and normalizes a header sequence that we are about to send.

    :param headers: The HTTP header set.
    :param hdr_validation_flags: An instance of HeaderValidationFlags.
    """
    headers = _reject_te(
        headers, hdr_validation_flags
    )
    headers = _reject_connection_header(
        headers, hdr_validation_flags
    )
    headers = _reject_pseudo_header_fields(
        headers, hdr_validation_flags
    )
    headers = _check_sent_host_authority_header(
        headers, hdr_validation_flags
    )
    headers = _check_path_header(headers, hdr_validation_flags)

    return headers


class SizeLimitDict(collections.OrderedDict):

    def __init__(self, *args, **kwargs):
        self._size_limit = kwargs.pop("size_limit", None)
        super(SizeLimitDict, self).__init__(*args, **kwargs)

        self._check_size_limit()

    def __setitem__(self, key, value):
        super(SizeLimitDict, self).__setitem__(key, value)

        self._check_size_limit()

    def _check_size_limit(self):
        if self._size_limit is not None:
            while len(self) > self._size_limit:
                self.popitem(last=False)
