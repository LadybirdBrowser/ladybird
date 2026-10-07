/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::fmt;

use crate::compile::ParsedProgram;

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
    /// The line of `source` the error is on, followed by a line that points at its column: `column - 1` spacers and
    /// the indicator.
    pub fn source_location_hint(&self, source: &[u16], spacer: u8, indicator: u8) -> Vec<u16> {
        const LINE_FEED: u16 = b'\n' as u16;
        const CARRIAGE_RETURN: u16 = b'\r' as u16;
        const LINE_SEPARATOR: u16 = 0x2028;
        const PARAGRAPH_SEPARATOR: u16 = 0x2029;

        // We need to modify the source to match what the lexer considers one line - normalizing
        // line terminators to \n is easier than splitting using all different LT characters.
        let mut source_string = Vec::with_capacity(source.len());
        let mut index = 0;
        while index < source.len() {
            let code_unit = source[index];
            index += 1;
            if code_unit == CARRIAGE_RETURN && source.get(index) == Some(&LINE_FEED) {
                index += 1;
            }
            source_string.push(match code_unit {
                CARRIAGE_RETURN | LINE_SEPARATOR | PARAGRAPH_SEPARATOR => LINE_FEED,
                _ => code_unit,
            });
        }

        let line = source_string
            .split(|&code_unit| code_unit == LINE_FEED)
            .nth((self.line as usize).wrapping_sub(1))
            .expect("the error is on a line of the source");
        let mut builder = line.to_vec();
        builder.push(LINE_FEED);
        builder.extend(core::iter::repeat_n(
            u16::from(spacer),
            (self.column as usize).saturating_sub(1),
        ));
        builder.push(u16::from(indicator));
        builder
    }

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
