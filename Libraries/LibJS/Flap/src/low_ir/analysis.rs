/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Shared machine-instruction use and definition classification.

use super::{ControlFlowOpcode, Instruction, Operand, VirtualRegister, visit_virtual_registers};
use crate::Architecture;
use crate::target::description::OperandKind;

#[derive(Debug, Default)]
pub(crate) struct VirtualRegisterAccess {
    pub(crate) reads: Vec<VirtualRegister>,
    pub(crate) definitions: Vec<VirtualRegister>,
}

impl VirtualRegisterAccess {
    fn read(&mut self, register: &VirtualRegister) {
        if !self.reads.contains(register) {
            self.reads.push(register.clone());
        }
    }

    fn define(&mut self, register: &VirtualRegister) {
        if !self.definitions.contains(register) {
            self.definitions.push(register.clone());
        }
    }
}

pub(crate) fn virtual_register_access(
    instruction: &Instruction<Operand, impl ControlFlowOpcode>,
    architecture: Option<Architecture>,
) -> VirtualRegisterAccess {
    let mut access = VirtualRegisterAccess::default();
    let description = instruction.opcode.description();
    let zeroes_register = description.zeroes_if_inputs_alias
        && instruction.operands.len() >= 2
        && instruction.operands[0] == instruction.operands[1];

    for (position, operand) in instruction.operands.iter().enumerate() {
        if position < 2 && zeroes_register {
            if position == 0
                && let Operand::VirtualRegister(register) = operand
            {
                access.define(register);
            }
            continue;
        }
        let kind = architecture.map_or_else(
            || description.operand_kind(position, instruction.operands.len()),
            |architecture| description.selected_operand_kind(position, instruction.operands.len(), architecture),
        );
        let Some(kind) = kind else {
            continue;
        };
        let reads = matches!(
            kind,
            OperandKind::GprIn
                | OperandKind::GprInOut
                | OperandKind::FprIn
                | OperandKind::FprInOut
                | OperandKind::RegisterIn
                | OperandKind::GprInOrImm
                | OperandKind::GprInOrMemory
                | OperandKind::GprInOrImmOrMemory
                | OperandKind::Memory
        );
        let defines = matches!(
            kind,
            OperandKind::GprOut
                | OperandKind::GprInOut
                | OperandKind::GprScratch
                | OperandKind::FprOut
                | OperandKind::FprInOut
                | OperandKind::FprScratch
                | OperandKind::RegisterOut
        );
        if reads {
            visit_virtual_registers(operand, &mut |register| access.read(register));
        }
        if defines && let Operand::VirtualRegister(register) = operand {
            access.define(register);
        }
    }
    access
}
