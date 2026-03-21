# Copyright 2012, Google Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
# copyright notice, this list of conditions and the following disclaimer
# in the documentation and/or other materials provided with the
# distribution.
#     * Neither the name of Google Inc. nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from __future__ import absolute_import

from pywebsocket3 import common, util
from pywebsocket3.http_header_util import quote_if_necessary

# The list of available server side extension processor classes.
_available_processors = {}


class ExtensionProcessorInterface(object):
    def __init__(self, request):
        self._logger = util.get_class_logger(self)

        self._request = request
        self._active = True

    def request(self):
        return self._request

    def name(self):
        return None

    def check_consistency_with_other_processors(self, processors):
        pass

    def set_active(self, active):
        self._active = active

    def is_active(self):
        return self._active

    def _get_extension_response_internal(self):
        return None

    def get_extension_response(self):
        if not self._active:
            self._logger.debug('Extension %s is deactivated', self.name())
            return None

        response = self._get_extension_response_internal()
        if response is None:
            self._active = False
        return response

    def _setup_stream_options_internal(self, stream_options):
        pass

    def setup_stream_options(self, stream_options):
        if self._active:
            self._setup_stream_options_internal(stream_options)


def _log_outgoing_compression_ratio(logger, original_bytes, filtered_bytes,
                                    average_ratio):
    # Print inf when ratio is not available.
    ratio = float('inf')
    if original_bytes != 0:
        ratio = float(filtered_bytes) / original_bytes

    logger.debug('Outgoing compression ratio: %f (average: %f)' %
                 (ratio, average_ratio))


def _log_incoming_compression_ratio(logger, received_bytes, filtered_bytes,
                                    average_ratio):
    # Print inf when ratio is not available.
    ratio = float('inf')
    if filtered_bytes != 0:
        ratio = float(received_bytes) / filtered_bytes

    logger.debug('Incoming compression ratio: %f (average: %f)' %
                 (ratio, average_ratio))


def _parse_window_bits(bits):
    """Return parsed integer value iff the given string conforms to the
    grammar of the window bits extension parameters.
    """

    if bits is None:
        raise ValueError('Value is required')

    # For non integer values such as "10.0", ValueError will be raised.
    int_bits = int(bits)

    # First condition is to drop leading zero case e.g. "08".
    if bits != str(int_bits) or int_bits < 8 or int_bits > 15:
        raise ValueError('Invalid value: %r' % bits)

    return int_bits


class _AverageRatioCalculator(object):
    """Stores total bytes of original and result data, and calculates average
    result / original ratio.
    """
    def __init__(self):
        self._total_original_bytes = 0
        self._total_result_bytes = 0

    def add_original_bytes(self, value):
        self._total_original_bytes += value

    def add_result_bytes(self, value):
        self._total_result_bytes += value

    def get_average_ratio(self):
        if self._total_original_bytes != 0:
            return (float(self._total_result_bytes) /
                    self._total_original_bytes)
        else:
            return float('inf')


class PerMessageDeflateExtensionProcessor(ExtensionProcessorInterface):
    """permessage-deflate extension processor.

    Specification:
    http://tools.ietf.org/html/draft-ietf-hybi-permessage-compression-08
    """

    _SERVER_MAX_WINDOW_BITS_PARAM = 'server_max_window_bits'
    _SERVER_NO_CONTEXT_TAKEOVER_PARAM = 'server_no_context_takeover'
    _CLIENT_MAX_WINDOW_BITS_PARAM = 'client_max_window_bits'
    _CLIENT_NO_CONTEXT_TAKEOVER_PARAM = 'client_no_context_takeover'

    def __init__(self, request):
        """Construct PerMessageDeflateExtensionProcessor."""

        ExtensionProcessorInterface.__init__(self, request)
        self._logger = util.get_class_logger(self)

        self._preferred_client_max_window_bits = None
        self._client_no_context_takeover = False

    def name(self):
        # This method returns "deflate" (not "permessage-deflate") for
        # compatibility.
        return 'deflate'

    def _get_extension_response_internal(self):
        for name in self._request.get_parameter_names():
            if name not in [
                    self._SERVER_MAX_WINDOW_BITS_PARAM,
                    self._SERVER_NO_CONTEXT_TAKEOVER_PARAM,
                    self._CLIENT_MAX_WINDOW_BITS_PARAM
            ]:
                self._logger.debug('Unknown parameter: %r', name)
                return None

        server_max_window_bits = None
        if self._request.has_parameter(self._SERVER_MAX_WINDOW_BITS_PARAM):
            server_max_window_bits = self._request.get_parameter_value(
                self._SERVER_MAX_WINDOW_BITS_PARAM)
            try:
                server_max_window_bits = _parse_window_bits(
                    server_max_window_bits)
            except ValueError as e:
                self._logger.debug('Bad %s parameter: %r',
                                   self._SERVER_MAX_WINDOW_BITS_PARAM, e)
                return None

        server_no_context_takeover = self._request.has_parameter(
            self._SERVER_NO_CONTEXT_TAKEOVER_PARAM)
        if (server_no_context_takeover and self._request.get_parameter_value(
                self._SERVER_NO_CONTEXT_TAKEOVER_PARAM) is not None):
            self._logger.debug('%s parameter must not have a value: %r',
                               self._SERVER_NO_CONTEXT_TAKEOVER_PARAM,
                               server_no_context_takeover)
            return None

        # client_max_window_bits from a client indicates whether the client can
        # accept client_max_window_bits from a server or not.
        client_client_max_window_bits = self._request.has_parameter(
            self._CLIENT_MAX_WINDOW_BITS_PARAM)
        if (client_client_max_window_bits
                and self._request.get_parameter_value(
                    self._CLIENT_MAX_WINDOW_BITS_PARAM) is not None):
            self._logger.debug(
                '%s parameter must not have a value in a '
                'client\'s opening handshake: %r',
                self._CLIENT_MAX_WINDOW_BITS_PARAM,
                client_client_max_window_bits)
            return None

        self._rfc1979_deflater = util._RFC1979Deflater(
            server_max_window_bits, server_no_context_takeover)

        # Note that we prepare for incoming messages compressed with window
        # bits upto 15 regardless of the client_max_window_bits value to be
        # sent to the client.
        self._rfc1979_inflater = util._RFC1979Inflater()

        self._framer = _PerMessageDeflateFramer(server_max_window_bits,
                                                server_no_context_takeover)
        self._framer.set_bfinal(False)
        self._framer.set_compress_outgoing_enabled(True)

        response = common.ExtensionParameter(self._request.name())

        if server_max_window_bits is not None:
            response.add_parameter(self._SERVER_MAX_WINDOW_BITS_PARAM,
                                   str(server_max_window_bits))

        if server_no_context_takeover:
            response.add_parameter(self._SERVER_NO_CONTEXT_TAKEOVER_PARAM,
                                   None)

        if self._preferred_client_max_window_bits is not None:
            if not client_client_max_window_bits:
                self._logger.debug(
                    'Processor is configured to use %s but '
                    'the client cannot accept it',
                    self._CLIENT_MAX_WINDOW_BITS_PARAM)
                return None
            response.add_parameter(self._CLIENT_MAX_WINDOW_BITS_PARAM,
                                   str(self._preferred_client_max_window_bits))

        if self._client_no_context_takeover:
            response.add_parameter(self._CLIENT_NO_CONTEXT_TAKEOVER_PARAM,
                                   None)

        self._logger.debug('Enable %s extension ('
                           'request: server_max_window_bits=%s; '
                           'server_no_context_takeover=%r, '
                           'response: client_max_window_bits=%s; '
                           'client_no_context_takeover=%r)' %
                           (self._request.name(), server_max_window_bits,
                            server_no_context_takeover,
                            self._preferred_client_max_window_bits,
                            self._client_no_context_takeover))

        return response

    def _setup_stream_options_internal(self, stream_options):
        self._framer.setup_stream_options(stream_options)

    def set_client_max_window_bits(self, value):
        """If this option is specified, this class adds the
        client_max_window_bits extension parameter to the handshake response,
        but doesn't reduce the LZ77 sliding window size of its inflater.
        I.e., you can use this for testing client implementation but cannot
        reduce memory usage of this class.

        If this method has been called with True and an offer without the
        client_max_window_bits extension parameter is received,

        - (When processing the permessage-deflate extension) this processor
          declines the request.
        - (When processing the permessage-compress extension) this processor
          accepts the request.
        """

        self._preferred_client_max_window_bits = value

    def set_client_no_context_takeover(self, value):
        """If this option is specified, this class adds the
        client_no_context_takeover extension parameter to the handshake
        response, but doesn't reset inflater for each message. I.e., you can
        use this for testing client implementation but cannot reduce memory
        usage of this class.
        """

        self._client_no_context_takeover = value

    def set_bfinal(self, value):
        self._framer.set_bfinal(value)

    def enable_outgoing_compression(self):
        self._framer.set_compress_outgoing_enabled(True)

    def disable_outgoing_compression(self):
        self._framer.set_compress_outgoing_enabled(False)


class _PerMessageDeflateFramer(object):
    """A framer for extensions with per-message DEFLATE feature."""
    def __init__(self, deflate_max_window_bits, deflate_no_context_takeover):
        self._logger = util.get_class_logger(self)

        self._rfc1979_deflater = util._RFC1979Deflater(
            deflate_max_window_bits, deflate_no_context_takeover)

        self._rfc1979_inflater = util._RFC1979Inflater()

        self._bfinal = False

        self._compress_outgoing_enabled = False

        # True if a message is fragmented and compression is ongoing.
        self._compress_ongoing = False

        # Calculates
        #     (Total outgoing bytes supplied to this filter) /
        #     (Total bytes sent to the network after applying this filter)
        self._outgoing_average_ratio_calculator = _AverageRatioCalculator()

        # Calculates
        #     (Total bytes received from the network) /
        #     (Total incoming bytes obtained after applying this filter)
        self._incoming_average_ratio_calculator = _AverageRatioCalculator()

    def set_bfinal(self, value):
        self._bfinal = value

    def set_compress_outgoing_enabled(self, value):
        self._compress_outgoing_enabled = value

    def _process_incoming_message(self, message, decompress):
        if not decompress:
            return message

        received_payload_size = len(message)
        self._incoming_average_ratio_calculator.add_result_bytes(
            received_payload_size)

        message = self._rfc1979_inflater.filter(message)

        filtered_payload_size = len(message)
        self._incoming_average_ratio_calculator.add_original_bytes(
            filtered_payload_size)

        _log_incoming_compression_ratio(
            self._logger, received_payload_size, filtered_payload_size,
            self._incoming_average_ratio_calculator.get_average_ratio())

        return message

    def _process_outgoing_message(self, message, end, binary):
        if not binary:
            message = message.encode('utf-8')

        if not self._compress_outgoing_enabled:
            return message

        original_payload_size = len(message)
        self._outgoing_average_ratio_calculator.add_original_bytes(
            original_payload_size)

        message = self._rfc1979_deflater.filter(message,
                                                end=end,
                                                bfinal=self._bfinal)

        filtered_payload_size = len(message)
        self._outgoing_average_ratio_calculator.add_result_bytes(
            filtered_payload_size)

        _log_outgoing_compression_ratio(
            self._logger, original_payload_size, filtered_payload_size,
            self._outgoing_average_ratio_calculator.get_average_ratio())

        if not self._compress_ongoing:
            self._outgoing_frame_filter.set_compression_bit()
        self._compress_ongoing = not end
        return message

    def _process_incoming_frame(self, frame):
        if frame.rsv1 == 1 and not common.is_control_opcode(frame.opcode):
            self._incoming_message_filter.decompress_next_message()
            frame.rsv1 = 0

    def _process_outgoing_frame(self, frame, compression_bit):
        if (not compression_bit or common.is_control_opcode(frame.opcode)):
            return

        frame.rsv1 = 1

    def setup_stream_options(self, stream_options):
        """Creates filters and sets them to the StreamOptions."""
        class _OutgoingMessageFilter(object):
            def __init__(self, parent):
                self._parent = parent

            def filter(self, message, end=True, binary=False):
                return self._parent._process_outgoing_message(
                    message, end, binary)

        class _IncomingMessageFilter(object):
            def __init__(self, parent):
                self._parent = parent
                self._decompress_next_message = False

            def decompress_next_message(self):
                self._decompress_next_message = True

            def filter(self, message):
                message = self._parent._process_incoming_message(
                    message, self._decompress_next_message)
                self._decompress_next_message = False
                return message

        self._outgoing_message_filter = _OutgoingMessageFilter(self)
        self._incoming_message_filter = _IncomingMessageFilter(self)
        stream_options.outgoing_message_filters.append(
            self._outgoing_message_filter)
        stream_options.incoming_message_filters.append(
            self._incoming_message_filter)

        class _OutgoingFrameFilter(object):
            def __init__(self, parent):
                self._parent = parent
                self._set_compression_bit = False

            def set_compression_bit(self):
                self._set_compression_bit = True

            def filter(self, frame):
                self._parent._process_outgoing_frame(frame,
                                                     self._set_compression_bit)
                self._set_compression_bit = False

        class _IncomingFrameFilter(object):
            def __init__(self, parent):
                self._parent = parent

            def filter(self, frame):
                self._parent._process_incoming_frame(frame)

        self._outgoing_frame_filter = _OutgoingFrameFilter(self)
        self._incoming_frame_filter = _IncomingFrameFilter(self)
        stream_options.outgoing_frame_filters.append(
            self._outgoing_frame_filter)
        stream_options.incoming_frame_filters.append(
            self._incoming_frame_filter)

        stream_options.encode_text_message_to_utf8 = False


_available_processors[common.PERMESSAGE_DEFLATE_EXTENSION] = (
    PerMessageDeflateExtensionProcessor)


def get_extension_processor(extension_request):
    """Given an ExtensionParameter representing an extension offer received
    from a client, configures and returns an instance of the corresponding
    extension processor class.
    """

    processor_class = _available_processors.get(extension_request.name())
    if processor_class is None:
        return None
    return processor_class(extension_request)


# vi:sts=4 sw=4 et
