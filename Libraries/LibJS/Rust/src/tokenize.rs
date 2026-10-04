/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Tokenizing a source text without parsing it, as syntax highlighters and a REPL's completion do.

use crate::lexer::Lexer;
use crate::token::TokenCategory;
use crate::token::TokenType;

/// A token, with the trivia (whitespace and comments) before it. Offsets and lengths count UTF-16 code units.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SourceToken {
    pub token_type: TokenType,
    pub category: TokenCategory,
    pub offset: u32,
    pub length: u32,
    pub trivia_offset: u32,
    pub trivia_length: u32,
}

/// The tokens of `source`, ending with its end-of-file token, whose trivia is whatever follows the last token.
pub fn tokenize(source: &[u16]) -> SourceTokens<'_> {
    SourceTokens {
        lexer: Lexer::new(source, 1, 0),
        reached_end_of_file: false,
    }
}

pub struct SourceTokens<'a> {
    lexer: Lexer<'a>,
    reached_end_of_file: bool,
}

impl Iterator for SourceTokens<'_> {
    type Item = SourceToken;

    fn next(&mut self) -> Option<SourceToken> {
        if self.reached_end_of_file {
            return None;
        }
        let token = self.lexer.next();
        self.reached_end_of_file = token.token_type == TokenType::Eof;
        Some(SourceToken {
            token_type: token.token_type,
            category: token.token_type.category(),
            offset: token.value_start,
            length: token.value_len,
            trivia_offset: token.trivia_start,
            trivia_length: token.trivia_len,
        })
    }
}
