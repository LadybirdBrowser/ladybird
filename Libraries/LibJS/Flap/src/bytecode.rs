/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Prepared bytecode metadata used by reusable handler lowering.

use crate::metadata::{InstructionDefinition, Specialization, SpecializationConstraint};
use crate::types::Type;
use std::collections::{HashMap, HashSet};

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
        op: Option<&InstructionDefinition>,
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

pub(crate) fn validate_specializations(
    ops: &[InstructionDefinition],
    specializations: &[Specialization],
) -> Result<(), String> {
    let ops = ops.iter().map(|op| (op.name.as_str(), op)).collect::<HashMap<_, _>>();
    let mut names = ops.keys().copied().map(str::to_string).collect::<HashSet<_>>();
    for specialization in specializations {
        for (component_index, component) in specialization.components.iter().enumerate() {
            let op = ops
                .get(component.bytecode.as_str())
                .ok_or_else(|| format!("specialization references unknown bytecode '{}'", component.bytecode))?;
            if component_index + 1 != specialization.components.len() && op.is_terminator {
                return Err(format!(
                    "terminating bytecode '{}' may only be the final specialization component",
                    component.bytecode
                ));
            }
            let mut parameters = HashSet::new();
            for parameter in &component.parameters {
                if !parameters.insert(parameter.name.as_str()) {
                    return Err(format!(
                        "specialization repeats parameter '{}.{}'",
                        component.bytecode, parameter.name
                    ));
                }
                let field_name = format!("m_{}", parameter.name);
                let field = op.fields.iter().find(|field| field.name == field_name).ok_or_else(|| {
                    format!(
                        "specialization parameter '{}.{}' does not name a bytecode field",
                        component.bytecode, parameter.name
                    )
                })?;
                match parameter.constraint {
                    SpecializationConstraint::Int32 | SpecializationConstraint::Undefined => {
                        if field.ty != "Operand" || field.is_array {
                            return Err(format!(
                                "constant constraint requires an Operand field, but '{}.{}' is {}",
                                component.bytecode, parameter.name, field.ty
                            ));
                        }
                    }
                }
            }
        }
        let name = crate::metadata::specialization_name(specialization);
        if !names.insert(name.clone()) {
            return Err(format!("duplicate specialization name '{name}'"));
        }
    }
    Ok(())
}

fn bytecode_field_name(name: &str) -> String {
    name.strip_prefix("m_")
        .map_or_else(|| format!("m_{name}"), |_| name.to_string())
}

fn bytecode_field_type_name(field: &crate::metadata::Field) -> String {
    if field.is_array {
        format!("{}[]", field.ty)
    } else {
        field.ty.clone()
    }
}

fn bytecode_field_type_matches(field: &crate::metadata::Field, parameter_type: &Type) -> bool {
    if field.is_array {
        return match field.ty.as_str() {
            "Operand" | "Optional<Operand>" => *parameter_type == Type::Sequence(Box::new(Type::U32)),
            "Value" => *parameter_type == Type::Sequence(Box::new(Type::Value)),
            _ => false,
        };
    }
    match field.ty.as_str() {
        "bool" => *parameter_type == Type::Bool || *parameter_type == Type::U8,
        "u32"
        | "Completion::Type"
        | "IteratorHint"
        | "EnvironmentMode"
        | "ArgumentsKind"
        | "FunctionNamePrefix"
        | "IdentifierTableIndex"
        | "ObjectPropertyIteratorCacheIndex"
        | "ObjectShapeCacheIndex"
        | "PropertyKeyTableIndex"
        | "RegexTableIndex"
        | "StringTableIndex"
        | "TemplateObjectCacheIndex"
        | "Optional<IdentifierTableIndex>"
        | "Optional<StringTableIndex>" => *parameter_type == Type::U32,
        "u64" => *parameter_type == Type::U64,
        "i32" => *parameter_type == Type::InlineInt32,
        "Operand" | "Optional<Operand>" => *parameter_type == Type::Operand,
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
        let mut op = crate::metadata::parse_flap_metadata(
            "test.flap",
            "handler Add(lhs: Operand, rhs: Operand) { dispatch_next; }\n",
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
        let op = crate::metadata::parse_flap_metadata(
            "test.flap",
            "handler Get(cache: PropertyKeyTableIndex) { dispatch_next; }\n",
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
    fn accepts_inline_int32_handler_parameters() {
        let op = crate::metadata::parse_flap_metadata("test.flap", "handler AddInt32(rhs: Int32) { dispatch_next; }\n")
            .unwrap()
            .remove(0);

        HandlerLayout::new("AddInt32", &["rhs".to_string()], &[Type::InlineInt32], Some(&op)).unwrap();
    }

    #[test]
    fn rejects_wide_put_kind_handler_parameters() {
        let op = crate::metadata::parse_flap_metadata("test.flap", "handler Put(kind: PutKind) { dispatch_next; }\n")
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
        let op = crate::metadata::parse_flap_metadata("test.flap", "handler Put(kind: PutKind) { dispatch_next; }\n")
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
    #[test]
    fn validates_pair_specialization_constraints() {
        let source = r#"
handler Increment(dst: inout Operand) { dispatch_next; }
handler JumpLessThan(lhs: Operand, rhs: Operand) @terminator { exit; }
specialize Increment() + JumpLessThan(rhs: Int32);
"#;
        let ops = crate::metadata::parse_flap_metadata("test.flap", source).unwrap();
        let specializations = crate::metadata::parse_specializations("test.flap", source).unwrap();

        validate_specializations(&ops, &specializations).unwrap();
    }

    #[test]
    fn rejects_duplicate_derived_specialization_names() {
        let source = r#"
handler Add(rhs: Operand) { dispatch_next; }
specialize Add(rhs: Int32);
specialize Add(rhs: Int32);
"#;
        let ops = crate::metadata::parse_flap_metadata("test.flap", source).unwrap();
        let specializations = crate::metadata::parse_specializations("test.flap", source).unwrap();
        let error = validate_specializations(&ops, &specializations).unwrap_err();

        assert_eq!(error, "duplicate specialization name 'AddRhsInt32'");
    }

    #[test]
    fn rejects_specialization_names_that_collide_with_bytecodes() {
        let source = r#"
handler Add(rhs: Operand) { dispatch_next; }
specialize Add(rhs: Int32) as Add;
"#;
        let ops = crate::metadata::parse_flap_metadata("test.flap", source).unwrap();
        let specializations = crate::metadata::parse_specializations("test.flap", source).unwrap();
        let error = validate_specializations(&ops, &specializations).unwrap_err();

        assert_eq!(error, "duplicate specialization name 'Add'");
    }
}
