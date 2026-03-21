#!/usr/bin/env python
#
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
"""Standalone WebSocket server.

Use this file to launch pywebsocket as a standalone server.


BASIC USAGE
===========

Go to the src directory and run

  $ python pywebsocket3/standalone.py [-p <ws_port>]
                                         [-w <websock_handlers>]
                                         [-d <document_root>]

<ws_port> is the port number to use for ws:// connection.

<document_root> is the path to the root directory of HTML files.

<websock_handlers> is the path to the root directory of WebSocket handlers.
If not specified, <document_root> will be used. See __init__.py (or
run $ pydoc pywebsocket3) for how to write WebSocket handlers.

For more detail and other options, run

  $ python pywebsocket3/standalone.py --help

or see _build_option_parser method below.

For trouble shooting, adding "--log_level debug" might help you.


TRY DEMO
========

Go to the src directory and run standalone.py with -d option to set the
document root to the directory containing example HTMLs and handlers like this:

  $ cd src
  $ PYTHONPATH=. python pywebsocket3/standalone.py -d example

to launch pywebsocket with the sample handler and html on port 80. Open
http://localhost/console.html, click the connect button, type something into
the text box next to the send button and click the send button. If everything
is working, you'll see the message you typed echoed by the server.


USING TLS
=========

To run the standalone server with TLS support, run it with -t, -k, and -c
options. When TLS is enabled, the standalone server accepts only TLS connection.

Note that when ssl module is used and the key/cert location is incorrect,
TLS connection silently fails while pyOpenSSL fails on startup.

Example:

  $ PYTHONPATH=. python pywebsocket3/standalone.py \
        -d example \
        -p 10443 \
        -t \
        -c ../test/cert/cert.pem \
        -k ../test/cert/key.pem \

Note that when passing a relative path to -c and -k option, it will be resolved
using the document root directory as the base.


USING CLIENT AUTHENTICATION
===========================

To run the standalone server with TLS client authentication support, run it with
--tls-client-auth and --tls-client-ca options in addition to ones required for
TLS support.

Example:

  $ PYTHONPATH=. python pywebsocket3/standalone.py -d example -p 10443 -t \
        -c ../test/cert/cert.pem -k ../test/cert/key.pem \
        --tls-client-auth \
        --tls-client-ca=../test/cert/cacert.pem

Note that when passing a relative path to --tls-client-ca option, it will be
resolved using the document root directory as the base.


CONFIGURATION FILE
==================

You can also write a configuration file and use it by specifying the path to
the configuration file by --config option. Please write a configuration file
following the documentation of the Python ConfigParser library. Name of each
entry must be the long version argument name. E.g. to set log level to debug,
add the following line:

log_level=debug

For options which doesn't take value, please add some fake value. E.g. for
--tls option, add the following line:

tls=True

Note that tls will be enabled even if you write tls=False as the value part is
fake.

When both a command line argument and a configuration file entry are set for
the same configuration item, the command line value will override one in the
configuration file.


THREADING
=========

This server is derived from SocketServer.ThreadingMixIn. Hence a thread is
used for each request.


SECURITY WARNING
================

This uses CGIHTTPServer and CGIHTTPServer is not secure.
It may execute arbitrary Python code or external programs. It should not be
used outside a firewall.
"""

from __future__ import absolute_import

import argparse
import base64
import logging
import os
import sys
import traceback

import six
from six.moves import configparser

from pywebsocket3 import common, server_util, util
from pywebsocket3.websocket_server import WebSocketServer


_DEFAULT_LOG_MAX_BYTES = 1024 * 256
_DEFAULT_LOG_BACKUP_COUNT = 5

_DEFAULT_REQUEST_QUEUE_SIZE = 128


def _build_option_parser():
    parser = argparse.ArgumentParser()

    parser.add_argument(
        '--config',
        dest='config_file',
        type=six.text_type,
        default=None,
        help=('Path to configuration file. See the file comment '
              'at the top of this file for the configuration '
              'file format'))
    parser.add_argument('-H',
                        '--server-host',
                        '--server_host',
                        dest='server_host',
                        default='',
                        help='server hostname to listen to')
    parser.add_argument('-V',
                        '--validation-host',
                        '--validation_host',
                        dest='validation_host',
                        default=None,
                        help='server hostname to validate in absolute path.')
    parser.add_argument('-p',
                        '--port',
                        dest='port',
                        type=int,
                        default=common.DEFAULT_WEB_SOCKET_PORT,
                        help='port to listen to')
    parser.add_argument('-P',
                        '--validation-port',
                        '--validation_port',
                        dest='validation_port',
                        type=int,
                        default=None,
                        help='server port to validate in absolute path.')
    parser.add_argument(
        '-w',
        '--websock-handlers',
        '--websock_handlers',
        dest='websock_handlers',
        default='.',
        help=('The root directory of WebSocket handler files. '
              'If the path is relative, --document-root is used '
              'as the base.'))
    parser.add_argument('-m',
                        '--websock-handlers-map-file',
                        '--websock_handlers_map_file',
                        dest='websock_handlers_map_file',
                        default=None,
                        help=('WebSocket handlers map file. '
                              'Each line consists of alias_resource_path and '
                              'existing_resource_path, separated by spaces.'))
    parser.add_argument('-s',
                        '--scan-dir',
                        '--scan_dir',
                        dest='scan_dir',
                        default=None,
                        help=('Must be a directory under --websock-handlers. '
                              'Only handlers under this directory are scanned '
                              'and registered to the server. '
                              'Useful for saving scan time when the handler '
                              'root directory contains lots of files that are '
                              'not handler file or are handler files but you '
                              'don\'t want them to be registered. '))
    parser.add_argument(
        '--allow-handlers-outside-root-dir',
        '--allow_handlers_outside_root_dir',
        dest='allow_handlers_outside_root_dir',
        action='store_true',
        default=False,
        help=('Scans WebSocket handlers even if their canonical '
              'path is not under --websock-handlers.'))
    parser.add_argument('-d',
                        '--document-root',
                        '--document_root',
                        dest='document_root',
                        default='.',
                        help='Document root directory.')
    parser.add_argument('-x',
                        '--cgi-paths',
                        '--cgi_paths',
                        dest='cgi_paths',
                        default=None,
                        help=('CGI paths relative to document_root.'
                              'Comma-separated. (e.g -x /cgi,/htbin) '
                              'Files under document_root/cgi_path are handled '
                              'as CGI programs. Must be executable.'))
    parser.add_argument('-t',
                        '--tls',
                        dest='use_tls',
                        action='store_true',
                        default=False,
                        help='use TLS (wss://)')
    parser.add_argument('-k',
                        '--private-key',
                        '--private_key',
                        dest='private_key',
                        default='',
                        help='TLS private key file.')
    parser.add_argument('-c',
                        '--certificate',
                        dest='certificate',
                        default='',
                        help='TLS certificate file.')
    parser.add_argument('--tls-client-auth',
                        dest='tls_client_auth',
                        action='store_true',
                        default=False,
                        help='Requests TLS client auth on every connection.')
    parser.add_argument('--tls-client-cert-optional',
                        dest='tls_client_cert_optional',
                        action='store_true',
                        default=False,
                        help=('Makes client certificate optional even though '
                              'TLS client auth is enabled.'))
    parser.add_argument('--tls-client-ca',
                        dest='tls_client_ca',
                        default='',
                        help=('Specifies a pem file which contains a set of '
                              'concatenated CA certificates which are used to '
                              'validate certificates passed from clients'))
    parser.add_argument('--basic-auth',
                        dest='use_basic_auth',
                        action='store_true',
                        default=False,
                        help='Requires Basic authentication.')
    parser.add_argument(
        '--basic-auth-credential',
        dest='basic_auth_credential',
        default='test:test',
        help='Specifies the credential of basic authentication '
        'by username:password pair (e.g. test:test).')
    parser.add_argument('-l',
                        '--log-file',
                        '--log_file',
                        dest='log_file',
                        default='',
                        help='Log file.')
    # Custom log level:
    # - FINE: Prints status of each frame processing step
    parser.add_argument('--log-level',
                        '--log_level',
                        type=six.text_type,
                        dest='log_level',
                        default='warn',
                        choices=[
                            'fine', 'debug', 'info', 'warning', 'warn',
                            'error', 'critical'
                        ],
                        help='Log level.')
    parser.add_argument(
        '--deflate-log-level',
        '--deflate_log_level',
        type=six.text_type,
        dest='deflate_log_level',
        default='warn',
        choices=['debug', 'info', 'warning', 'warn', 'error', 'critical'],
        help='Log level for _Deflater and _Inflater.')
    parser.add_argument('--thread-monitor-interval-in-sec',
                        '--thread_monitor_interval_in_sec',
                        dest='thread_monitor_interval_in_sec',
                        type=int,
                        default=-1,
                        help=('If positive integer is specified, run a thread '
                              'monitor to show the status of server threads '
                              'periodically in the specified inteval in '
                              'second. If non-positive integer is specified, '
                              'disable the thread monitor.'))
    parser.add_argument('--log-max',
                        '--log_max',
                        dest='log_max',
                        type=int,
                        default=_DEFAULT_LOG_MAX_BYTES,
                        help='Log maximum bytes')
    parser.add_argument('--log-count',
                        '--log_count',
                        dest='log_count',
                        type=int,
                        default=_DEFAULT_LOG_BACKUP_COUNT,
                        help='Log backup count')
    parser.add_argument('-q',
                        '--queue',
                        dest='request_queue_size',
                        type=int,
                        default=_DEFAULT_REQUEST_QUEUE_SIZE,
                        help='request queue size')
    parser.add_argument(
        '--handler-encoding',
        '--handler_encoding',
        dest='handler_encoding',
        type=six.text_type,
        default=None,
        help=('Text encoding used for loading handlers. '
              'By default, the encoding from the locale is used when '
              'reading handler files, but this option can override it. '
              'Any encoding supported by the codecs module may be used.'))

    return parser


def _parse_args_and_config(args):
    parser = _build_option_parser()

    # First, parse options without configuration file.
    temporary_options, temporary_args = parser.parse_known_args(args=args)
    if temporary_args:
        logging.critical('Unrecognized positional arguments: %r',
                         temporary_args)
        sys.exit(1)

    if temporary_options.config_file:
        try:
            config_fp = open(temporary_options.config_file, 'r')
        except IOError as e:
            logging.critical('Failed to open configuration file %r: %r',
                             temporary_options.config_file, e)
            sys.exit(1)

        config_parser = configparser.SafeConfigParser()
        config_parser.readfp(config_fp)
        config_fp.close()

        args_from_config = []
        for name, value in config_parser.items('pywebsocket'):
            args_from_config.append('--' + name)
            args_from_config.append(value)
        if args is None:
            args = args_from_config
        else:
            args = args_from_config + args
        return parser.parse_known_args(args=args)
    else:
        return temporary_options, temporary_args


def _main(args=None):
    """You can call this function from your own program, but please note that
    this function has some side-effects that might affect your program. For
    example, it changes the current directory.
    """

    options, args = _parse_args_and_config(args=args)

    os.chdir(options.document_root)

    server_util.configure_logging(options)

    # TODO(tyoshino): Clean up initialization of CGI related values. Move some
    # of code here to WebSocketRequestHandler class if it's better.
    options.cgi_directories = []
    options.is_executable_method = None
    if options.cgi_paths:
        options.cgi_directories = options.cgi_paths.split(',')
        if sys.platform in ('cygwin', 'win32'):
            cygwin_path = None
            # For Win32 Python, it is expected that CYGWIN_PATH
            # is set to a directory of cygwin binaries.
            # For example, websocket_server.py in Chromium sets CYGWIN_PATH to
            # full path of third_party/cygwin/bin.
            if 'CYGWIN_PATH' in os.environ:
                cygwin_path = os.environ['CYGWIN_PATH']

            def __check_script(scriptpath):
                return util.get_script_interp(scriptpath, cygwin_path)

            options.is_executable_method = __check_script

    if options.use_tls:
        logging.debug('Using ssl module')

        if not options.private_key or not options.certificate:
            logging.critical(
                'To use TLS, specify private_key and certificate.')
            sys.exit(1)

        if (options.tls_client_cert_optional and not options.tls_client_auth):
            logging.critical('Client authentication must be enabled to '
                             'specify tls_client_cert_optional')
            sys.exit(1)
    else:
        if options.tls_client_auth:
            logging.critical('TLS must be enabled for client authentication.')
            sys.exit(1)

        if options.tls_client_cert_optional:
            logging.critical('TLS must be enabled for client authentication.')
            sys.exit(1)

    if not options.scan_dir:
        options.scan_dir = options.websock_handlers

    if options.use_basic_auth:
        options.basic_auth_credential = 'Basic ' + base64.b64encode(
            options.basic_auth_credential.encode('UTF-8')).decode()

    try:
        if options.thread_monitor_interval_in_sec > 0:
            # Run a thread monitor to show the status of server threads for
            # debugging.
            server_util.ThreadMonitor(
                options.thread_monitor_interval_in_sec).start()

        server = WebSocketServer(options)
        server.serve_forever()
    except Exception as e:
        logging.critical('pywebsocket3: %s' % e)
        logging.critical('pywebsocket3: %s' % traceback.format_exc())
        sys.exit(1)


if __name__ == '__main__':
    _main(sys.argv[1:])

# vi:sts=4 sw=4 et
