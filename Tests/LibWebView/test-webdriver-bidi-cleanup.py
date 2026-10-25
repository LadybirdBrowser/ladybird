#!/usr/bin/env python3
#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

import importlib
import subprocess
import sys
import unittest

from unittest.mock import Mock
from unittest.mock import patch

bidi_helpers = importlib.import_module("test-webdriver-bidi")


class CleanupTests(unittest.TestCase):
    def resources(self):
        server, driver, connection, bidi = Mock(), Mock(), Mock(), Mock()
        driver.wait.side_effect = [subprocess.TimeoutExpired("WebDriver", 5), 0]
        server.shutdown.side_effect = OSError("server shutdown failed")
        bidi.close.side_effect = OSError("WebSocket close failed")
        connection.close.side_effect = OSError("HTTP close failed")
        return server, driver, connection, bidi

    def assert_cleaned(self, server, driver, connection, bidi):
        server.release_response.set.assert_called_once()
        bidi.close.assert_called_once()
        connection.close.assert_called_once()
        driver.terminate.assert_called_once()
        driver.kill.assert_called_once()
        self.assertEqual(driver.wait.call_count, 2)
        server.shutdown.assert_called_once()
        server.server_close.assert_called_once()

    def test_cleanup_failures_preserve_primary_error_and_release_every_resource(self):
        resources = self.resources()
        primary = AssertionError("original test failure")
        with patch.object(bidi_helpers.helpers, "request", side_effect=OSError("DELETE failed")) as delete:
            with self.assertRaises(AssertionError) as raised:
                try:
                    raise primary
                finally:
                    bidi_helpers.cleanup_resources(*resources, "session", primary_error=sys.exc_info()[1])
            self.assertIs(raised.exception, primary)
            delete.assert_called_once()
        self.assert_cleaned(*resources)

    def test_cleanup_without_primary_error_reports_failure_after_releasing_resources(self):
        resources = self.resources()
        failure = OSError("DELETE failed")
        with patch.object(bidi_helpers.helpers, "request", side_effect=failure):
            with self.assertRaises(OSError) as raised:
                bidi_helpers.cleanup_resources(*resources, "session")
            self.assertIs(raised.exception, failure)
        self.assert_cleaned(*resources)

    def test_termination_failure_still_waits_and_kills(self):
        resources = self.resources()
        resources[1].terminate.side_effect = OSError("terminate failed")
        with patch.object(bidi_helpers.helpers, "request", side_effect=OSError("DELETE failed")):
            bidi_helpers.cleanup_resources(*resources, "session", primary_error=AssertionError("primary"))
        self.assert_cleaned(*resources)

    def test_unexpected_wait_failure_still_kills_and_waits(self):
        resources = self.resources()
        resources[1].wait.side_effect = [OSError("wait failed"), 0]
        with patch.object(bidi_helpers.helpers, "request", side_effect=OSError("DELETE failed")):
            bidi_helpers.cleanup_resources(*resources, "session", primary_error=AssertionError("primary"))
        self.assert_cleaned(*resources)

    def test_reader_close_failure_does_not_skip_socket_close(self):
        connection = object.__new__(bidi_helpers.BiDiConnection)
        connection.reader = Mock()
        connection.socket = Mock()
        connection.reader.close.side_effect = OSError("reader close failed")
        with self.assertRaises(OSError):
            connection.close()
        connection.socket.close.assert_called_once()


if __name__ == "__main__":
    unittest.main()
