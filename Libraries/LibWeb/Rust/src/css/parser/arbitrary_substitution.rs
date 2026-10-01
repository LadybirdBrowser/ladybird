/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_tokenizer::ParserTokenKind;
use crate::css::parser::component_value::{ComponentKind, ComponentValue};
use crate::css::parser::value_parser::equals_ascii_case_insensitive;
use std::collections::HashSet;
use std::sync::RwLock;
use std::sync::atomic::{AtomicU64, Ordering};

#[derive(Clone, Copy)]
enum ArbitrarySubstitutionFunction {
    Attr,
    DashedFunction,
    Env,
    If,
    Inherit,
    Var,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct SubstitutionFunctionsPresence {
    pub attr: bool,
    pub dashed_function: bool,
    pub env: bool,
    pub if_: bool,
    pub inherit: bool,
    pub var: bool,
}

impl SubstitutionFunctionsPresence {
    pub(crate) fn has_any(self) -> bool {
        self.attr || self.dashed_function || self.env || self.if_ || self.inherit || self.var
    }
}

fn arbitrary_substitution_function(name: &[u16]) -> Option<ArbitrarySubstitutionFunction> {
    if equals_ascii_case_insensitive(name, b"attr") {
        return Some(ArbitrarySubstitutionFunction::Attr);
    }
    if name.starts_with(&[u16::from(b'-'), u16::from(b'-')]) {
        return Some(ArbitrarySubstitutionFunction::DashedFunction);
    }
    if equals_ascii_case_insensitive(name, b"env") {
        return Some(ArbitrarySubstitutionFunction::Env);
    }
    if equals_ascii_case_insensitive(name, b"if") {
        return Some(ArbitrarySubstitutionFunction::If);
    }
    if equals_ascii_case_insensitive(name, b"inherit") {
        return Some(ArbitrarySubstitutionFunction::Inherit);
    }
    if equals_ascii_case_insensitive(name, b"var") {
        return Some(ArbitrarySubstitutionFunction::Var);
    }
    None
}

#[derive(Clone, Copy)]
enum EndToken {
    Colon,
    Comma,
}

fn is_end_token(value: &ComponentValue, end_token: EndToken) -> bool {
    match end_token {
        EndToken::Colon => value.is_colon(),
        EndToken::Comma => value.is_comma(),
    }
}

fn nested_declaration_value_is_valid(values: &[ComponentValue]) -> bool {
    let mut position = 0;
    consume_declaration_value(values, &mut position, None, true, false);
    position == values.len()
}

// https://drafts.csswg.org/css-syntax/#typedef-declaration-value
fn consume_declaration_value(
    values: &[ComponentValue],
    position: &mut usize,
    end_token: Option<EndToken>,
    nested: bool,
    disallow_top_level_curly_blocks: bool,
) {
    while let Some(value) = values.get(*position) {
        let valid = match &value.kind {
            ComponentKind::Function { values, .. } => nested_declaration_value_is_valid(values),
            ComponentKind::SimpleBlock { opening, values } => {
                if !nested && disallow_top_level_curly_blocks && matches!(opening, ParserTokenKind::OpenCurly) {
                    false
                } else {
                    nested_declaration_value_is_valid(values)
                }
            }
            ComponentKind::Token(token) => match token {
                ParserTokenKind::EndOfFile
                | ParserTokenKind::BadString
                | ParserTokenKind::BadUrl
                | ParserTokenKind::Function(_)
                | ParserTokenKind::OpenCurly
                | ParserTokenKind::OpenParen
                | ParserTokenKind::OpenSquare
                | ParserTokenKind::CloseCurly
                | ParserTokenKind::CloseParen
                | ParserTokenKind::CloseSquare => false,
                ParserTokenKind::Semicolon => nested,
                ParserTokenKind::Delim(value) if *value == u32::from(b'!') => nested,
                _ => nested || !end_token.is_some_and(|end_token| is_end_token(value, end_token)),
            },
        };
        if !valid {
            break;
        }
        *position += 1;
    }
}

fn consume_declaration_value_as_span(
    values: &[ComponentValue],
    position: &mut usize,
    end_token: Option<EndToken>,
    disallow_top_level_curly_blocks: bool,
) -> bool {
    let start = *position;
    consume_declaration_value(values, position, end_token, false, disallow_top_level_curly_blocks);
    *position != start
}

fn parse_declaration_value_then_optional_declaration_value(
    values: &[ComponentValue],
    position: &mut usize,
    separator: EndToken,
) -> bool {
    if !consume_declaration_value_as_span(values, position, Some(separator), false) {
        return false;
    }
    if *position == values.len() {
        return true;
    }
    if !is_end_token(&values[*position], separator) {
        return false;
    }
    *position += 1;
    consume_declaration_value_as_span(values, position, None, false);
    true
}

fn discard_whitespace(values: &[ComponentValue], position: &mut usize) {
    while values.get(*position).is_some_and(ComponentValue::is_whitespace) {
        *position += 1;
    }
}

fn parse_dashed_function_argument(values: &[ComponentValue], position: &mut usize) -> bool {
    let saved_position = *position;
    discard_whitespace(values, position);
    if let Some(ComponentValue {
        kind:
            ComponentKind::SimpleBlock {
                opening: ParserTokenKind::OpenCurly,
                values: block_values,
            },
        ..
    }) = values.get(*position)
    {
        let mut block_position = 0;
        discard_whitespace(block_values, &mut block_position);
        if consume_declaration_value_as_span(block_values, &mut block_position, None, false)
            && block_position == block_values.len()
        {
            *position += 1;
            return true;
        }
    }
    *position = saved_position;
    consume_declaration_value_as_span(values, position, Some(EndToken::Comma), true)
}

// This mirrors parse_according_to_argument_grammar() in
// CSS/Parser/ArbitrarySubstitutionFunctions.cpp.
fn arguments_are_valid(function: ArbitrarySubstitutionFunction, values: &[ComponentValue]) -> bool {
    let mut position = 0;
    match function {
        ArbitrarySubstitutionFunction::Attr
        | ArbitrarySubstitutionFunction::Env
        | ArbitrarySubstitutionFunction::Inherit
        | ArbitrarySubstitutionFunction::Var => {
            parse_declaration_value_then_optional_declaration_value(values, &mut position, EndToken::Comma)
                && position == values.len()
        }
        ArbitrarySubstitutionFunction::DashedFunction => {
            discard_whitespace(values, &mut position);
            if position == values.len() {
                return true;
            }
            if !parse_dashed_function_argument(values, &mut position) {
                return false;
            }
            discard_whitespace(values, &mut position);
            while position < values.len() {
                if !values[position].is_comma() {
                    return false;
                }
                position += 1;
                discard_whitespace(values, &mut position);
                if !parse_dashed_function_argument(values, &mut position) {
                    return false;
                }
                discard_whitespace(values, &mut position);
            }
            true
        }
        ArbitrarySubstitutionFunction::If => {
            let mut branch_count = 0;
            while position < values.len() {
                if !parse_declaration_value_then_optional_declaration_value(values, &mut position, EndToken::Colon) {
                    break;
                }
                branch_count += 1;
                if !values
                    .get(position)
                    .is_some_and(|value| matches!(value.kind, ComponentKind::Token(ParserTokenKind::Semicolon)))
                {
                    break;
                }
                position += 1;
            }
            branch_count != 0 && position == values.len()
        }
    }
}

pub(crate) fn arguments_are_valid_for_ffi(function: u8, values: &[ComponentValue]) -> bool {
    let function = match function {
        0 => ArbitrarySubstitutionFunction::Attr,
        1 => ArbitrarySubstitutionFunction::DashedFunction,
        2 => ArbitrarySubstitutionFunction::Env,
        3 => ArbitrarySubstitutionFunction::If,
        4 => ArbitrarySubstitutionFunction::Inherit,
        5 => ArbitrarySubstitutionFunction::Var,
        _ => return false,
    };
    arguments_are_valid(function, values)
}

pub(crate) fn substitution_function_presence_bits(values: &[ComponentValue]) -> Option<u8> {
    let mut presence = SubstitutionFunctionsPresence::default();
    collect_arbitrary_substitution_function_presence(values, &mut presence).ok()?;
    Some(
        u8::from(presence.attr)
            | (u8::from(presence.dashed_function) << 1)
            | (u8::from(presence.env) << 2)
            | (u8::from(presence.if_) << 3)
            | (u8::from(presence.inherit) << 4)
            | (u8::from(presence.var) << 5),
    )
}

fn collect_presence_from_value(value: &ComponentValue, presence: &mut SubstitutionFunctionsPresence) -> Result<(), ()> {
    let nested_values = match &value.kind {
        ComponentKind::Function { name, values } => {
            if let Some(function) = arbitrary_substitution_function(name) {
                if !arguments_are_valid(function, values) {
                    return Err(());
                }
                match function {
                    ArbitrarySubstitutionFunction::Attr => presence.attr = true,
                    ArbitrarySubstitutionFunction::DashedFunction => presence.dashed_function = true,
                    ArbitrarySubstitutionFunction::Env => presence.env = true,
                    ArbitrarySubstitutionFunction::If => presence.if_ = true,
                    ArbitrarySubstitutionFunction::Inherit => presence.inherit = true,
                    ArbitrarySubstitutionFunction::Var => presence.var = true,
                }
            }
            values.as_ref()
        }
        ComponentKind::SimpleBlock { values, .. } => values.as_ref(),
        ComponentKind::Token(_) => return Ok(()),
    };
    collect_arbitrary_substitution_function_presence(nested_values, presence)
}

pub(crate) fn collect_arbitrary_substitution_function_presence(
    values: &[ComponentValue],
    presence: &mut SubstitutionFunctionsPresence,
) -> Result<(), ()> {
    for value in values {
        collect_presence_from_value(value, presence)?;
    }
    Ok(())
}

/// The attribute names an `attr()` anywhere in the process can read, ASCII-lowercased, or every
/// name once one computes its name. A style engine copies an attribute's value text only for names
/// a selector or this set can read, and backfills its elements when the set grows.
struct AttrNamesRead {
    any_name: bool,
    names: HashSet<Box<[u16]>>,
}

static ATTR_NAMES_READ: RwLock<Option<AttrNamesRead>> = RwLock::new(None);
static ATTR_NAMES_READ_GENERATION: AtomicU64 = AtomicU64::new(0);

/// Grows with every name an `attr()` can newly read.
pub(crate) fn attr_names_read_generation() -> u64 {
    ATTR_NAMES_READ_GENERATION.load(Ordering::Acquire)
}

/// Whether an `attr()` may read an attribute with this local name.
pub(crate) fn attr_may_read_name(local_name: &[u16]) -> bool {
    let read = ATTR_NAMES_READ.read().expect("attr() name set poisoned");
    read.as_ref()
        .is_some_and(|read| read.any_name || read.names.contains(ascii_lowercase(local_name).as_slice()))
}

fn ascii_lowercase(text: &[u16]) -> Vec<u16> {
    text.iter()
        .map(|&unit| {
            if (u16::from(b'A')..=u16::from(b'Z')).contains(&unit) {
                unit + 32
            } else {
                unit
            }
        })
        .collect()
}

// https://drafts.csswg.org/css-values-5/#attr-notation
// attr( <attr-name> <attr-type>? , <declaration-value>?), where <attr-name> = [ <ident-token>? '|' ]? <ident-token>
fn collect_attr_names_read(values: &[ComponentValue], names: &mut Vec<Vec<u16>>, any_name: &mut bool) {
    for value in values {
        let nested = match &value.kind {
            ComponentKind::Function { name, values } => {
                if equals_ascii_case_insensitive(name, b"attr") {
                    let mut position = 0;
                    discard_whitespace(values, &mut position);
                    let first = values.get(position);
                    let mut local = first.and_then(ComponentValue::ident);
                    if first.is_some_and(|first| first.ident().is_some() || first.is_delim(b'*'))
                        && values.get(position + 1).is_some_and(|value| value.is_delim(b'|'))
                    {
                        local = values.get(position + 2).and_then(ComponentValue::ident);
                    } else if first.is_some_and(|first| first.is_delim(b'|')) {
                        local = values.get(position + 1).and_then(ComponentValue::ident);
                    }
                    match local {
                        Some(local) => names.push(ascii_lowercase(local)),
                        // A name that substitution computes can be any name.
                        None => *any_name = true,
                    }
                    // An attribute value read with a syntax is substituted in turn, so an attr() in
                    // it can name any attribute.
                    *any_name |= values.iter().take_while(|value| !value.is_comma()).any(|value| {
                        matches!(&value.kind, ComponentKind::Function { name, .. } if equals_ascii_case_insensitive(name, b"type"))
                    });
                }
                values.as_ref()
            }
            ComponentKind::SimpleBlock { values, .. } => values.as_ref(),
            ComponentKind::Token(_) => continue,
        };
        collect_attr_names_read(nested, names, any_name);
    }
}

/// Record what the `attr()` functions of a declared value can read, before any engine resolves it.
pub(crate) fn note_attr_names_read_by(values: &[ComponentValue]) {
    let mut names = Vec::new();
    let mut any_name = false;
    collect_attr_names_read(values, &mut names, &mut any_name);
    {
        let read = ATTR_NAMES_READ.read().expect("attr() name set poisoned");
        if read.as_ref().is_some_and(|read| {
            read.any_name || (!any_name && names.iter().all(|name| read.names.contains(name.as_slice())))
        }) {
            return;
        }
    }
    let mut read = ATTR_NAMES_READ.write().expect("attr() name set poisoned");
    let read = read.get_or_insert_with(|| AttrNamesRead {
        any_name: false,
        names: HashSet::new(),
    });
    let mut grew = any_name && !read.any_name;
    read.any_name |= any_name;
    for name in names {
        grew |= read.names.insert(name.into_boxed_slice());
    }
    if grew {
        ATTR_NAMES_READ_GENERATION.fetch_add(1, Ordering::AcqRel);
    }
}

pub(crate) fn declaration_value_is_valid(values: &[ComponentValue]) -> bool {
    let mut position = 0;
    consume_declaration_value_as_span(values, &mut position, None, false) && position == values.len()
}

#[cfg(test)]
mod tests {
    use super::{
        SubstitutionFunctionsPresence, collect_arbitrary_substitution_function_presence, collect_attr_names_read,
        declaration_value_is_valid,
    };
    use crate::css::css_tokenizer::tokenize_for_parser;
    use crate::css::parser::component_value::consume_a_list_of_component_values;

    fn attr_names_read(source: &str) -> (Vec<String>, bool) {
        let values = consume_a_list_of_component_values(tokenize_for_parser(source.as_bytes())).unwrap();
        let mut names = Vec::new();
        let mut any_name = false;
        collect_attr_names_read(&values, &mut names, &mut any_name);
        (
            names.iter().map(|name| String::from_utf16_lossy(name)).collect(),
            any_name,
        )
    }

    #[test]
    fn attr_reads_the_names_it_names_and_any_name_through_a_syntax() {
        assert_eq!(
            attr_names_read("attr(Data-Size px)"),
            (vec!["data-size".to_string()], false)
        );
        assert_eq!(
            attr_names_read("attr(*|foo, attr(bar)) var(--x, attr(|baz))"),
            (vec!["foo".to_string(), "bar".to_string(), "baz".to_string()], false)
        );
        // The value read with a syntax is substituted in turn, so it can name any attribute.
        assert_eq!(
            attr_names_read("attr(data-foo type(*))"),
            (vec!["data-foo".to_string()], true)
        );
        // A name that substitution computes can be any name; a type() in the fallback is not a syntax.
        assert!(attr_names_read("attr(var(--name))").1);
        assert!(!attr_names_read("attr(data-foo, type(*))").1);
    }

    fn collect(source: &str) -> Result<SubstitutionFunctionsPresence, ()> {
        let values = consume_a_list_of_component_values(tokenize_for_parser(source.as_bytes())).unwrap();
        let mut presence = SubstitutionFunctionsPresence::default();
        collect_arbitrary_substitution_function_presence(&values, &mut presence)?;
        Ok(presence)
    }

    #[test]
    fn collects_nested_substitution_function_presence() {
        assert_eq!(
            collect("outer([ATTR(data-x, var(--fallback)) --mix({ value }, env(name))])"),
            Ok(SubstitutionFunctionsPresence {
                attr: true,
                dashed_function: true,
                env: true,
                var: true,
                ..Default::default()
            })
        );
        assert!(!collect("random-item(--choice, one, two)").unwrap().has_any());
    }

    #[test]
    fn validates_declaration_value_argument_grammars() {
        for source in [
            "var(--name)",
            "var(--name,)",
            "attr(data-name type(<string>), fallback)",
            "env(safe-area-inset-top, 0px)",
            "inherit(color, red)",
            "if(value)",
            "if(style(--flag): one; else: two;)",
            "--mix()",
            "--mix({ value }, fallback)",
        ] {
            assert!(collect(source).is_ok(), "expected valid arguments: {source}");
        }
        for source in [
            "var()",
            "var(,--fallback)",
            "attr(data-name ! invalid)",
            "env(name, fallback ! invalid)",
            "inherit(value; fallback)",
            "if()",
            "if(condition: value ! invalid)",
            "--mix(, value)",
            "--mix({})",
            "--mix(value ! invalid)",
        ] {
            assert!(collect(source).is_err(), "expected invalid arguments: {source}");
        }
    }

    #[test]
    fn validates_nested_substitution_functions() {
        assert!(collect("outer(var(--name, env(name)))").is_ok());
        assert!(collect("outer(var(--name, env()))").is_err());
        assert!(collect("var(--name, [attr()])").is_err());
    }

    #[test]
    fn validates_complete_declaration_values() {
        for source in ["value", "function(value; !important)", "{ value; !important }"] {
            let values = consume_a_list_of_component_values(tokenize_for_parser(source.as_bytes())).unwrap();
            assert!(
                declaration_value_is_valid(&values),
                "expected valid declaration value: {source}"
            );
        }
        for source in ["", "value; trailing", "value ! trailing", "url(bad\"url)"] {
            let values = consume_a_list_of_component_values(tokenize_for_parser(source.as_bytes())).unwrap();
            assert!(
                !declaration_value_is_valid(&values),
                "expected invalid declaration value: {source}"
            );
        }
    }
}
