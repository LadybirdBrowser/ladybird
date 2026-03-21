# -*- coding: utf-8 -*-
"""
hyperframe/flags
~~~~~~~~~~~~~~~~

Defines basic Flag and Flags data structures.
"""
from collections.abc import MutableSet
from typing import NamedTuple, Iterable, Set, Iterator


class Flag(NamedTuple):
    name: str
    bit: int


class Flags(MutableSet):  # type: ignore
    """
    A simple MutableSet implementation that will only accept known flags as
    elements.

    Will behave like a regular set(), except that a ValueError will be thrown
    when .add()ing unexpected flags.
    """
    def __init__(self, defined_flags: Iterable[Flag]):
        self._valid_flags = set(flag.name for flag in defined_flags)
        self._flags: Set[str] = set()

    def __repr__(self) -> str:
        return repr(sorted(list(self._flags)))

    def __contains__(self, x: object) -> bool:
        return self._flags.__contains__(x)

    def __iter__(self) -> Iterator[str]:
        return self._flags.__iter__()

    def __len__(self) -> int:
        return self._flags.__len__()

    def discard(self, value: str) -> None:
        return self._flags.discard(value)

    def add(self, value: str) -> None:
        if value not in self._valid_flags:
            raise ValueError(
                "Unexpected flag: {}. Valid flags are: {}".format(
                    value, self._valid_flags
                )
            )
        return self._flags.add(value)
