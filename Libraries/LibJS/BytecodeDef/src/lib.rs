/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Shared Bytecode.def parser and layout computation.
//!
//! Used by both `Libraries/LibJS/Rust/build.rs` (bytecode codegen) and
//! `Libraries/LibJS/Flap` (interpreter codegen) to ensure a single source
//! of truth for instruction field offsets and sizes.

use std::collections::HashMap;
use std::error::Error;
use std::fmt;

#[derive(Debug, Clone)]
pub struct Field {
    pub name: String,
    pub ty: String,
    pub is_array: bool,
}

#[derive(Debug, Clone)]
#[non_exhaustive]
pub struct OpDef {
    pub name: String,
    pub parent: String,
    pub fields: Vec<Field>,
    pub is_terminator: bool,
    pub layout: OpLayout,
    pub array: Option<ArrayLayout>,
}

#[derive(Debug, Clone)]
pub struct ArrayLayout {
    pub field_index: usize,
    pub count_field_index: usize,
    pub offset: usize,
    pub element_size: usize,
}

pub struct FieldType {
    pub rust_type: &'static str,
    pub align: usize,
    pub size: usize,
    pub kind: &'static str,
}

impl From<(&'static str, usize, usize, &'static str)> for FieldType {
    fn from(v: (&'static str, usize, usize, &'static str)) -> Self {
        Self {
            rust_type: v.0,
            align: v.1,
            size: v.2,
            kind: v.3,
        }
    }
}

/// The alignment of the C++ Instruction base class (`alignas(void*)`).
/// On 64-bit: alignof(void*) = 8.
pub const STRUCT_ALIGN: usize = 8;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ParseError {
    pub source_name: String,
    pub line: usize,
    pub column: usize,
    pub message: String,
}

impl ParseError {
    fn new(source_name: &str, line: usize, column: usize, message: impl Into<String>) -> Self {
        Self {
            source_name: source_name.to_string(),
            line,
            column,
            message: message.into(),
        }
    }
}

impl fmt::Display for ParseError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            formatter,
            "{}:{}:{}: error: {}",
            self.source_name, self.line, self.column, self.message
        )
    }
}

impl Error for ParseError {}

fn is_identifier(name: &str) -> bool {
    let mut characters = name.chars();
    characters
        .next()
        .is_some_and(|character| character.is_ascii_alphabetic() || character == '_')
        && characters.all(|character| character.is_ascii_alphanumeric() || character == '_')
}

pub fn parse_bytecode_def(source_name: &str, content: &str) -> Result<Vec<OpDef>, ParseError> {
    let mut ops = Vec::new();
    let mut current: Option<OpDef> = None;
    let mut op_names = HashMap::new();
    let mut field_names = HashMap::new();
    let mut op_start = None;

    for (line_index, raw_line) in content.lines().enumerate() {
        let line = line_index + 1;
        let stripped = raw_line.trim();
        let column = raw_line.len() - raw_line.trim_start().len() + 1;
        if stripped.is_empty() || stripped.starts_with("//") || stripped.starts_with('#') {
            continue;
        }

        if stripped.starts_with("op ") {
            if current.is_some() {
                return Err(ParseError::new(source_name, line, column, "nested op block"));
            }
            let declaration = stripped.strip_prefix("op ").unwrap().trim();
            let (name, parent) = if let Some((name, parent)) = declaration.split_once('<') {
                (name.trim(), Some(parent.trim()))
            } else {
                (declaration, None)
            };
            if name.is_empty() {
                return Err(ParseError::new(source_name, line, column, "missing op name"));
            }
            if !is_identifier(name) {
                return Err(ParseError::new(
                    source_name,
                    line,
                    column,
                    format!("invalid op name '{name}'"),
                ));
            }
            if parent.is_some_and(str::is_empty) {
                return Err(ParseError::new(source_name, line, column, "missing parent op name"));
            }
            if parent.is_some_and(|parent| !is_identifier(parent)) {
                return Err(ParseError::new(
                    source_name,
                    line,
                    column,
                    format!("invalid parent op name '{}'", parent.unwrap()),
                ));
            }
            if let Some(previous_line) = op_names.insert(name.to_string(), line) {
                return Err(ParseError::new(
                    source_name,
                    line,
                    column,
                    format!("duplicate op '{name}' previously defined on line {previous_line}"),
                ));
            }
            current = Some(OpDef {
                name: name.to_string(),
                parent: parent.unwrap_or_default().to_string(),
                fields: Vec::new(),
                is_terminator: false,
                layout: OpLayout::default(),
                array: None,
            });
            field_names.clear();
            op_start = Some((line, column));
            continue;
        }

        if stripped == "endop" {
            let Some(mut op) = current.take() else {
                return Err(ParseError::new(source_name, line, column, "endop without op"));
            };
            validate_op(&mut op).map_err(|message| ParseError::new(source_name, line, column, message))?;
            ops.push(op);
            op_start = None;
            continue;
        }

        let Some(op) = current.as_mut() else {
            return Err(ParseError::new(
                source_name,
                line,
                column,
                format!("unexpected top-level line '{stripped}'"),
            ));
        };

        if stripped.starts_with('@') {
            match stripped {
                "@terminator" => op.is_terminator = true,
                "@nothrow" => {}
                _ => {
                    return Err(ParseError::new(
                        source_name,
                        line,
                        column,
                        format!("unknown annotation '{stripped}'"),
                    ));
                }
            }
            continue;
        }

        let Some((lhs, rhs)) = stripped.split_once(':') else {
            return Err(ParseError::new(source_name, line, column, "malformed field line"));
        };
        let field_name = lhs.trim();
        if field_name.is_empty() {
            return Err(ParseError::new(source_name, line, column, "missing field name"));
        }
        if !is_identifier(field_name) {
            return Err(ParseError::new(
                source_name,
                line,
                column,
                format!("invalid field name '{field_name}'"),
            ));
        }
        if let Some(previous_line) = field_names.insert(field_name.to_string(), line) {
            return Err(ParseError::new(
                source_name,
                line,
                column,
                format!("duplicate field '{field_name}' previously defined on line {previous_line}"),
            ));
        }
        let mut field_type = rhs.trim();
        let is_array = field_type.ends_with("[]");
        if is_array {
            field_type = field_type[..field_type.len() - 2].trim();
        }
        if try_field_type_info(field_type).is_none() {
            let type_column = raw_line.find(':').unwrap() + 2 + rhs.len() - rhs.trim_start().len();
            return Err(ParseError::new(
                source_name,
                line,
                type_column,
                format!("unknown field type '{field_type}'"),
            ));
        }
        op.fields.push(Field {
            name: field_name.to_string(),
            ty: field_type.to_string(),
            is_array,
        });
    }
    if let Some(op) = current {
        let (line, column) = op_start.unwrap();
        return Err(ParseError::new(
            source_name,
            line,
            column,
            format!("unclosed op block '{}'", op.name),
        ));
    }

    // Remove the base "Instruction" definition (not an actual opcode).
    ops.retain(|op| op.name != "Instruction");
    Ok(ops)
}

fn try_field_type_info(ty: &str) -> Option<FieldType> {
    Some(
        match ty {
            "bool" => ("bool", 1, 1, "bool"),
            "u32" => ("u32", 4, 4, "u32"),
            "u64" => ("u64", 8, 8, "u64"),
            "Operand" => ("Operand", 4, 4, "operand"),
            "Optional<Operand>" => ("Option<Operand>", 4, 4, "optional_operand"),
            "Label" => ("Label", 4, 4, "label"),
            "Optional<Label>" => ("Option<Label>", 4, 8, "optional_label"),
            "IdentifierTableIndex" => ("IdentifierTableIndex", 4, 4, "u32_newtype"),
            "Optional<IdentifierTableIndex>" => ("Option<IdentifierTableIndex>", 4, 4, "optional_u32_newtype"),
            "PropertyKeyTableIndex" => ("PropertyKeyTableIndex", 4, 4, "u32_newtype"),
            "StringTableIndex" => ("StringTableIndex", 4, 4, "u32_newtype"),
            "Optional<StringTableIndex>" => ("Option<StringTableIndex>", 4, 4, "optional_u32_newtype"),
            "RegexTableIndex" => ("RegexTableIndex", 4, 4, "u32_newtype"),
            "EnvironmentCoordinate" => ("EnvironmentCoordinate", 4, 8, "env_coord"),
            "Builtin" => ("u8", 1, 1, "u8"),
            "Completion::Type" => ("u32", 4, 4, "u32"),
            "IteratorHint" => ("u32", 4, 4, "u32"),
            "EnvironmentMode" => ("u32", 4, 4, "u32"),
            "PutKind" => ("u32", 4, 4, "u32"),
            "ArgumentsKind" => ("u32", 4, 4, "u32"),
            "FunctionNamePrefix" => ("u32", 4, 4, "u32"),
            "Value" => ("u64", 8, 8, "u64"),
            "PropertyLookupCacheIndex"
            | "GlobalVariableCacheIndex"
            | "EnvironmentCoordinateCacheIndex"
            | "TemplateObjectCacheIndex"
            | "ObjectShapeCacheIndex"
            | "ObjectPropertyIteratorCacheIndex" => ("u32", 4, 4, "u32"),
            _ => return None,
        }
        .into(),
    )
}

pub fn field_type_info(ty: &str) -> FieldType {
    try_field_type_info(ty).unwrap_or_else(|| panic!("Unknown field type: {ty}"))
}

pub fn round_up(value: usize, align: usize) -> usize {
    assert!(align.is_power_of_two());
    value.checked_add(align - 1).expect("layout size overflow") & !(align - 1)
}

/// Returns the user-visible fields (excludes m_type, m_strict, m_length).
pub fn user_fields(op: &OpDef) -> Vec<&Field> {
    op.fields
        .iter()
        .filter(|f| f.name != "m_type" && f.name != "m_strict" && f.name != "m_length")
        .collect()
}

fn count_field_index(op: &OpDef, array_field: &Field) -> Option<usize> {
    let plural = format!("{}_count", array_field.name);
    let singular = array_field.name.strip_suffix('s').map(|name| format!("{name}_count"));
    op.fields.iter().position(|field| {
        !field.is_array
            && field.ty == "u32"
            && (field.name == plural || singular.as_ref().is_some_and(|name| field.name == *name))
    })
}

fn validate_op(op: &mut OpDef) -> Result<(), String> {
    if op.name == "Instruction" {
        if !op.parent.is_empty() {
            return Err("base op 'Instruction' cannot have a parent".to_string());
        }
    } else if op.parent != "Instruction" {
        return Err(format!("op '{}' must derive directly from Instruction", op.name));
    }

    let mut offset = 2usize;
    let mut field_offsets = HashMap::new();
    let array_indices = op
        .fields
        .iter()
        .enumerate()
        .filter_map(|(index, field)| field.is_array.then_some(index))
        .collect::<Vec<_>>();
    if array_indices.len() > 1 {
        return Err(format!("op '{}' has more than one flexible array field", op.name));
    }
    if let Some(array_index) = array_indices.first()
        && *array_index + 1 != op.fields.len()
    {
        return Err(format!(
            "flexible array field '{}' in op '{}' must be last",
            op.fields[*array_index].name, op.name
        ));
    }

    for field in &op.fields {
        if field.is_array || field.name == "m_type" || field.name == "m_strict" {
            continue;
        }
        let info = try_field_type_info(&field.ty).expect("field types are checked while parsing");
        offset = offset
            .checked_add(info.align - 1)
            .map(|offset| offset & !(info.align - 1))
            .ok_or_else(|| format!("layout of op '{}' overflows usize", op.name))?;
        field_offsets.insert(field.name.clone(), offset);
        offset = offset
            .checked_add(info.size)
            .ok_or_else(|| format!("layout of op '{}' overflows usize", op.name))?;
    }

    let array = if let Some(array_index) = array_indices.first().copied() {
        let length_field = op
            .fields
            .iter()
            .position(|field| field.name == "m_length" && field.ty == "u32" && !field.is_array);
        if length_field.is_none() {
            return Err(format!("array op '{}' requires a u32 m_length field", op.name));
        }
        let count_field_index = count_field_index(op, &op.fields[array_index]).ok_or_else(|| {
            format!(
                "array field '{}' in op '{}' requires a matching u32 count field",
                op.fields[array_index].name, op.name
            )
        })?;
        let info = try_field_type_info(&op.fields[array_index].ty).expect("field types are checked while parsing");
        let array_offset = offset
            .checked_add(info.align - 1)
            .map(|offset| offset & !(info.align - 1))
            .ok_or_else(|| format!("layout of op '{}' overflows usize", op.name))?;
        field_offsets.insert(op.fields[array_index].name.clone(), array_offset);
        Some(ArrayLayout {
            field_index: array_index,
            count_field_index,
            offset: array_offset,
            element_size: info.size,
        })
    } else {
        None
    };
    let size = if array.is_some() {
        None
    } else {
        Some(
            offset
                .checked_add(STRUCT_ALIGN - 1)
                .map(|offset| offset & !(STRUCT_ALIGN - 1))
                .ok_or_else(|| format!("layout of op '{}' overflows usize", op.name))?,
        )
    };
    let minimum_size = offset
        .checked_add(STRUCT_ALIGN - 1)
        .map(|offset| offset & !(STRUCT_ALIGN - 1))
        .ok_or_else(|| format!("layout of op '{}' overflows usize", op.name))?;
    let m_length_offset = field_offsets.get("m_length").copied();
    op.layout = OpLayout {
        field_offsets,
        size,
        minimum_size,
        m_length_offset,
    };
    op.array = array;
    Ok(())
}

/// Computed layout info for a single opcode.
#[derive(Debug, Clone, Default)]
pub struct OpLayout {
    /// Byte offset of each field within the C++ struct (keyed by field name, e.g. "m_dst").
    pub field_offsets: HashMap<String, usize>,
    /// Total encoded size (for fixed-size instructions), or None for variable-length.
    pub size: Option<usize>,
    /// Minimum encoded size, including the fixed header and tail padding.
    pub minimum_size: usize,
    /// Byte offset of m_length for variable-length instructions.
    pub m_length_offset: Option<usize>,
}

/// Compute field offsets and total sizes for all opcodes.
pub fn compute_layouts(ops: &[OpDef]) -> HashMap<String, OpLayout> {
    ops.iter().map(|op| (op.name.clone(), op.layout.clone())).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_operations_fields_and_annotations() {
        let ops = parse_bytecode_def(
            "Bytecode.def",
            r#"
op Instruction
    m_type: bool
    m_strict: bool
endop

op Call < Instruction
    @terminator
    @nothrow
    m_length: u32
    m_argument_count: u32
    m_arguments: Operand[]
endop
"#,
        )
        .unwrap();

        assert_eq!(ops.len(), 1);
        assert_eq!(ops[0].name, "Call");
        assert!(ops[0].is_terminator);
        assert_eq!(ops[0].fields.len(), 3);
        assert!(ops[0].fields[2].is_array);
    }

    #[test]
    fn parses_current_bytecode_definition() {
        let ops = parse_bytecode_def(
            "Libraries/LibJS/Bytecode/Bytecode.def",
            include_str!("../../Bytecode/Bytecode.def"),
        )
        .unwrap();

        assert!(ops.len() > 100);
    }

    #[test]
    fn reports_structured_parse_errors() {
        let error = parse_bytecode_def("broken.def", "op Add\n    m_value: Mystery\nendop\n").unwrap_err();

        assert_eq!(error.source_name, "broken.def");
        assert_eq!(error.line, 2);
        assert_eq!(error.column, 14);
        assert_eq!(error.message, "unknown field type 'Mystery'");
        assert_eq!(
            error.to_string(),
            "broken.def:2:14: error: unknown field type 'Mystery'"
        );
    }

    #[test]
    fn parses_single_byte_mutations_without_panicking() {
        let seed = b"op Add < Instruction\n    m_value: Value\n    m_length: u32\nendop\n";
        for index in 0..seed.len() {
            for replacement in [b' ', b'\n', b'<', b':', b'[', b']', b'0', b'a'] {
                let mut mutated = seed.to_vec();
                mutated[index] = replacement;
                let source = std::str::from_utf8(&mutated).unwrap();
                let _ = parse_bytecode_def("mutated.def", source);
            }
        }
    }

    #[test]
    fn rejects_malformed_definitions() {
        for (source, message) in [
            ("unexpected\n", "unexpected top-level line"),
            ("endop\n", "endop without op"),
            ("op Outer\nop Inner\n", "nested op block"),
            ("op Open\n", "unclosed op block"),
            ("op Bad\n    @mystery\nendop\n", "unknown annotation"),
            ("op Bad\n    malformed\nendop\n", "malformed field line"),
            ("op 42\nendop\n", "invalid op name"),
            ("op Bad < Parent < Other\nendop\n", "invalid parent op name"),
            ("op Bad\n    42: u32\nendop\n", "invalid field name"),
            (
                "op First < Instruction\nendop\nop First < Instruction\nendop\n",
                "duplicate op 'First'",
            ),
            (
                "op Fields < Instruction\n    value: u32\n    value: u64\nendop\n",
                "duplicate field 'value'",
            ),
            (
                "op Array < Instruction\n    m_values: Value[]\nendop\n",
                "requires a u32 m_length field",
            ),
            (
                "op Array < Instruction\n    m_length: u32\n    m_values: Value[]\nendop\n",
                "requires a matching u32 count field",
            ),
            (
                "op Array < Instruction\n    m_length: u32\n    m_value_count: u32\n    m_values: Value[]\n    m_tail: u32\nendop\n",
                "must be last",
            ),
            ("op Derived < Mystery\nendop\n", "must derive directly from Instruction"),
        ] {
            let error = parse_bytecode_def("broken.def", source).unwrap_err();
            assert!(
                error.message.contains(message),
                "expected '{message}', got '{}'",
                error.message
            );
        }
    }
}
