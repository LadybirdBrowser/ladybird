/*
 * Copyright (c) 2025-2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// NOTE: All exceptions which are thrown by the URLPattern spec are TypeErrors which web-based callers are expected to assume.
//       If this ever does not become the case, this should change to also include the error type.
#[derive(Clone, Debug, Eq, PartialEq)]
pub struct ErrorInfo {
    pub message: String,
}

pub type PatternErrorOr<ValueT> = Result<ValueT, ErrorInfo>;

impl ErrorInfo {
    pub fn new(message: impl Into<String>) -> Self {
        Self {
            message: message.into(),
        }
    }
}
