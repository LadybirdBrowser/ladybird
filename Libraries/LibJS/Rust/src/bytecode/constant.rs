/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Constants whose value depends on the VM, so codegen records only which one it needs.

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
