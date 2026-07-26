/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Deterministic textual rendering for SSA diagnostics.

use super::{BlockLayout, Constant, Effects, Function, MemoryEffect, Operation, OperationValue, Terminator, ValueId};
use std::fmt::{self, Write};

pub(crate) fn function_to_string(function: &Function) -> String {
    let mut output = String::new();
    write!(&mut output, "function {}(", function.name).unwrap();
    for (index, (name, ty)) in function
        .parameter_names
        .iter()
        .zip(&function.parameter_types)
        .enumerate()
    {
        if index != 0 {
            output.push_str(", ");
        }
        write!(&mut output, "%{index} {name}: {ty:?}").unwrap();
    }
    output.push(')');
    if !function.return_types.is_empty() {
        output.push_str(" -> (");
        for (index, ty) in function.return_types.iter().enumerate() {
            if index != 0 {
                output.push_str(", ");
            }
            write!(&mut output, "{ty:?}").unwrap();
        }
        output.push(')');
    }
    output.push_str(" {\n");

    for (index, value) in function.values.iter().enumerate() {
        let super::ValueDefinition::Constant(constant) = &value.definition else {
            continue;
        };
        writeln!(
            &mut output,
            "  %{index}: {:?} = {}",
            value.ty,
            DisplayConstant(constant)
        )
        .unwrap();
    }

    for (block_index, block) in function.blocks.iter().enumerate() {
        write!(&mut output, "  bb{block_index}").unwrap();
        if let Some(name) = &block.name {
            write!(&mut output, " {name}").unwrap();
        }
        write!(&mut output, " [{}]", DisplayBlockLayout(block.layout)).unwrap();
        if !block.parameters.is_empty() {
            output.push('(');
            write_values_with_types(&mut output, function, &block.parameters);
            output.push(')');
        }
        output.push_str(":\n");

        for instruction_id in &block.instructions {
            let instruction = &function.instructions[instruction_id.0];
            output.push_str("    ");
            if !instruction.results.is_empty() {
                write_values_with_types(&mut output, function, &instruction.results);
                output.push_str(" = ");
            }
            write!(&mut output, "{}", DisplayOperation(&instruction.operation)).unwrap();
            write_values(&mut output, &instruction.inputs);
            write_effects(&mut output, instruction.effects);
            output.push('\n');
        }

        output.push_str("    ");
        match block.terminator.as_ref() {
            Some(terminator) => write!(&mut output, "{}", DisplayTerminator(terminator)).unwrap(),
            None => output.push_str("<missing terminator>"),
        }
        output.push('\n');
    }

    output.push_str("}\n");
    output
}

fn write_values(output: &mut String, values: &[ValueId]) {
    output.push('(');
    for (index, value) in values.iter().enumerate() {
        if index != 0 {
            output.push_str(", ");
        }
        write!(output, "%{}", value.0).unwrap();
    }
    output.push(')');
}

fn write_values_with_types(output: &mut String, function: &Function, values: &[ValueId]) {
    for (index, value) in values.iter().enumerate() {
        if index != 0 {
            output.push_str(", ");
        }
        write!(output, "%{}: {:?}", value.0, function.values[value.0].ty).unwrap();
    }
}

fn write_effects(output: &mut String, effects: Effects) {
    if effects == Effects::PURE {
        return;
    }
    write!(
        output,
        " [memory={}, machine-state={}, call={}, trap={}]",
        DisplayMemoryEffect(effects.memory),
        DisplayMemoryEffect(effects.machine_state),
        effects.may_call,
        effects.may_trap
    )
    .unwrap();
}

struct DisplayBlockLayout(BlockLayout);

impl fmt::Display for DisplayBlockLayout {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.0 {
            BlockLayout::Hot => formatter.write_str("hot"),
            BlockLayout::Preferred => formatter.write_str("preferred"),
            BlockLayout::Cold => formatter.write_str("cold"),
        }
    }
}

struct DisplayMemoryEffect(MemoryEffect);

impl fmt::Display for DisplayMemoryEffect {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.0 {
            MemoryEffect::None => formatter.write_str("none"),
            MemoryEffect::Read => formatter.write_str("read"),
            MemoryEffect::Write => formatter.write_str("write"),
            MemoryEffect::ReadWrite => formatter.write_str("read-write"),
        }
    }
}

struct DisplayConstant<'a>(&'a Constant);

impl fmt::Display for DisplayConstant<'_> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.0 {
            Constant::Integer(value) => write!(formatter, "{value}"),
            Constant::KnownLayout(value) => {
                write!(formatter, "known-layout {}", value.name())
            }
            Constant::LayoutValue(crate::frontend::layout::LayoutValue::Immediate(value)) => {
                write!(formatter, "layout-value {value}")
            }
            Constant::LayoutValue(crate::frontend::layout::LayoutValue::Constant(value)) => {
                write!(formatter, "layout-value #{} = {}", value.id().index(), value.value())
            }
            Constant::Symbol(value) => write!(formatter, "symbol {value:?}"),
            Constant::Layout(value) => {
                write!(formatter, "layout #{} = {}", value.id().index(), value.value())
            }
            Constant::SlowPath(value) => write!(formatter, "slow-path {value:?}"),
            Constant::FunctionSymbol(value) => write!(formatter, "function-symbol {value:?}"),
            Constant::Operation(OperationValue::Intrinsic(intrinsic)) => {
                write!(formatter, "operation {:?}", intrinsic.name())
            }
            Constant::Operation(OperationValue::InlineFunction(id)) => {
                write!(formatter, "inline-function #{}", id.index())
            }
            Constant::MachineRegister(value) => write!(formatter, "register {:?}", value.as_str()),
        }
    }
}

struct DisplayOperation<'a>(&'a Operation);

impl fmt::Display for DisplayOperation<'_> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.0 {
            Operation::Parameter(parameter) => write!(formatter, "operation-parameter {}", parameter.0),
            Operation::Intrinsic(intrinsic) => formatter.write_str(intrinsic.name()),
            Operation::FieldAccess(access) => write!(formatter, "field-access {access:?}"),
            Operation::InlineCall(id) => write!(formatter, "inline-call #{}", id.index()),
            Operation::MachineAssign { destination, intrinsic } => {
                write!(
                    formatter,
                    "machine-assign {} {}",
                    destination.as_str(),
                    intrinsic.name()
                )
            }
            Operation::BlockReference(block) => write!(formatter, "block-reference bb{}", block.0),
            Operation::Address => formatter.write_str("address"),
            Operation::Guard { failure } => write!(formatter, "guard else bb{}", failure.0),
        }
    }
}

struct DisplayTerminator<'a>(&'a Terminator);

impl fmt::Display for DisplayTerminator<'_> {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.0 {
            Terminator::Jump(edge) => write!(formatter, "jump bb{}{:?}", edge.block.0, edge.arguments),
            Terminator::Branch {
                condition,
                then_edge,
                else_edge,
            } => write!(
                formatter,
                "branch %{}, bb{}{:?}, bb{}{:?}",
                condition.0, then_edge.block.0, then_edge.arguments, else_edge.block.0, else_edge.arguments
            ),
            Terminator::Switch { value, cases, default } => {
                write!(formatter, "switch %{} [", value.0)?;
                for (index, (pattern, edge)) in cases.iter().enumerate() {
                    if index != 0 {
                        formatter.write_str(", ")?;
                    }
                    write!(formatter, "{pattern:?} => bb{}{:?}", edge.block.0, edge.arguments)?;
                }
                write!(formatter, "] default bb{}{:?}", default.block.0, default.arguments)
            }
            Terminator::CheckedOperation {
                operation,
                inputs,
                results,
                effects,
                success,
                failure,
            } => write!(
                formatter,
                "checked {}{:?} -> {:?}, bb{}{:?}, else bb{}{:?} {effects:?}",
                DisplayOperation(operation),
                inputs,
                results,
                success.block.0,
                success.arguments,
                failure.block.0,
                failure.arguments
            ),
            Terminator::IndirectJump { target } => write!(formatter, "indirect-jump %{}", target.0),
            Terminator::Return(values) => write!(formatter, "return {values:?}"),
            Terminator::Unreachable => formatter.write_str("unreachable"),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::types::Type;

    #[test]
    fn prints_ssa_deterministically() {
        let mut function = Function::new("identity", vec![Type::I32], vec![Type::I32]);
        function.set_parameter_name(0, "value");
        function.set_terminator(function.entry, Terminator::Return(vec![function.parameter(0)]));

        assert_eq!(
            function_to_string(&function),
            "function identity(%0 value: I32) -> (I32) {\n  bb0 entry [hot]:\n    return [ValueId(0)]\n}\n"
        );
    }
}
