/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The process's perf map, `/tmp/perf-PID.map`, which tells `perf` what generated code is where. Every code generator
//! in the process writes it through here.

use std::fs::OpenOptions;
use std::io::Write;

/// Appends a line for `size` bytes of code at `address` called `name` to the perf map.
///
/// NB: The line goes out in one write to a file opened for appending, so lines that threads write at the same time
///     never interleave, whichever copy of this crate they go through, and no lock is needed.
pub fn write_perf_map_entry(address: usize, size: usize, name: &str) -> std::io::Result<()> {
    let line = format!("{address:x} {size:x} {name}\n");
    let mut file = OpenOptions::new()
        .create(true)
        .append(true)
        .open(format!("/tmp/perf-{}.map", std::process::id()))?;
    let written = file.write(line.as_bytes())?;
    if written != line.len() {
        return Err(std::io::Error::new(
            std::io::ErrorKind::WriteZero,
            "a short write to the perf map",
        ));
    }
    Ok(())
}
