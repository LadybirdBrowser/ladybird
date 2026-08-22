/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ops::{Deref, DerefMut};

use super::component_value::ComponentValue;

#[derive(Clone)]
pub(crate) struct TokenStream<'a> {
    pub(crate) values: &'a [ComponentValue],
    pub(crate) position: usize,
    end_of_file: ComponentValue,
}

impl<'a> TokenStream<'a> {
    pub(crate) fn new(values: &'a [ComponentValue]) -> Self {
        Self {
            values,
            position: 0,
            end_of_file: ComponentValue::end_of_file(),
        }
    }

    // https://drafts.csswg.org/css-syntax-3/#token-stream-next-token
    pub(crate) fn next_token(&self) -> &ComponentValue {
        self.values.get(self.position).unwrap_or(&self.end_of_file)
    }

    // https://drafts.csswg.org/css-syntax-3/#token-stream-consume-a-token
    pub(crate) fn consume_a_token(&mut self) -> &ComponentValue {
        let position = self.position;
        self.position += 1;
        self.values.get(position).unwrap_or(&self.end_of_file)
    }

    // https://drafts.csswg.org/css-syntax-3/#token-stream-discard-a-token
    pub(crate) fn discard_a_token(&mut self) {
        if self.has_next_token() {
            self.position += 1;
        }
    }

    // https://drafts.csswg.org/css-syntax-3/#token-stream-discard-whitespace
    pub(crate) fn discard_whitespace(&mut self) {
        while self.next_token().is_whitespace() {
            self.discard_a_token();
        }
    }

    pub(crate) fn has_next_token(&self) -> bool {
        !self.is_empty()
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.next_token()
            .is(&crate::css::css_tokenizer::ParserTokenKind::EndOfFile)
    }

    pub(crate) fn begin_transaction(&mut self) -> StateTransaction<'_, 'a> {
        StateTransaction {
            saved_position: self.position,
            stream: self,
            committed: false,
        }
    }

    pub(crate) fn current_index(&self) -> usize {
        self.position
    }

    pub(crate) fn tokens_since(&self, start: usize) -> &[ComponentValue] {
        let end = self.position.min(self.values.len());
        if start > end {
            return &[];
        }
        &self.values[start..end]
    }

    // Compatibility names for parsers that use iterator-style terminology.
    pub(crate) fn peek(&self) -> Option<&'a ComponentValue> {
        self.values.get(self.position)
    }

    pub(crate) fn peek_n(&self, offset: usize) -> Option<&'a ComponentValue> {
        self.values.get(self.position.saturating_add(offset))
    }

    pub(crate) fn next(&mut self) -> Option<&'a ComponentValue> {
        let value = self.values.get(self.position)?;
        self.position += 1;
        Some(value)
    }
}

pub(crate) struct StateTransaction<'stream, 'values> {
    stream: &'stream mut TokenStream<'values>,
    saved_position: usize,
    committed: bool,
}

impl StateTransaction<'_, '_> {
    pub(crate) fn commit(&mut self) {
        self.committed = true;
    }
}

impl<'values> Deref for StateTransaction<'_, 'values> {
    type Target = TokenStream<'values>;

    fn deref(&self) -> &Self::Target {
        self.stream
    }
}

impl DerefMut for StateTransaction<'_, '_> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        self.stream
    }
}

impl Drop for StateTransaction<'_, '_> {
    fn drop(&mut self) {
        if !self.committed {
            self.stream.position = self.saved_position;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::TokenStream;
    use crate::css::css_tokenizer::{ParserString, ParserTokenKind, tokenize_for_parser};
    use crate::css::parser::component_value::consume_a_list_of_component_values;

    fn values(input: &str) -> Vec<crate::css::parser::component_value::ComponentValue> {
        consume_a_list_of_component_values(tokenize_for_parser(input.as_bytes())).unwrap()
    }

    #[test]
    fn uncommitted_transaction_rewinds_on_drop() {
        let values = values(" one two");
        let mut stream = TokenStream::new(&values);
        stream.discard_whitespace();
        let start = stream.current_index();
        {
            let mut transaction = stream.begin_transaction();
            transaction.consume_a_token();
            transaction.discard_whitespace();
            assert!(
                transaction
                    .next_token()
                    .is(&ParserTokenKind::Ident(ParserString::Owned(Box::new([]))))
            );
        }
        assert_eq!(stream.current_index(), start);
    }

    #[test]
    fn committed_transaction_keeps_position() {
        let values = values("one two");
        let mut stream = TokenStream::new(&values);
        {
            let mut transaction = stream.begin_transaction();
            transaction.discard_a_token();
            transaction.discard_whitespace();
            transaction.commit();
        }
        assert_eq!(stream.tokens_since(0).len(), 2);
    }
}
