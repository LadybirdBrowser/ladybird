/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The sides of a transaction that its readers compare.

#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum TransactionFactSide {
    Before,
    After,
}
