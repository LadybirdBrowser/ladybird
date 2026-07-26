/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Prepared bytecode metadata used by reusable handler lowering.

use crate::types::Type;
use bytecode_def::OpDef;

/// The identity of a handler parameter that names a bytecode field.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(crate) struct BytecodeFieldId(u32);

impl BytecodeFieldId {
    pub(crate) fn new(index: usize) -> Self {
        Self(u32::try_from(index).expect("Flap handler parameter count fits in u32"))
    }

    pub(crate) fn index(self) -> usize {
        self.0 as usize
    }
}

#[derive(Debug)]
struct FieldLayout {
    name: String,
    offset: Option<usize>,
}

/// Bytecode layout data aligned with one prepared handler's parameters.
#[derive(Debug)]
pub(crate) struct HandlerLayout {
    pub(crate) size: Option<usize>,
    fields: Vec<FieldLayout>,
}

impl HandlerLayout {
    pub(crate) fn new(
        handler_name: &str,
        parameter_names: &[String],
        parameter_types: &[Type],
        op: Option<&OpDef>,
    ) -> Result<Self, String> {
        if parameter_names.len() != parameter_types.len() {
            return Err(format!(
                "handler '{handler_name}' has {} parameter names but {} parameter types",
                parameter_names.len(),
                parameter_types.len()
            ));
        }
        let layout = op.map(|op| &op.layout);
        let fields = parameter_names
            .iter()
            .zip(parameter_types)
            .map(|(name, parameter_type)| {
                let name = bytecode_field_name(name);
                if let Some(op) = op {
                    let field = op
                        .fields
                        .iter()
                        .find(|field| field.name == name)
                        .ok_or_else(|| format!("handler parameter '{name}' does not name a bytecode field"))?;
                    if !bytecode_field_type_matches(field, parameter_type) {
                        return Err(format!(
                            "handler parameter '{name}' has type {parameter_type}, but bytecode field '{}.{name}' has type {}",
                            op.name,
                            bytecode_field_type_name(field),
                        ));
                    }
                }
                Ok(FieldLayout {
                    offset: layout.and_then(|layout| layout.field_offsets.get(&name)).copied(),
                    name,
                })
            })
            .collect::<Result<Vec<_>, _>>()?;
        Ok(Self {
            size: layout.and_then(|layout| layout.size),
            fields,
        })
    }

    pub(crate) fn field_name(&self, field: BytecodeFieldId) -> &str {
        &self.fields[field.index()].name
    }

    pub(crate) fn field_offset(&self, field: BytecodeFieldId) -> Option<usize> {
        self.fields[field.index()].offset
    }
}

fn bytecode_field_name(name: &str) -> String {
    name.strip_prefix("m_")
        .map_or_else(|| format!("m_{name}"), |_| name.to_string())
}

fn bytecode_field_type_name(field: &bytecode_def::Field) -> String {
    if field.is_array {
        format!("{}[]", field.ty)
    } else {
        field.ty.clone()
    }
}

fn bytecode_field_type_matches(field: &bytecode_def::Field, parameter_type: &Type) -> bool {
    if field.is_array {
        return field.ty == "Operand" && *parameter_type == Type::Sequence(Box::new(Type::U32));
    }
    match field.ty.as_str() {
        "bool" => *parameter_type == Type::Bool || *parameter_type == Type::U8,
        "u32" | "Completion::Type" | "IteratorHint" | "EnvironmentMode" | "ArgumentsKind" | "FunctionNamePrefix" => {
            *parameter_type == Type::U32
        }
        "u64" => *parameter_type == Type::U64,
        "Operand" => *parameter_type == Type::Operand,
        "Label" | "Optional<Label>" => *parameter_type == Type::BytecodeOffset,
        "EnvironmentCoordinate" => *parameter_type == Type::EnvironmentCoordinate,
        "EnvironmentCoordinateCacheIndex" => *parameter_type == Type::EnvironmentCoordinateCacheIndex,
        "GlobalVariableCacheIndex" => *parameter_type == Type::GlobalVariableCacheIndex,
        "PropertyLookupCacheIndex" => *parameter_type == Type::PropertyLookupCacheIndex,
        "PutKind" => *parameter_type == Type::U8,
        _ => false,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    #[test]
    fn aligns_layout_offsets_with_handler_parameters() {
        let mut op = bytecode_def::parse_bytecode_def(
            "TestBytecode.def",
            "op Add < Instruction\n    m_lhs: Operand\n    m_rhs: Operand\nendop\n",
        )
        .unwrap()
        .remove(0);
        op.layout.field_offsets = HashMap::from([("m_lhs".to_string(), 4), ("m_rhs".to_string(), 8)]);
        op.layout.size = Some(12);
        let prepared = HandlerLayout::new(
            "Add",
            &["lhs".to_string(), "m_rhs".to_string()],
            &[Type::Operand, Type::Operand],
            Some(&op),
        )
        .unwrap();

        assert_eq!(prepared.size, Some(12));
        assert_eq!(prepared.field_offset(BytecodeFieldId::new(0)), Some(4));
        assert_eq!(prepared.field_offset(BytecodeFieldId::new(1)), Some(8));
        assert_eq!(prepared.field_name(BytecodeFieldId::new(1)), "m_rhs");
    }

    #[test]
    fn rejects_unrelated_index_types_as_property_lookup_cache_indices() {
        let op = bytecode_def::parse_bytecode_def(
            "TestBytecode.def",
            "op Get < Instruction\n    m_cache: PropertyKeyTableIndex\nendop\n",
        )
        .unwrap()
        .remove(0);
        let error = HandlerLayout::new(
            "Get",
            &["cache".to_string()],
            &[Type::PropertyLookupCacheIndex],
            Some(&op),
        )
        .unwrap_err();

        assert!(
            error.contains(
                "handler parameter 'm_cache' has type PropertyLookupCacheIndex, but bytecode field 'Get.m_cache' has type PropertyKeyTableIndex"
            ),
            "{error}"
        );
    }

    #[test]
    fn rejects_wide_put_kind_handler_parameters() {
        let op =
            bytecode_def::parse_bytecode_def("TestBytecode.def", "op Put < Instruction\n    m_kind: PutKind\nendop\n")
                .unwrap()
                .remove(0);
        let error = HandlerLayout::new("Put", &["kind".to_string()], &[Type::U32], Some(&op)).unwrap_err();

        assert!(
            error.contains("handler parameter 'm_kind' has type u32, but bytecode field 'Put.m_kind' has type PutKind"),
            "{error}"
        );
    }

    #[test]
    fn rejects_mismatched_handler_parameter_metadata_before_field_types() {
        let op =
            bytecode_def::parse_bytecode_def("TestBytecode.def", "op Put < Instruction\n    m_kind: PutKind\nendop\n")
                .unwrap()
                .remove(0);
        let error = HandlerLayout::new(
            "Put",
            &["kind".to_string(), "extra".to_string()],
            &[Type::U32],
            Some(&op),
        )
        .unwrap_err();

        assert_eq!(error, "handler 'Put' has 2 parameter names but 1 parameter types");
    }
}
