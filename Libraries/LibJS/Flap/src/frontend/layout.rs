/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Validated database for generated LibJS data-layout descriptions.

use crate::types::Type;
use std::collections::{HashMap, HashSet};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct LayoutConstantId(u32);

impl LayoutConstantId {
    pub(crate) fn index(self) -> u32 {
        self.0
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum LayoutConstantCategory {
    Other,
    Tag,
    Value,
}

macro_rules! define_known_layout_constants {
    ($($variant:ident => $name:literal;)+) => {
        #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
        pub(crate) enum KnownLayoutConstant {
            $($variant),+
        }

        impl KnownLayoutConstant {
            pub(crate) const COUNT: usize = [$(Self::$variant),+].len();
            pub(crate) const ALL: [Self; Self::COUNT] = [$(Self::$variant),+];

            pub(crate) const fn name(self) -> &'static str {
                match self {
                    $(Self::$variant => $name,)+
                }
            }

            pub(crate) fn from_name(name: &str) -> Option<Self> {
                Some(match name {
                    $($name => Self::$variant,)+
                    _ => return None,
                })
            }
        }
    };
}

define_known_layout_constants! {
    Int32TagShifted => "INT32_TAG_SHIFTED";
    Int32Tag => "INT32_TAG";
    BooleanTag => "BOOLEAN_TAG";
    NanBaseTag => "NAN_BASE_TAG";
    UndefinedTag => "UNDEFINED_TAG";
    EmptyValue => "EMPTY_VALUE";
    UndefinedValue => "UNDEFINED_VALUE";
    NullValue => "NULL_VALUE";
    ShiftedIsCellPattern => "SHIFTED_IS_CELL_PATTERN";
    VmRunningExecutionContext => "VM_RUNNING_EXECUTION_CONTEXT";
    ExecutionContextExecutable => "EXECUTION_CONTEXT_EXECUTABLE";
    ExecutionContextProgramCounter => "EXECUTION_CONTEXT_PROGRAM_COUNTER";
    ExecutableBytecodeData => "EXECUTABLE_BYTECODE_DATA";
    SizeOfExecutionContext => "SIZEOF_EXECUTION_CONTEXT";
    CanonicalNanBits => "CANON_NAN_BITS";
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct LayoutConstant {
    id: LayoutConstantId,
    value: i64,
    category: LayoutConstantCategory,
}

impl LayoutConstant {
    pub(crate) fn id(self) -> LayoutConstantId {
        self.id
    }

    pub(crate) fn value(self) -> i64 {
        self.value
    }

    pub(crate) fn category(self) -> LayoutConstantCategory {
        self.category
    }

    pub(crate) fn known(self) -> Option<KnownLayoutConstant> {
        KnownLayoutConstant::ALL
            .get(self.id.0 as usize)
            .copied()
    }

    pub(crate) fn is(self, known: KnownLayoutConstant) -> bool {
        self.id == LayoutConstantId(known as u32)
    }
}

#[derive(Debug, Clone)]
pub(crate) struct LayoutConstants {
    by_name: HashMap<String, LayoutConstant>,
    known: [Option<LayoutConstant>; KnownLayoutConstant::COUNT],
}

impl LayoutConstants {
    fn new() -> Self {
        Self {
            by_name: HashMap::new(),
            known: [None; KnownLayoutConstant::COUNT],
        }
    }

    fn insert(&mut self, name: &str, value: i64) -> Result<(), ()> {
        if self.by_name.contains_key(name) {
            return Err(());
        }
        let known = KnownLayoutConstant::from_name(name);
        let id = known.map_or_else(
            || {
                (KnownLayoutConstant::COUNT + self.by_name.len())
                    .try_into()
                    .map(LayoutConstantId)
                    .map_err(|_| ())
            },
            |known| Ok(LayoutConstantId(known as u32)),
        )?;
        let category = if name.ends_with("_TAG") {
            LayoutConstantCategory::Tag
        } else if name.ends_with("_VALUE") {
            LayoutConstantCategory::Value
        } else {
            LayoutConstantCategory::Other
        };
        let constant = LayoutConstant {
            id,
            value,
            category,
        };
        self.by_name.insert(name.to_string(), constant);
        if let Some(known) = known {
            self.known[known as usize] = Some(constant);
        }
        Ok(())
    }

    pub(crate) fn get(&self, name: &str) -> Option<LayoutConstant> {
        self.by_name.get(name).copied()
    }

    pub(crate) fn known(
        &self,
        constant: KnownLayoutConstant,
    ) -> Option<LayoutConstant> {
        self.known[constant as usize]
    }

    #[cfg(test)]
    pub(crate) fn from_values(
        values: impl IntoIterator<Item = (String, i64)>,
    ) -> Self {
        let mut constants = Self::new();
        for (name, value) in values {
            constants.insert(&name, value).unwrap();
        }
        constants
    }
}

impl Default for LayoutConstants {
    fn default() -> Self {
        Self::new()
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum LayoutValue {
    Immediate(i64),
    Constant(LayoutConstant),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct Field {
    pub(crate) owner: Type,
    pub(crate) name: String,
    pub(crate) ty: Type,
    pub(crate) offset: LayoutValue,
    pub(crate) nonnull: bool,
    pub(crate) embedded: bool,
    pub(crate) pair: Option<String>,
    pub(crate) pinned_base: Option<String>,
    pub(crate) pinned_offset: Option<LayoutValue>,
    pub(crate) stride: Option<LayoutValue>,
}

struct UnresolvedField {
    line: usize,
    owner: Type,
    name: String,
    ty: Type,
    offset: String,
    nonnull: bool,
    embedded: bool,
    pair: Option<String>,
    pinned_base: Option<String>,
    pinned_offset: Option<String>,
    stride: Option<String>,
}

#[derive(Debug, Clone)]
pub(crate) struct LayoutDatabase {
    fields: Vec<Field>,
    constants: LayoutConstants,
}

impl LayoutDatabase {
    pub(crate) fn parse(source: &str) -> Result<Self, LayoutError> {
        let mut database = Self {
            fields: Vec::new(),
            constants: LayoutConstants::new(),
        };
        let mut field_names = HashSet::new();
        let mut unresolved_fields = Vec::new();

        for (index, source_line) in source.lines().enumerate() {
            let line_number = index + 1;
            let line = source_line.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            if let Some(declaration) = line.strip_prefix("const ") {
                let (name, value) = declaration
                    .split_once('=')
                    .ok_or_else(|| LayoutError::new(line_number, "constant requires '='"))?;
                let name = name.trim();
                if name.is_empty() || !is_identifier(name) {
                    return Err(LayoutError::new(line_number, format!("invalid constant name '{name}'")));
                }
                let value = parse_integer(value.trim()).map_err(|message| {
                    LayoutError::new(line_number, format!("invalid value for '{name}': {message}"))
                })?;
                if database.constants.insert(name, value).is_err() {
                    return Err(LayoutError::new(line_number, format!("duplicate constant '{name}'")));
                }
                continue;
            }
            let Some(declaration) = line.strip_prefix("field ") else {
                return Err(LayoutError::new(line_number, "expected 'const' or 'field' declaration"));
            };
            let words = declaration.split_ascii_whitespace().collect::<Vec<_>>();
            if words.len() < 4 {
                return Err(LayoutError::new(
                    line_number,
                    "field requires a qualified name, type, offset, and storage kind",
                ));
            }
            let (owner_name, name) = words[0]
                .split_once('.')
                .ok_or_else(|| LayoutError::new(line_number, "field name must be qualified by its owner type"))?;
            if owner_name.is_empty() || name.is_empty() || !is_identifier(owner_name) || !is_identifier(name) {
                return Err(LayoutError::new(
                    line_number,
                    format!("invalid qualified field name '{}'", words[0]),
                ));
            }
            if !field_names.insert(words[0].to_string()) {
                return Err(LayoutError::new(line_number, format!("duplicate field '{}'", words[0])));
            }
            let owner = Type::from_layout_type_name(owner_name)
                .ok_or_else(|| LayoutError::new(line_number, format!("unknown owner type '{owner_name}'")))?;
            let ty = Type::from_layout_type_name(words[1])
                .ok_or_else(|| LayoutError::new(line_number, format!("unknown field type '{}'", words[1])))?;
            let (nonnull, embedded) = match words[3] {
                "nonnull" => (true, false),
                "nullable" => (false, false),
                "embedded" => (false, true),
                storage => {
                    return Err(LayoutError::new(
                        line_number,
                        format!("unknown storage kind '{storage}'"),
                    ));
                }
            };

            let mut pair = None;
            let mut pinned_base = None;
            let mut pinned_offset = None;
            let mut stride = None;
            let mut cursor = 4;
            while cursor < words.len() {
                match words[cursor] {
                    "pinned" => {
                        if pinned_base.is_some() || cursor + 2 >= words.len() {
                            return Err(LayoutError::new(
                                line_number,
                                "pinned field requires one base and offset",
                            ));
                        }
                        pinned_base = Some(words[cursor + 1].to_string());
                        pinned_offset = Some(words[cursor + 2].to_string());
                        cursor += 3;
                    }
                    "stride" => {
                        if stride.is_some() || cursor + 1 >= words.len() {
                            return Err(LayoutError::new(line_number, "stride requires one value"));
                        }
                        stride = Some(words[cursor + 1].to_string());
                        cursor += 2;
                    }
                    value if pair.is_none() && is_identifier(value) => {
                        pair = Some(value.to_string());
                        cursor += 1;
                    }
                    value => {
                        return Err(LayoutError::new(
                            line_number,
                            format!("unexpected field modifier '{value}'"),
                        ));
                    }
                }
            }

            unresolved_fields.push(UnresolvedField {
                line: line_number,
                owner,
                name: name.to_string(),
                ty,
                offset: words[2].to_string(),
                nonnull,
                embedded,
                pair,
                pinned_base,
                pinned_offset,
                stride,
            });
        }

        database.fields = unresolved_fields
            .into_iter()
            .map(|field| {
                Ok(Field {
                    owner: field.owner,
                    name: field.name,
                    ty: field.ty,
                    offset: resolve_layout_value(
                        field.line,
                        &field.offset,
                        &database.constants,
                    )?,
                    nonnull: field.nonnull,
                    embedded: field.embedded,
                    pair: field.pair,
                    pinned_base: field.pinned_base,
                    pinned_offset: field
                        .pinned_offset
                        .as_deref()
                        .map(|value| {
                            resolve_layout_value(
                                field.line,
                                value,
                                &database.constants,
                            )
                        })
                        .transpose()?,
                    stride: field
                        .stride
                        .as_deref()
                        .map(|value| {
                            resolve_layout_value(
                                field.line,
                                value,
                                &database.constants,
                            )
                        })
                        .transpose()?,
                })
            })
            .collect::<Result<_, LayoutError>>()?;
        Ok(database)
    }

    pub(crate) fn fields(&self) -> &[Field] {
        &self.fields
    }

    #[cfg(test)]
    pub(crate) fn constants(&self) -> &LayoutConstants {
        &self.constants
    }

    pub(crate) fn into_constants(self) -> LayoutConstants {
        self.constants
    }
}

fn resolve_layout_value(
    line: usize,
    value: &str,
    constants: &LayoutConstants,
) -> Result<LayoutValue, LayoutError> {
    if let Ok(value) = parse_integer(value) {
        return Ok(LayoutValue::Immediate(value));
    }
    constants
        .get(value)
        .map(LayoutValue::Constant)
        .ok_or_else(|| {
            LayoutError::new(
                line,
                format!("field references undefined layout value '{value}'"),
            )
        })
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct LayoutError {
    pub(crate) line: usize,
    pub(crate) message: String,
}

impl LayoutError {
    fn new(line: usize, message: impl Into<String>) -> Self {
        Self {
            line,
            message: message.into(),
        }
    }
}

impl std::fmt::Display for LayoutError {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.line == 0 {
            write!(formatter, "invalid generated layout: {}", self.message)
        } else {
            write!(
                formatter,
                "invalid generated layout at line {}: {}",
                self.line, self.message
            )
        }
    }
}

impl std::error::Error for LayoutError {}

fn is_identifier(value: &str) -> bool {
    value.chars().enumerate().all(|(index, character)| {
        character == '_' || character.is_ascii_alphanumeric() && (index != 0 || !character.is_ascii_digit())
    })
}

fn parse_integer(value: &str) -> Result<i64, String> {
    if let Some(value) = value.strip_prefix("0x") {
        u64::from_str_radix(value, 16).map(|value| value as i64)
    } else if let Some(value) = value.strip_prefix("0b") {
        u64::from_str_radix(value, 2).map(|value| value as i64)
    } else {
        value.parse::<i64>()
    }
    .map_err(|error| error.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_typed_layout_fields_and_constants() {
        let database =
            LayoutDatabase::parse("const OBJECT_SHAPE = 16\nfield Object.shape Shape OBJECT_SHAPE nonnull\n").unwrap();

        assert_eq!(
            database.constants().get("OBJECT_SHAPE").unwrap().value(),
            16
        );
        assert_eq!(database.fields()[0].owner, Type::Object);
        assert_eq!(database.fields()[0].ty, Type::Shape);
        assert!(database.fields()[0].nonnull);
    }

    #[test]
    fn parses_pairs_pinned_offsets_and_strides() {
        let database = LayoutDatabase::parse(
            "const EXCEPTION = 8\nfield ExecutionContext.exception Value EXCEPTION nullable values_pair pinned values EXCEPTION\nfield EnvironmentCoordinateEntry.hops u32 0 nullable coordinate_pair stride 8\n",
        )
        .unwrap();

        assert_eq!(database.fields()[0].pair.as_deref(), Some("values_pair"));
        assert_eq!(database.fields()[0].pinned_base.as_deref(), Some("values"));
        assert!(matches!(
            database.fields()[0].pinned_offset,
            Some(LayoutValue::Constant(constant)) if constant.value() == 8
        ));
        assert_eq!(
            database.fields()[1].stride,
            Some(LayoutValue::Immediate(8))
        );
    }

    #[test]
    fn resolves_forward_layout_value_references() {
        let database = LayoutDatabase::parse(
            "field Object.shape Shape OBJECT_SHAPE nonnull\nconst OBJECT_SHAPE = 16\n",
        )
        .unwrap();

        assert!(matches!(
            database.fields()[0].offset,
            LayoutValue::Constant(constant) if constant.value() == 16
        ));
    }

    #[test]
    fn rejects_malformed_generated_input() {
        let error = LayoutDatabase::parse("field Object.shape Shape MISSING sometimes\n").unwrap_err();
        assert_eq!(error.line, 1);
        assert!(error.message.contains("unknown storage kind"));

        let error = LayoutDatabase::parse("field Object.shape Unknown 0 nullable\n").unwrap_err();
        assert_eq!(error.line, 1);
        assert!(error.message.contains("unknown field type"));

        let error = LayoutDatabase::parse("field Object.shape Shape MISSING nullable\n").unwrap_err();
        assert!(error.message.contains("undefined layout value"));
    }
}
