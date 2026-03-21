# Copyright 2020, Google Inc.
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
"""Server related utilities."""

import logging
import logging.handlers
import threading
import time

from pywebsocket3 import common, util


def _get_logger_from_class(c):
    return logging.getLogger('%s.%s' % (c.__module__, c.__name__))


def configure_logging(options):
    logging.addLevelName(common.LOGLEVEL_FINE, 'FINE')

    logger = logging.getLogger()
    logger.setLevel(logging.getLevelName(options.log_level.upper()))
    if options.log_file:
        handler = logging.handlers.RotatingFileHandler(options.log_file, 'a',
                                                       options.log_max,
                                                       options.log_count)
    else:
        handler = logging.StreamHandler()
    formatter = logging.Formatter(
        '[%(asctime)s] [%(levelname)s] %(name)s: %(message)s')
    handler.setFormatter(formatter)
    logger.addHandler(handler)

    deflate_log_level_name = logging.getLevelName(
        options.deflate_log_level.upper())
    _get_logger_from_class(util._Deflater).setLevel(deflate_log_level_name)
    _get_logger_from_class(util._Inflater).setLevel(deflate_log_level_name)


class ThreadMonitor(threading.Thread):
    daemon = True

    def __init__(self, interval_in_sec):
        threading.Thread.__init__(self, name='ThreadMonitor')

        self._logger = util.get_class_logger(self)

        self._interval_in_sec = interval_in_sec

    def run(self):
        while True:
            thread_name_list = []
            for thread in threading.enumerate():
                thread_name_list.append(thread.name)
            self._logger.info("%d active threads: %s",
                              threading.active_count(),
                              ', '.join(thread_name_list))
            time.sleep(self._interval_in_sec)


# vi:sts=4 sw=4 et
