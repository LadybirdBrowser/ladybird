/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Target descriptions and textual assembly backends.

pub(crate) mod aarch64;
pub(crate) mod allocator;
pub(crate) mod backend;
pub(crate) mod description;
pub(crate) mod emitter;
pub(crate) mod finalize;
pub(crate) mod finalize_support;
pub(crate) mod ir;
pub(crate) mod machine_verify;
pub(crate) mod registers;
pub(crate) mod selection;
pub(crate) mod verify;
pub(crate) mod x86_64;
