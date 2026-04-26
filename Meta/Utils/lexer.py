#!/usr/bin/env python3

# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause


from typing import Callable


class Lexer:
    def __init__(self, text: str) -> None:
        self.text = text
        self.position = 0

    def tell(self) -> int:
        return self.position

    def is_eof(self) -> bool:
        return self.position >= len(self.text)

    def peek(self, offset: int = 0) -> str:
        position = self.position + offset
        if position >= len(self.text):
            return "\0"
        return self.text[position]

    def consume(self) -> str:
        if self.is_eof():
            return "\0"
        ch = self.text[self.position]
        self.position += 1
        return ch

    def consume_specific(self, string: str) -> bool:
        if self.text.startswith(string, self.position):
            self.position += len(string)
            return True
        return False

    def next_is(self, string: str) -> bool:
        return self.text.startswith(string, self.position)

    def consume_until(self, predicate: Callable[[str], bool]) -> str:
        start = self.position
        while self.position < len(self.text) and not predicate(self.text[self.position]):
            self.position += 1
        return self.text[start : self.position]

    def consume_while(self, predicate: Callable[[str], bool]) -> str:
        start = self.position
        while self.position < len(self.text) and predicate(self.text[self.position]):
            self.position += 1
        return self.text[start : self.position]

    def ignore(self, count: int = 1) -> None:
        self.position = min(self.position + count, len(self.text))

    def ignore_until(self, ch: str) -> None:
        while self.position < len(self.text) and self.text[self.position] != ch:
            self.position += 1

    def ignore_while(self, predicate: Callable[[str], bool]) -> None:
        while self.position < len(self.text) and predicate(self.text[self.position]):
            self.position += 1
