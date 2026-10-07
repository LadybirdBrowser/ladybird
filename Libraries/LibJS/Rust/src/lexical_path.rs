/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The parts of AK::LexicalPath that module loading resolves specifiers with.

/// The parts of a path between its slashes, without the empty ones, like AK's split_view('/').
fn parts_of(path: &str) -> impl Iterator<Item = &str> {
    path.split('/').filter(|part| !part.is_empty())
}

/// LexicalPath::canonicalized_path()
pub fn canonicalized_path(path: &str) -> String {
    // NOTE: We never allow an empty m_string, if it's empty, we just set it to '.'.
    if path.is_empty() {
        return ".".to_string();
    }

    // NOTE: If there are no dots, no '//' and the path doesn't end with a slash, it is already canonical.
    if !path.contains('.') && !path.contains("//") && !path.ends_with('/') {
        return path.to_string();
    }

    let is_absolute = path.starts_with('/');
    let mut canonical_parts: Vec<&str> = Vec::new();

    for part in parts_of(path) {
        if part == "." {
            continue;
        }
        if part == ".." {
            match canonical_parts.last() {
                // At the root, .. does nothing.
                None if is_absolute => continue,
                // A .. and a previous non-.. part cancel each other.
                Some(&last) if last != ".." => {
                    canonical_parts.pop();
                    continue;
                }
                _ => {}
            }
        }
        canonical_parts.push(part);
    }

    if canonical_parts.is_empty() && !is_absolute {
        canonical_parts.push(".");
    }

    let mut canonical = String::new();
    if is_absolute {
        canonical.push('/');
    }
    canonical.push_str(&canonical_parts.join("/"));
    canonical
}

/// LexicalPath::is_absolute_path()
pub fn is_absolute_path(path: &str) -> bool {
    path.starts_with('/')
}

/// LexicalPath::join(first, rest).string()
pub fn join(first: &str, rest: &str) -> String {
    canonicalized_path(&format!("{first}/{rest}"))
}

/// LexicalPath::absolute_path()
pub fn absolute_path(dir_path: &str, target: &str) -> String {
    if is_absolute_path(target) {
        return canonicalized_path(target);
    }

    canonicalized_path(&join(dir_path, target))
}

/// LexicalPath(path).dirname()
pub fn dirname(path: &str) -> String {
    let canonical = canonicalized_path(path);
    match canonical.rfind('/') {
        // The path contains a single part and is not absolute. m_dirname = "."sv
        None => ".".to_string(),
        // The path contains a single part and is absolute. m_dirname = "/"sv
        Some(0) => "/".to_string(),
        Some(last_slash_index) => canonical[..last_slash_index].to_string(),
    }
}
