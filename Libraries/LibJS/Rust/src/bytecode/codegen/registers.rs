/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the emission helpers take as operands.

use crate::bytecode::operand::Operand;

/// An operand, whose register belongs to whoever is making use of it.
#[derive(Clone, Debug, PartialEq)]
pub struct ScopedOperand {
    operand: Operand,
}

impl ScopedOperand {
    #[inline(always)]
    pub(super) fn plain(operand: Operand) -> Self {
        Self { operand }
    }

    #[inline(always)]
    pub(super) fn operand(&self) -> Operand {
        self.operand
    }
}
