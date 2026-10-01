/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::fmt;

use libjs_rust::compile::ParsedProgram;

/// An error the parser reports, as in Libraries/LibJS/ParserError.h.
#[derive(Clone, Debug)]
pub struct ParserError {
    pub message: String,
    pub line: u32,
    pub column: u32,
}

impl fmt::Display for ParserError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            formatter,
            "{} (line: {}, column: {})",
            self.message, self.line, self.column
        )
    }
}

impl ParserError {
    pub fn all_from_parsed_program(parsed: &ParsedProgram) -> Vec<Self> {
        parsed
            .errors()
            .iter()
            .map(|error| Self {
                message: error.message.clone(),
                line: error.line,
                column: error.column,
            })
            .collect()
    }
}
