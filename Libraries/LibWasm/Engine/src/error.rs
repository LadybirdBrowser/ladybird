/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::fmt;

/// Why a module was rejected: its bytes do not follow the binary format, or it breaks a validation rule.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Error {
    Malformed(String),
    Invalid(String),
}

pub type Result<T> = std::result::Result<T, Error>;

impl Error {
    pub fn malformed(message: impl Into<String>) -> Self {
        Self::Malformed(message.into())
    }

    pub fn invalid(message: impl Into<String>) -> Self {
        Self::Invalid(message.into())
    }

    pub fn message(&self) -> &str {
        match self {
            Self::Malformed(message) | Self::Invalid(message) => message,
        }
    }

    pub fn is_malformed(&self) -> bool {
        matches!(self, Self::Malformed(_))
    }
}

impl fmt::Display for Error {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::Malformed(message) => write!(formatter, "malformed module: {message}"),
            Self::Invalid(message) => write!(formatter, "invalid module: {message}"),
        }
    }
}

impl std::error::Error for Error {}
