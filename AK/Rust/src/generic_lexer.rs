/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub struct GenericLexer<'a, T> {
    input: &'a [T],
    index: usize,
}

impl<'a, T: Copy + PartialEq> GenericLexer<'a, T> {
    pub fn new(input: &'a [T]) -> Self {
        Self { input, index: 0 }
    }

    pub fn is_eof(&self) -> bool {
        self.index >= self.input.len()
    }

    pub fn peek(&self) -> Option<T> {
        self.input.get(self.index).copied()
    }

    pub fn consume(&mut self) -> Option<T> {
        let item = self.peek()?;
        self.index += 1;
        Some(item)
    }

    pub fn consume_specific(&mut self, item: T) -> bool {
        if self.peek() != Some(item) {
            return false;
        }
        self.index += 1;
        true
    }

    pub fn consume_while(&mut self, mut predicate: impl FnMut(T) -> bool) -> &'a [T] {
        let start = self.index;
        while self.peek().is_some_and(&mut predicate) {
            self.index += 1;
        }
        &self.input[start..self.index]
    }

    pub fn ignore(&mut self, count: usize) {
        self.index = self.input.len().min(self.index + count);
    }

    pub fn ignore_while(&mut self, predicate: impl FnMut(T) -> bool) {
        self.consume_while(predicate);
    }

    pub fn ignore_until(&mut self, stop: T) {
        self.consume_while(|item| item != stop);
    }
}
