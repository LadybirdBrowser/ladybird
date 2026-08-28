/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Shared Flap handler metadata parser and instruction layout computation.
//!
//! Used by both `Libraries/LibJS/Rust/build.rs` (bytecode codegen) and
//! `Libraries/LibJS/Flap` (interpreter codegen) to ensure a single source
//! of truth for instruction names, fields, offsets, and sizes.

use std::collections::HashMap;
use std::error::Error;
use std::fmt;

#[derive(Debug, Clone)]
pub struct Field {
    pub name: String,
    pub ty: String,
    pub is_array: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SpecializationConstraint {
    Int32,
    Undefined,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SpecializationParameter {
    pub name: String,
    pub constraint: SpecializationConstraint,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SpecializationComponent {
    pub bytecode: String,
    pub parameters: Vec<SpecializationParameter>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Specialization {
    pub components: Vec<SpecializationComponent>,
    pub name: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SpecializedParameterBinding {
    Field(String),
    Undefined,
}

#[derive(Debug, Clone)]
pub struct SpecializedComponentLayout {
    pub bytecode: String,
    pub parameters: HashMap<String, SpecializedParameterBinding>,
}

#[derive(Debug, Clone)]
pub struct SpecializedInstruction {
    pub definition: InstructionDefinition,
    pub components: Vec<SpecializedComponentLayout>,
}

pub fn specialization_name(specialization: &Specialization) -> String {
    if let Some(name) = &specialization.name {
        return name.clone();
    }
    let mut name = String::new();
    for component in &specialization.components {
        name.push_str(&component.bytecode);
        for parameter in &component.parameters {
            name.push_str(&parameter.name[..1].to_ascii_uppercase());
            name.push_str(&parameter.name[1..]);
            match parameter.constraint {
                SpecializationConstraint::Int32 => name.push_str("Int32"),
                SpecializationConstraint::Undefined => name.push_str("Undefined"),
            }
        }
    }
    name
}

#[derive(Debug, Clone)]
#[non_exhaustive]
pub struct InstructionDefinition {
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

pub fn parse_flap_metadata(source_name: &str, content: &str) -> Result<Vec<InstructionDefinition>, ParseError> {
    let mut ops = Vec::new();
    let mut op_names = HashMap::new();
    let lines = content.lines().collect::<Vec<_>>();
    let mut line_index = 0;
    while line_index < lines.len() {
        let raw_line = lines[line_index];
        let stripped = raw_line.split_once('#').map_or(raw_line, |(source, _)| source).trim();
        if !stripped.starts_with("handler ") {
            line_index += 1;
            continue;
        }

        let start_line = line_index + 1;
        let mut declaration = stripped.to_string();
        let mut parenthesis_depth = declaration.chars().fold(0i32, |depth, character| match character {
            '(' => depth + 1,
            ')' => depth - 1,
            _ => depth,
        });
        while parenthesis_depth > 0 {
            line_index += 1;
            let Some(next_line) = lines.get(line_index) else {
                return Err(ParseError::new(
                    source_name,
                    start_line,
                    1,
                    "unterminated handler parameter list",
                ));
            };
            let next_line = next_line
                .split_once('#')
                .map_or(*next_line, |(source, _)| source)
                .trim();
            declaration.push(' ');
            declaration.push_str(next_line);
            parenthesis_depth += next_line.chars().fold(0i32, |depth, character| match character {
                '(' => depth + 1,
                ')' => depth - 1,
                _ => depth,
            });
        }

        let declaration = declaration.strip_prefix("handler ").unwrap();
        let Some(open) = declaration.find('(') else {
            return Err(ParseError::new(source_name, start_line, 1, "handler is missing '('"));
        };
        let name = declaration[..open].trim();
        if !is_identifier(name) {
            return Err(ParseError::new(
                source_name,
                start_line,
                1,
                format!("invalid handler name '{name}'"),
            ));
        }
        if let Some(previous_line) = op_names.insert(name.to_string(), start_line) {
            return Err(ParseError::new(
                source_name,
                start_line,
                1,
                format!("duplicate handler '{name}' previously defined on line {previous_line}"),
            ));
        }

        let close = find_matching_parenthesis(declaration, open)
            .ok_or_else(|| ParseError::new(source_name, start_line, 1, "unterminated handler parameter list"))?;
        let fields = parse_handler_fields(source_name, start_line, &declaration[open + 1..close])?;
        let mut op = InstructionDefinition {
            name: name.to_string(),
            parent: "Instruction".to_string(),
            fields,
            is_terminator: declaration[close + 1..]
                .split_whitespace()
                .any(|token| token == "@terminator"),
            layout: OpLayout::default(),
            array: None,
        };
        validate_op(&mut op).map_err(|message| ParseError::new(source_name, start_line, 1, message))?;
        ops.push(op);
        line_index += 1;
    }
    Ok(ops)
}

fn find_matching_parenthesis(source: &str, open: usize) -> Option<usize> {
    let mut depth = 0i32;
    for (offset, character) in source[open..].char_indices() {
        match character {
            '(' => depth += 1,
            ')' => {
                depth -= 1;
                if depth == 0 {
                    return Some(open + offset);
                }
            }
            _ => {}
        }
    }
    None
}

fn parse_handler_fields(source_name: &str, line: usize, source: &str) -> Result<Vec<Field>, ParseError> {
    let mut fields = Vec::new();
    let mut names = std::collections::HashSet::new();
    let mut start = 0;
    let mut depth = 0i32;
    for (offset, character) in source.char_indices().chain([(source.len(), ',')]) {
        match character {
            '<' | '[' => depth += 1,
            '>' | ']' => depth -= 1,
            ',' if depth == 0 => {
                let parameter = source[start..offset].trim();
                start = offset + 1;
                if parameter.is_empty() {
                    continue;
                }
                let (name, ty) = parameter
                    .split_once(':')
                    .ok_or_else(|| ParseError::new(source_name, line, 1, "malformed handler parameter"))?;
                let name = name.trim();
                if !is_identifier(name) {
                    return Err(ParseError::new(
                        source_name,
                        line,
                        1,
                        format!("invalid handler parameter name '{name}'"),
                    ));
                }
                if !names.insert(name) {
                    return Err(ParseError::new(
                        source_name,
                        line,
                        1,
                        format!("duplicate handler parameter '{name}'"),
                    ));
                }
                let ty = ty.trim();
                let ty = ["inout ", "in ", "out "]
                    .into_iter()
                    .find_map(|mode| ty.strip_prefix(mode))
                    .unwrap_or(ty);
                let is_array = ty.ends_with("[]");
                let ty = ty.strip_suffix("[]").unwrap_or(ty);
                let ty = match ty {
                    "BytecodeOffset" => "Label",
                    "Int32" => "i32",
                    other => other,
                };
                if try_field_type_info(ty).is_none() {
                    return Err(ParseError::new(
                        source_name,
                        line,
                        1,
                        format!("unknown encoded handler parameter type '{ty}'"),
                    ));
                }
                fields.push(Field {
                    name: format!("m_{name}"),
                    ty: ty.to_string(),
                    is_array,
                });
            }
            _ => {}
        }
    }
    Ok(fields)
}

pub fn parse_specializations(source_name: &str, content: &str) -> Result<Vec<Specialization>, ParseError> {
    let mut specializations = Vec::new();
    let mut declaration = None::<(usize, String)>;
    for (line_index, raw_line) in content.lines().enumerate() {
        let line = line_index + 1;
        let stripped = raw_line.split_once('#').map_or(raw_line, |(source, _)| source).trim();
        if declaration.is_none() && stripped.starts_with("specialize ") {
            declaration = Some((line, stripped.to_string()));
        } else if let Some((_, source)) = declaration.as_mut() {
            source.push(' ');
            source.push_str(stripped);
        } else {
            continue;
        }
        if stripped.ends_with(';') {
            let (start_line, source) = declaration.take().unwrap();
            specializations.push(parse_specialization(source_name, start_line, &source)?);
        }
    }
    if let Some((line, _)) = declaration {
        return Err(ParseError::new(
            source_name,
            line,
            1,
            "unterminated specialization declaration",
        ));
    }
    Ok(specializations)
}

fn parse_specialization(source_name: &str, line: usize, source: &str) -> Result<Specialization, ParseError> {
    let tokens = tokenize_specialization(source_name, line, source)?;
    let mut parser = SpecializationParser {
        source_name,
        line,
        tokens: &tokens,
        index: 0,
    };
    parser.expect("specialize")?;
    let mut components = vec![parser.parse_component()?];
    while parser.consume("+") {
        components.push(parser.parse_component()?);
    }
    let name = if parser.consume("as") {
        Some(parser.identifier("specialization name")?.to_string())
    } else {
        None
    };
    parser.expect(";")?;
    if parser.index != tokens.len() {
        return Err(parser.error("unexpected token after specialization declaration"));
    }
    Ok(Specialization { components, name })
}

fn tokenize_specialization(source_name: &str, line: usize, source: &str) -> Result<Vec<String>, ParseError> {
    let mut tokens = Vec::new();
    let mut current = String::new();
    for character in source.chars() {
        if character.is_ascii_alphanumeric() || character == '_' {
            current.push(character);
            continue;
        }
        if !current.is_empty() {
            tokens.push(std::mem::take(&mut current));
        }
        if character.is_whitespace() {
            continue;
        }
        if matches!(character, '(' | ')' | ',' | ':' | '+' | ';') {
            tokens.push(character.to_string());
        } else {
            return Err(ParseError::new(
                source_name,
                line,
                1,
                format!("unexpected character '{character}' in specialization"),
            ));
        }
    }
    if !current.is_empty() {
        tokens.push(current);
    }
    Ok(tokens)
}

struct SpecializationParser<'a> {
    source_name: &'a str,
    line: usize,
    tokens: &'a [String],
    index: usize,
}

impl SpecializationParser<'_> {
    fn error(&self, message: impl Into<String>) -> ParseError {
        ParseError::new(self.source_name, self.line, 1, message)
    }

    fn current(&self) -> Option<&str> {
        self.tokens.get(self.index).map(String::as_str)
    }

    fn consume(&mut self, token: &str) -> bool {
        if self.current() == Some(token) {
            self.index += 1;
            true
        } else {
            false
        }
    }

    fn expect(&mut self, token: &str) -> Result<(), ParseError> {
        self.consume(token)
            .then_some(())
            .ok_or_else(|| self.error(format!("expected '{token}' in specialization")))
    }

    fn identifier(&mut self, description: &str) -> Result<&str, ParseError> {
        let Some(token) = self.current() else {
            return Err(self.error(format!("expected {description}")));
        };
        if !is_identifier(token) {
            return Err(self.error(format!("expected {description}")));
        }
        self.index += 1;
        Ok(&self.tokens[self.index - 1])
    }

    fn parse_component(&mut self) -> Result<SpecializationComponent, ParseError> {
        let bytecode = self.identifier("bytecode name")?.to_string();
        self.expect("(")?;
        let mut parameters = Vec::new();
        while !self.consume(")") {
            let name = self.identifier("parameter name")?.to_string();
            self.expect(":")?;
            let constraint = self.parse_constraint()?;
            parameters.push(SpecializationParameter { name, constraint });
            if self.consume(")") {
                break;
            }
            self.expect(",")?;
        }
        Ok(SpecializationComponent { bytecode, parameters })
    }

    fn parse_constraint(&mut self) -> Result<SpecializationConstraint, ParseError> {
        let spelling = self
            .current()
            .ok_or_else(|| self.error("expected specialization constraint"))?
            .to_string();
        self.index += 1;
        match spelling.as_str() {
            "Int32" => Ok(SpecializationConstraint::Int32),
            "Undefined" => Ok(SpecializationConstraint::Undefined),
            _ => Err(self.error(format!("unknown specialization constraint '{spelling}'"))),
        }
    }
}

fn try_field_type_info(ty: &str) -> Option<FieldType> {
    Some(
        match ty {
            "bool" => ("bool", 1, 1, "bool"),
            "i32" => ("i32", 4, 4, "i32"),
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
pub fn user_fields(op: &InstructionDefinition) -> Vec<&Field> {
    op.fields
        .iter()
        .filter(|f| f.name != "m_type" && f.name != "m_strict" && f.name != "m_length")
        .collect()
}

fn count_field_index(op: &InstructionDefinition, array_field: &Field) -> Option<usize> {
    let plural = format!("{}_count", array_field.name);
    let singular = array_field.name.strip_suffix('s').map(|name| format!("{name}_count"));
    op.fields.iter().position(|field| {
        !field.is_array
            && field.ty == "u32"
            && (field.name == plural || singular.as_ref().is_some_and(|name| field.name == *name))
    })
}

fn validate_op(op: &mut InstructionDefinition) -> Result<(), String> {
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
pub fn compute_layouts(ops: &[InstructionDefinition]) -> HashMap<String, OpLayout> {
    ops.iter().map(|op| (op.name.clone(), op.layout.clone())).collect()
}

pub fn derive_specialized_instructions(
    ops: &[InstructionDefinition],
    specializations: &[Specialization],
) -> Result<Vec<SpecializedInstruction>, String> {
    let ops_by_name = ops.iter().map(|op| (op.name.as_str(), op)).collect::<HashMap<_, _>>();
    let mut derived = Vec::with_capacity(specializations.len());

    for specialization in specializations {
        let name = specialization_name(specialization);
        let mut fields = Vec::new();
        let mut components = Vec::with_capacity(specialization.components.len());
        let is_fused = specialization.components.len() > 1;
        for (component_index, component) in specialization.components.iter().enumerate() {
            let op = ops_by_name
                .get(component.bytecode.as_str())
                .ok_or_else(|| format!("specialization references unknown bytecode '{}'", component.bytecode))?;
            let constraints = component
                .parameters
                .iter()
                .map(|parameter| (parameter.name.as_str(), &parameter.constraint))
                .collect::<HashMap<_, _>>();
            let mut parameters = HashMap::new();

            for field in user_fields(op) {
                let source_name = field.name.strip_prefix("m_").unwrap_or(&field.name);
                let constraint = constraints.get(source_name).copied();
                let binding = match constraint {
                    Some(SpecializationConstraint::Undefined) => SpecializedParameterBinding::Undefined,
                    constraint => {
                        let specialized_name = if is_fused {
                            format!("c{component_index}_{source_name}")
                        } else {
                            source_name.to_string()
                        };
                        let ty = match constraint {
                            Some(SpecializationConstraint::Int32) => "i32",
                            _ => field.ty.as_str(),
                        };
                        fields.push(Field {
                            name: format!("m_{specialized_name}"),
                            ty: ty.to_string(),
                            is_array: field.is_array,
                        });
                        SpecializedParameterBinding::Field(specialized_name)
                    }
                };
                parameters.insert(source_name.to_string(), binding);
            }
            components.push(SpecializedComponentLayout {
                bytecode: component.bytecode.clone(),
                parameters,
            });
        }

        let last_component = specialization
            .components
            .last()
            .expect("specializations contain at least one component");
        let mut definition = InstructionDefinition {
            name,
            parent: "Instruction".to_string(),
            fields,
            is_terminator: ops_by_name[last_component.bytecode.as_str()].is_terminator,
            layout: OpLayout::default(),
            array: None,
        };
        validate_op(&mut definition)?;
        derived.push(SpecializedInstruction { definition, components });
    }

    Ok(derived)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_operations_fields_and_annotations() {
        let ops = parse_flap_metadata(
            "interpreter.flap",
            r#"
handler Call(
    length: u32,
    argument_count: u32,
    arguments: Operand[]
) @terminator = call_slow_path(call);
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
    fn ignores_comments_in_handler_declarations() {
        let ops = parse_flap_metadata(
            "test.flap",
            r#"
handler First(
    value: Value # Ignore unmatched ( and @terminator.
) # This is not an @terminator annotation.
handler Second() { dispatch_next; }
"#,
        )
        .unwrap();

        assert_eq!(ops.len(), 2);
        assert!(!ops[0].is_terminator);
    }

    #[test]
    fn parses_current_interpreter_handlers() {
        let ops = parse_flap_metadata(
            "Libraries/LibJS/Interpreter/interpreter.flap",
            include_str!("../../Interpreter/interpreter.flap"),
        )
        .unwrap();

        assert!(ops.len() > 100);
    }

    #[test]
    fn parses_specializations_of_arbitrary_length() {
        let specializations = parse_specializations(
            "test.flap",
            r#"
specialize Add(rhs: Int32);
specialize Increment()
    + JumpLessThan(rhs: Int32);
specialize Mov() + Add(rhs: Undefined)
    + JumpIf(condition: Int32) as HotLoop;
specialize A() + B() + C() + D();
"#,
        )
        .unwrap();

        assert_eq!(specializations.len(), 4);
        assert_eq!(specializations[1].components.len(), 2);
        assert_eq!(
            specializations[1].components[1].parameters[0].constraint,
            SpecializationConstraint::Int32
        );
        assert_eq!(specializations[2].components.len(), 3);
        assert_eq!(specializations[2].name.as_deref(), Some("HotLoop"));
        assert_eq!(specializations[3].components.len(), 4);
    }

    #[test]
    fn ignores_comments_in_multiline_specializations() {
        let specializations = parse_specializations(
            "test.flap",
            r#"
specialize Increment() # Continue with the comparison.
    + JumpLessThan( # Only the right-hand side is constant.
        rhs: Int32); # End the declaration.
"#,
        )
        .unwrap();

        assert_eq!(specializations.len(), 1);
        assert_eq!(specializations[0].components.len(), 2);
    }

    #[test]
    fn derives_compact_single_and_fused_instruction_layouts() {
        let source = r#"
handler Add(dst: out Operand, lhs: in Operand, rhs: in Operand) {
    dispatch_next;
}
handler Increment(dst: inout Operand) {
    dispatch_next;
}
handler Mov(dst: out Operand, src: in Operand) {
    dispatch_next;
}
handler JumpLessThan(lhs: in Operand, rhs: in Operand, target: Label) @terminator {
    dispatch_next;
}
specialize Add(rhs: Int32);
specialize Mov(src: Undefined);
specialize Increment() + JumpLessThan(rhs: Int32);
"#;
        let ops = parse_flap_metadata("test.flap", source).unwrap();
        let specializations = parse_specializations("test.flap", source).unwrap();
        let derived = derive_specialized_instructions(&ops, &specializations).unwrap();

        assert_eq!(derived[0].definition.name, "AddRhsInt32");
        assert_eq!(
            derived[0]
                .definition
                .fields
                .iter()
                .map(|field| (field.name.as_str(), field.ty.as_str()))
                .collect::<Vec<_>>(),
            [("m_dst", "Operand"), ("m_lhs", "Operand"), ("m_rhs", "i32")]
        );
        assert_eq!(derived[1].definition.name, "MovSrcUndefined");
        assert_eq!(
            derived[1]
                .definition
                .fields
                .iter()
                .map(|field| (field.name.as_str(), field.ty.as_str()))
                .collect::<Vec<_>>(),
            [("m_dst", "Operand")]
        );
        assert_eq!(
            derived[1].components[0].parameters["src"],
            SpecializedParameterBinding::Undefined
        );
        assert_eq!(derived[2].definition.name, "IncrementJumpLessThanRhsInt32");
        assert_eq!(
            derived[2]
                .definition
                .fields
                .iter()
                .map(|field| (field.name.as_str(), field.ty.as_str()))
                .collect::<Vec<_>>(),
            [
                ("m_c0_dst", "Operand"),
                ("m_c1_lhs", "Operand"),
                ("m_c1_rhs", "i32"),
                ("m_c1_target", "Label"),
            ]
        );
        assert!(derived[2].definition.is_terminator);
        assert_eq!(
            derived[2].components[1].parameters["lhs"],
            SpecializedParameterBinding::Field("c1_lhs".to_string())
        );
    }

    #[test]
    fn qualifies_field_names_across_fused_components() {
        let source = r#"
handler Copy(dst: out Operand) { dispatch_next; }
handler Convert(dst: out Operand) { dispatch_next; }
handler Use(dst1: in Operand) { dispatch_next; }
specialize Copy() + Convert() + Use();
"#;
        let ops = parse_flap_metadata("test.flap", source).unwrap();
        let specializations = parse_specializations("test.flap", source).unwrap();
        let derived = derive_specialized_instructions(&ops, &specializations).unwrap();

        assert_eq!(
            derived[0]
                .definition
                .fields
                .iter()
                .map(|field| field.name.as_str())
                .collect::<Vec<_>>(),
            ["m_c0_dst", "m_c1_dst", "m_c2_dst1"]
        );
        assert_eq!(
            derived[0].components[2].parameters["dst1"],
            SpecializedParameterBinding::Field("c2_dst1".to_string())
        );
    }

    #[test]
    fn rejects_invalid_specialization_declarations() {
        for (source, message) in [
            ("specialize Add(rhs: Mystery);", "unknown specialization constraint"),
            ("specialize Add(rhs: Int32)", "unterminated specialization declaration"),
        ] {
            let error = parse_specializations("broken.flap", source).unwrap_err();
            assert!(error.message.contains(message), "expected '{message}', got '{error}'");
        }
    }

    #[test]
    fn reports_structured_parse_errors() {
        let error = parse_flap_metadata("broken.flap", "handler Add(value: Mystery) { dispatch_next; }\n").unwrap_err();

        assert_eq!(error.source_name, "broken.flap");
        assert_eq!(error.line, 1);
        assert_eq!(error.column, 1);
        assert_eq!(error.message, "unknown encoded handler parameter type 'Mystery'");
        assert_eq!(
            error.to_string(),
            "broken.flap:1:1: error: unknown encoded handler parameter type 'Mystery'"
        );
    }

    #[test]
    fn parses_single_byte_mutations_without_panicking() {
        let seed = b"handler Add(value: Value, length: u32) { dispatch_next; }\n";
        for index in 0..seed.len() {
            for replacement in *b" \n<:[]0a" {
                let mut mutated = seed.to_vec();
                mutated[index] = replacement;
                let source = std::str::from_utf8(&mutated).unwrap();
                let _ = parse_flap_metadata("mutated.flap", source);
            }
        }
    }

    #[test]
    fn rejects_malformed_definitions() {
        for (source, message) in [
            ("handler Open(\n", "unterminated handler parameter list"),
            ("handler Missing { dispatch_next; }\n", "missing '('"),
            ("handler 42() { dispatch_next; }\n", "invalid handler name"),
            (
                "handler Bad(42: u32) { dispatch_next; }\n",
                "invalid handler parameter name",
            ),
            (
                "handler First() { dispatch_next; }\nhandler First() { dispatch_next; }\n",
                "duplicate handler 'First'",
            ),
            (
                "handler Fields(value: u32, value: u64) { dispatch_next; }\n",
                "duplicate handler parameter 'value'",
            ),
            (
                "handler Array(values: Value[]) { dispatch_next; }\n",
                "requires a u32 m_length field",
            ),
            (
                "handler Array(length: u32, values: Value[]) { dispatch_next; }\n",
                "requires a matching u32 count field",
            ),
            (
                "handler Array(length: u32, value_count: u32, values: Value[], tail: u32) { dispatch_next; }\n",
                "must be last",
            ),
        ] {
            let error = parse_flap_metadata("broken.flap", source).unwrap_err();
            assert!(
                error.message.contains(message),
                "expected '{message}', got '{}'",
                error.message
            );
        }
    }
}
