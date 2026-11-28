/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Diagnostic output that never panics, like AK's outln(), warn() and warnln().
//!
//! The standard library's print!, println!, eprint! and eprintln! panic when the write fails, and a process loses its
//! stderr all the time: A browser launched from a terminal that has since closed gets EIO, and one whose stderr is a
//! pipe gets EPIPE once the reader exits. These macros drop a failed write instead, so a log line can never take the
//! process down.

use std::fmt;
use std::io::Write;

#[doc(hidden)]
pub fn write_to_stdout(arguments: fmt::Arguments<'_>) {
    let _ = std::io::stdout().write_fmt(arguments);
}

#[doc(hidden)]
pub fn write_to_stderr(arguments: fmt::Arguments<'_>) {
    let _ = std::io::stderr().write_fmt(arguments);
}

/// Writes a line to stdout, and drops it if the write fails.
#[macro_export]
macro_rules! outln {
    () => {
        $crate::format::write_to_stdout(::std::format_args!("\n"))
    };
    ($($arg:tt)*) => {
        $crate::format::write_to_stdout(::std::format_args!("{}\n", ::std::format_args!($($arg)*)))
    };
}

/// Writes to stderr, and drops the output if the write fails.
#[macro_export]
macro_rules! warn {
    ($($arg:tt)*) => {
        $crate::format::write_to_stderr(::std::format_args!($($arg)*))
    };
}

/// Writes a line to stderr, and drops it if the write fails.
#[macro_export]
macro_rules! warnln {
    () => {
        $crate::format::write_to_stderr(::std::format_args!("\n"))
    };
    ($($arg:tt)*) => {
        $crate::format::write_to_stderr(::std::format_args!("{}\n", ::std::format_args!($($arg)*)))
    };
}

#[cfg(test)]
mod tests {
    use std::process::{Command, Stdio};

    const CHILD_ENVIRONMENT_VARIABLE: &str = "AK_FORMAT_WRITES_TO_A_CLOSED_STDERR";

    // Runs this test again in a child process whose stderr is a pipe with no reader, so every write to it fails with
    // EPIPE (Rust ignores SIGPIPE). The child succeeds only if no macro panicked. Its stdout stays usable, since the
    // test harness reports there.
    #[test]
    fn macros_drop_writes_to_a_closed_stderr() {
        if std::env::var_os(CHILD_ENVIRONMENT_VARIABLE).is_some() {
            crate::warn!("warn {}", 1);
            crate::warnln!("warnln {}", 2);
            crate::warnln!();
            return;
        }

        let (stderr_reader, stderr_writer) = std::io::pipe().expect("a pipe for stderr");
        drop(stderr_reader);

        let status = Command::new(std::env::current_exe().expect("the test binary's path"))
            .args([
                "--exact",
                "format::tests::macros_drop_writes_to_a_closed_stderr",
                "--nocapture",
                "--quiet",
            ])
            .env(CHILD_ENVIRONMENT_VARIABLE, "1")
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(stderr_writer)
            .status()
            .expect("the child test to run");
        assert!(status.success(), "the child test failed: {status}");
    }
}
