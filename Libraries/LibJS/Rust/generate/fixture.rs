/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Compares the generated layout with Libraries/LibJS/Flap/tests/interpreter-layout.conf, the recorded layout that
//! flapc's tests and the CI lint compile the interpreter against. Offsets and sizes may drift from the recording. The
//! set of fields, how the interpreter accesses each of them, and the values that encode shared meaning may not.

use std::collections::BTreeMap;

pub struct ParsedLayout {
    pub constants: BTreeMap<String, String>,
    /// Each field's line without its offset, which is free to differ.
    pub fields: BTreeMap<String, String>,
}

pub fn parse(text: &str) -> ParsedLayout {
    let mut constants = BTreeMap::new();
    let mut fields = BTreeMap::new();
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let words: Vec<&str> = line.split_whitespace().collect();
        match words.as_slice() {
            ["const", name, "=", value] => {
                constants.insert((*name).to_string(), normalize_integer(value));
            }
            ["field", name, flap_type, _offset, rest @ ..] => {
                fields.insert((*name).to_string(), format!("{flap_type} {}", rest.join(" ")));
            }
            _ => panic!("unexpected layout line: {line}"),
        }
    }
    ParsedLayout { constants, fields }
}

fn normalize_integer(value: &str) -> String {
    if let Some(hex) = value.strip_prefix("0x") {
        return u64::from_str_radix(hex, 16).map_or_else(|_| value.to_string(), |number| number.to_string());
    }
    value.to_string()
}

/// Constants whose values are shared meaning rather than layout: value encodings, enum values and flags that the
/// interpreter relies on.
const SHARED_CONSTANTS: &[&str] = &[
    "OBJECT_TAG",
    "STRING_TAG",
    "SYMBOL_TAG",
    "BIGINT_TAG",
    "ACCESSOR_TAG",
    "IS_CELL_PATTERN",
    "INT32_TAG",
    "BOOLEAN_TAG",
    "UNDEFINED_TAG",
    "NULL_TAG",
    "OBJECT_TAG_SHIFTED",
    "EMPTY_VALUE",
    "INT32_TAG_SHIFTED",
    "BOOLEAN_TRUE",
    "BOOLEAN_FALSE",
    "UNDEFINED_SHIFTED",
    "NULL_VALUE",
    "EMPTY_TAG_SHIFTED",
    "NAN_BASE_TAG",
    "CANON_NAN_BITS",
    "DOUBLE_ONE",
    "NEGATIVE_ZERO",
    "SHIFTED_IS_CELL_PATTERN",
    "ACCUMULATOR_REG_OFFSET",
    "EXCEPTION_REG_OFFSET",
    "THIS_VALUE_REG_OFFSET",
    "RETURN_VALUE_REG_OFFSET",
    "SAVED_LEXICAL_ENVIRONMENT_REG_OFFSET",
    "RESERVED_REGISTER_COUNT",
    "SLOW_PATH_CONTINUATION_BIT",
    "SIZEOF_VALUE",
    "SIZEOF_SCRIPT_OR_MODULE",
    "ALIGNOF_EXECUTION_CONTEXT",
    "EXECUTION_CONTEXT_NO_YIELD_CONTINUATION",
    "PUT_KIND_NORMAL",
    "BUILTIN_MATH_ABS",
    "BUILTIN_MATH_FLOOR",
    "BUILTIN_MATH_CEIL",
    "BUILTIN_MATH_ROUND",
    "BUILTIN_MATH_SQRT",
    "BUILTIN_MATH_EXP",
    "BUILTIN_STRING_FROM_CHAR_CODE",
    "BUILTIN_STRING_PROTOTYPE_CHAR_CODE_AT",
    "BUILTIN_STRING_PROTOTYPE_CHAR_AT",
    "ENVIRONMENT_COORDINATE_INVALID",
    "ENVIRONMENT_COORDINATE_SIZE",
    "UTF16_SHORT_STRING_FLAG",
    "UTF16_SHORT_STRING_BYTE_COUNT_SHIFT_COUNT",
    "UTF16_SHORT_STRING_BYTE_COUNT_AND_FLAG",
    "UTF16_SHORT_STRING_STORAGE",
    "UTF16_STRING_DATA_LENGTH_IN_CODE_UNITS",
    "UTF16_STRING_DATA_FLAGS",
    "UTF16_STRING_DATA_STRING_STORAGE",
    "UTF16_STRING_DATA_HAS_UTF16_STORAGE",
    "PROPERTY_LOOKUP_CACHE_SIZE",
    "PROPERTY_LOOKUP_CACHE_DATA_POINTER_MASK",
    "BYTE_LENGTH_U32_INDEX",
    "BYTE_LENGTH_SIZE",
    "NATIVE_FUNCTION_TYPE_COUNT",
    "TYPED_ARRAY_CACHED_DATA_OFFSET_INVALID",
];

pub fn check_against_fixture(generated: &ParsedLayout, fixture: &ParsedLayout) -> Result<(), String> {
    let mut problems = Vec::new();

    for (name, access) in &fixture.fields {
        match generated.fields.get(name) {
            None => problems.push(format!("missing field {name}")),
            Some(generated_access) if generated_access != access => {
                problems.push(format!(
                    "field {name} is `{generated_access}`, the recorded layout has `{access}`"
                ));
            }
            Some(_) => {}
        }
    }
    for name in generated.fields.keys() {
        if !fixture.fields.contains_key(name) {
            problems.push(format!("field {name} is not in the recorded layout"));
        }
    }

    for name in fixture.constants.keys() {
        if !generated.constants.contains_key(name) {
            problems.push(format!("missing constant {name}"));
        }
    }
    for name in SHARED_CONSTANTS {
        let (Some(generated_value), Some(fixture_value)) =
            (generated.constants.get(*name), fixture.constants.get(*name))
        else {
            problems.push(format!("shared constant {name} is missing"));
            continue;
        };
        if generated_value != fixture_value {
            problems.push(format!(
                "constant {name} is {generated_value}, the recorded layout has {fixture_value}"
            ));
        }
    }

    if problems.is_empty() {
        Ok(())
    } else {
        Err(problems.join("\n"))
    }
}
