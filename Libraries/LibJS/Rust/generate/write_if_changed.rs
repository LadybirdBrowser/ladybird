/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::fs;
use std::path::Path;

/// Writes `contents` to `path` unless the file already holds them, so that what depends on the file is not rebuilt
/// for a write that changed nothing.
pub fn write_if_changed(path: &Path, contents: &str) {
    if fs::read_to_string(path).is_ok_and(|existing| existing == contents) {
        return;
    }
    fs::write(path, contents).unwrap_or_else(|error| panic!("write {}: {error}", path.display()));
}
