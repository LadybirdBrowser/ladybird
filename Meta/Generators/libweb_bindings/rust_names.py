# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause

# The Rust names of IDL identifiers, for the Rust bindings of the interfaces implemented in Rust.

from Utils.utils import title_case_to_snake_case

RUST_KEYWORDS = {
    "as", "async", "await", "break", "const", "continue", "crate", "dyn", "else", "enum", "extern", "false", "fn", "for",
    "if", "impl", "in", "let", "loop", "match", "mod", "move", "mut", "pub", "ref", "return", "self", "static",
    "struct", "super", "trait", "true", "type", "unsafe", "use", "where", "while", "abstract", "become", "box", "do",
    "final", "gen", "macro", "override", "priv", "try", "typeof", "unsized", "virtual", "yield",
}  # fmt: skip

# Names that Rust does not accept as raw identifiers either.
RUST_RESERVED_NAMES = {"self", "Self", "crate", "super"}


def rust_identifier(name: str) -> str:
    identifier = title_case_to_snake_case(name).replace("-", "_")
    if identifier in RUST_RESERVED_NAMES:
        return f"{identifier}_"
    if identifier in RUST_KEYWORDS:
        return f"r#{identifier}"
    return identifier


def rust_local_name(name: str) -> str:
    """The local variable that holds the IDL value of a parameter, which never clashes with the generated ones."""
    return f"idl_{rust_identifier(name).removeprefix('r#')}"
