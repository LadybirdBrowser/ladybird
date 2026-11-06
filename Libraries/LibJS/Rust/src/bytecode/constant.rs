/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Constants whose value depends on the VM, so codegen records only which one it needs.

/// Well-known symbol IDs that the runtime resolves when materializing an Executable.
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WellKnownSymbolKind {
    SymbolIterator = 0,
    SymbolAsyncIterator = 1,
}

/// NativeJavaScriptBackedFunction intrinsic IDs that the runtime resolves when materializing an Executable.
#[repr(u8)]
#[derive(Debug, Clone, Copy)]
pub enum AbstractOperationKind {
    AsyncIteratorClose = 0,
    GetMethod = 1,
    GetIteratorDirect = 2,
    GetIteratorFromMethod = 3,
    IteratorComplete = 4,
    ArraySpeciesCreate = 5,
}
