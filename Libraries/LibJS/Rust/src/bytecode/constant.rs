/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Constants whose value depends on the VM, so codegen records only which one it needs, and the tags of the encoding
//! that the C++ runtime and the bytecode cache store constants in.

/// Well-known symbol IDs resolved by C++ when materializing an Executable.
#[repr(u8)]
#[derive(Debug, Clone, Copy)]
pub enum WellKnownSymbolKind {
    SymbolIterator = 0,
    SymbolAsyncIterator = 1,
}

/// NativeJavaScriptBackedFunction intrinsic IDs resolved by C++ when materializing an Executable.
#[repr(u8)]
#[derive(Debug, Clone, Copy)]
pub enum AbstractOperationKind {
    AsyncIteratorClose = 0,
    GetMethod = 1,
    GetIteratorDirect = 2,
    GetIteratorFromMethod = 3,
    IteratorComplete = 4,
}

/// Constant tags for the FFI constant buffer (ABI-compatible).
#[repr(u8)]
pub enum ConstantTag {
    Number = 0,
    BooleanTrue = 1,
    BooleanFalse = 2,
    Null = 3,
    Undefined = 4,
    Empty = 5,
    String = 6,
    BigInt = 7,
    WellKnownSymbol = 8,
    AbstractOperation = 9,
}
