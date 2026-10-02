/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! LibTest's LiveDisplay: a few lines at the bottom of the terminal that are redrawn in place while the tests run,
//! with standard output and standard error redirected into a log file meanwhile.

use std::ffi::CString;
use std::fs::File;
use std::io::{IsTerminal, Write};
use std::os::fd::{FromRawFd, RawFd};

use crate::standard_output;

pub fn stdout_is_tty() -> bool {
    std::io::stdout().is_terminal()
}

fn query_terminal_width(fd: RawFd) -> usize {
    const FALLBACK: usize = 80;
    // SAFETY: An all-zero winsize is valid, and ioctl only writes into it.
    let mut size: libc::winsize = unsafe { core::mem::zeroed() };
    // SAFETY: TIOCGWINSZ writes a winsize into the struct, which outlives the call.
    if unsafe { libc::ioctl(fd, libc::TIOCGWINSZ, &raw mut size) } < 0 || size.ws_col == 0 {
        return FALLBACK;
    }
    usize::from(size.ws_col)
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Color {
    None,
    Red,
    Green,
    Yellow,
    Gray,
}

#[derive(Clone, Copy)]
pub struct LabelColor {
    pub prefix: Color,
    pub text: Color,
}

pub struct Counter {
    pub label: &'static str,
    pub color: Color,
    pub value: u32,
}

fn ansi_on(color: Color) -> &'static str {
    match color {
        Color::None => "",
        Color::Red => "\x1b[31m",
        Color::Green => "\x1b[32m",
        Color::Yellow => "\x1b[33m",
        Color::Gray => "\x1b[90m",
    }
}

fn ansi_bold_on(color: Color) -> &'static str {
    match color {
        Color::None => "\x1b[1m",
        Color::Red => "\x1b[1;31m",
        Color::Green => "\x1b[1;32m",
        Color::Yellow => "\x1b[1;33m",
        Color::Gray => "\x1b[1;90m",
    }
}

fn ansi_reset(color: Color) -> &'static str {
    if color == Color::None { "" } else { "\x1b[0m" }
}

pub struct RenderTarget<'builder> {
    builder: &'builder mut Vec<u8>,
    terminal_width: usize,
}

impl RenderTarget<'_> {
    pub fn line(&mut self, callback: impl FnOnce(&mut Self)) {
        self.builder.extend_from_slice(b"\x1b[2K");
        callback(self);
        self.builder.push(b'\n');
    }

    pub fn label(&mut self, prefix: &str, text: &[u8], color: LabelColor) {
        self.builder.extend_from_slice(ansi_on(color.prefix).as_bytes());
        self.builder.extend_from_slice(prefix.as_bytes());
        self.builder.extend_from_slice(ansi_reset(color.prefix).as_bytes());

        let available = if self.terminal_width > prefix.len() {
            self.terminal_width - prefix.len()
        } else {
            10
        };

        self.builder.extend_from_slice(ansi_on(color.text).as_bytes());
        if text.len() > available && available > 3 {
            self.builder.extend_from_slice(b"...");
            self.builder.extend_from_slice(&text[text.len() - available + 3..]);
        } else {
            self.builder.extend_from_slice(text);
        }
        self.builder.extend_from_slice(ansi_reset(color.text).as_bytes());
    }

    pub fn counter(&mut self, counters: &[Counter]) {
        for (index, counter) in counters.iter().enumerate() {
            if index != 0 {
                self.builder.extend_from_slice(b", ");
            }
            self.builder.extend_from_slice(ansi_bold_on(counter.color).as_bytes());
            self.builder.extend_from_slice(format!("{}:", counter.label).as_bytes());
            self.builder.extend_from_slice(b"\x1b[0m");
            self.builder.extend_from_slice(format!(" {}", counter.value).as_bytes());
        }
    }

    pub fn progress_bar(&mut self, completed: usize, total: usize) {
        let counter_begin = self.builder.len();
        self.builder
            .extend_from_slice(format!("{completed}/{total} ").as_bytes());
        let counter_length = self.builder.len() - counter_begin;
        let bar_width = if self.terminal_width > counter_length + 3 {
            self.terminal_width - counter_length - 3
        } else {
            20
        };
        let filled = (completed * bar_width).checked_div(total).unwrap_or(0);
        let empty = bar_width.saturating_sub(filled);

        self.builder.extend_from_slice(b"\x1b[32m[");
        for _ in 0..filled {
            self.builder.extend_from_slice("█".as_bytes());
        }
        if empty > 0 && filled < bar_width {
            self.builder.extend_from_slice("\x1b[33m▓\x1b[0m\x1b[90m".as_bytes());
            for _ in 1..empty {
                self.builder.extend_from_slice("░".as_bytes());
            }
        }
        self.builder.extend_from_slice(b"\x1b[32m]\x1b[0m");
    }
}

/// Where the display is drawn: the standard output, or the terminal it was on before it was redirected.
enum Output {
    StandardOutput,
    Terminal(File),
}

pub struct LiveDisplay {
    active: bool,
    reserved_lines: usize,
    terminal_width: usize,
    output: Option<Output>,
    saved_stdout_fd: RawFd,
    saved_stderr_fd: RawFd,
    log_file_path: String,
}

impl Default for LiveDisplay {
    fn default() -> Self {
        Self {
            active: false,
            reserved_lines: 0,
            terminal_width: 80,
            output: None,
            saved_stdout_fd: -1,
            saved_stderr_fd: -1,
            log_file_path: String::new(),
        }
    }
}

impl Drop for LiveDisplay {
    fn drop(&mut self) {
        self.end();
    }
}

impl LiveDisplay {
    /// Redirects the standard output and the standard error into the log file, if there is one, and reserves the
    /// lines of the display.
    pub fn begin(&mut self, reserved_lines: usize, log_file_path: String) -> bool {
        if self.active {
            return false;
        }

        self.reserved_lines = reserved_lines;
        self.log_file_path = log_file_path;

        if self.log_file_path.is_empty() {
            self.output = Some(Output::StandardOutput);
        } else {
            let Some(output) = self.redirect_standard_streams_into_log_file() else {
                return false;
            };
            self.output = Some(Output::Terminal(output));
        }

        self.refresh_terminal_width();

        self.write(&b"\n".repeat(self.reserved_lines));

        self.active = true;
        true
    }

    fn redirect_standard_streams_into_log_file(&mut self) -> Option<File> {
        let path = CString::new(self.log_file_path.as_bytes()).ok()?;
        // SAFETY: These calls only open and rearrange this process's descriptors, and the path is a valid C string.
        unsafe {
            let log_fd = libc::open(path.as_ptr(), libc::O_WRONLY | libc::O_CREAT | libc::O_TRUNC, 0o644);
            if log_fd < 0 {
                return None;
            }

            self.saved_stdout_fd = libc::dup(libc::STDOUT_FILENO);
            self.saved_stderr_fd = libc::dup(libc::STDERR_FILENO);
            if self.saved_stdout_fd < 0 || self.saved_stderr_fd < 0 {
                libc::close(log_fd);
                return None;
            }

            standard_output::flush();
            libc::dup2(log_fd, libc::STDOUT_FILENO);
            libc::dup2(log_fd, libc::STDERR_FILENO);
            libc::close(log_fd);

            let display_fd = libc::dup(self.saved_stdout_fd);
            if display_fd < 0 {
                return None;
            }
            Some(File::from_raw_fd(display_fd))
        }
    }

    /// Erases the display and puts the standard streams back.
    pub fn end(&mut self) {
        if !self.active {
            return;
        }

        self.clear();

        let redirected = self.saved_stdout_fd >= 0 || self.saved_stderr_fd >= 0;
        self.output = None;
        if redirected {
            standard_output::flush();
            // SAFETY: The saved descriptors belong to this display and are not used afterwards.
            unsafe {
                if self.saved_stdout_fd >= 0 {
                    libc::dup2(self.saved_stdout_fd, libc::STDOUT_FILENO);
                    libc::close(self.saved_stdout_fd);
                    self.saved_stdout_fd = -1;
                }
                if self.saved_stderr_fd >= 0 {
                    libc::dup2(self.saved_stderr_fd, libc::STDERR_FILENO);
                    libc::close(self.saved_stderr_fd);
                    self.saved_stderr_fd = -1;
                }
            }
        }
        self.active = false;
    }

    pub fn log_file_path(&self) -> &str {
        &self.log_file_path
    }

    fn refresh_terminal_width(&mut self) {
        let fd = match &self.output {
            Some(Output::Terminal(file)) => std::os::fd::AsRawFd::as_raw_fd(file),
            _ => libc::STDOUT_FILENO,
        };
        self.terminal_width = query_terminal_width(fd);
    }

    /// Erases the reserved display area, leaving the cursor at the top of it.
    fn clear(&mut self) {
        if !self.active || self.output.is_none() {
            return;
        }
        self.write(&b"\x1b[A\r\x1b[2K".repeat(self.reserved_lines));
    }

    pub fn render(&mut self, callback: impl FnOnce(&mut RenderTarget<'_>)) {
        if !self.active || self.output.is_none() {
            return;
        }

        self.refresh_terminal_width();

        let mut builder = b"\x1b[A".repeat(self.reserved_lines);
        builder.push(b'\r');

        let mut target = RenderTarget {
            builder: &mut builder,
            terminal_width: self.terminal_width,
        };
        callback(&mut target);

        self.write(&builder);
    }

    fn write(&mut self, data: &[u8]) {
        match &mut self.output {
            Some(Output::Terminal(file)) => {
                let _ = file.write_all(data);
                let _ = file.flush();
            }
            Some(Output::StandardOutput) => {
                standard_output::out(data);
                standard_output::flush();
            }
            None => {}
        }
    }
}
