/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::runtime_functions::{Runtime, RuntimeFunctions, SlowPathControl, handle_asm_exception};
use super::vm::Vm;
use crate::bytecode::op;

impl RuntimeFunctions for Runtime {
    fn throw(vm: &Vm, pc: u32, _instruction: &op::Throw, values: &mut op::ThrowValues) -> SlowPathControl {
        handle_asm_exception(vm, pc, values.src)
    }
}
