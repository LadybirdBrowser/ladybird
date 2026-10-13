/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Writes the GL constants that the WebGL interfaces define, from the `const` members of their IDL, so that the
//! implementation names a value as the specifications do and cannot disagree with the constant that script reads.

use std::fmt::Write;

#[derive(Debug)]
pub struct Constant {
    pub name: String,
    pub rust_type: &'static str,
    /// The value as the first definition of the constant spells it, which the generated constant keeps.
    pub spelling: String,
    pub value: i128,
}

/// A comment becomes a space, which keeps the words around it apart, and a string literal keeps only its quotes: both
/// can hold a `;` or a `const`, and neither holds a statement.
fn without_comments_and_string_contents(idl: &str) -> String {
    let mut out = String::with_capacity(idl.len());
    let mut rest = idl;
    while let Some(start) = rest.find(['/', '"']) {
        let (before, found) = rest.split_at(start);
        out.push_str(before);
        rest = if let Some(comment) = found.strip_prefix("//") {
            out.push(' ');
            comment.find('\n').map_or("", |end| &comment[end..])
        } else if let Some(comment) = found.strip_prefix("/*") {
            out.push(' ');
            comment.find("*/").map_or("", |end| &comment[end + 2..])
        } else if let Some(string) = found.strip_prefix('"') {
            out.push_str("\"\"");
            string.find('"').map_or("", |end| &string[end + 1..])
        } else {
            out.push('/');
            &found[1..]
        };
    }
    out.push_str(rest);
    out
}

/// The value of an integer of the IDL, https://webidl.spec.whatwg.org/#prod-integer, whose spelling Rust reads as the
/// same integer: decimal digits without a leading zero, which makes an integer of the IDL octal, or hexadecimal digits
/// after `0x`.
fn integer_value(spelling: &str) -> Option<i128> {
    let digits = spelling.strip_prefix('-').unwrap_or(spelling);
    let magnitude = match digits.strip_prefix("0x") {
        Some(hex_digits) if hex_digits.bytes().all(|byte| byte.is_ascii_hexdigit()) => {
            i128::from_str_radix(hex_digits, 16).ok()?
        }
        None if digits.bytes().all(|byte| byte.is_ascii_digit()) && (digits == "0" || !digits.starts_with('0')) => {
            digits.parse::<i128>().ok()?
        }
        _ => return None,
    };
    // NB: The spelling is written to Rust as it is, where an unsigned type does not take the sign of -0.
    match (spelling.starts_with('-'), magnitude) {
        (true, 0) => None,
        (true, magnitude) => Some(-magnitude),
        (false, magnitude) => Some(magnitude),
    }
}

/// https://webidl.spec.whatwg.org/#prod-ExtendedAttributeList
fn without_leading_extended_attributes(statement: &str) -> &str {
    let statement = statement.trim_start();
    statement
        .strip_prefix('[')
        .and_then(|attributes_and_member| attributes_and_member.split_once(']'))
        .map_or(statement, |(_attributes, member)| member)
}

fn parse_constant(statement: &str) -> Result<Constant, String> {
    let unreadable = || {
        let statement = statement.split_whitespace().collect::<Vec<_>>().join(" ");
        format!("the WebGL constants cannot be read from the IDL statement {statement:?}")
    };
    let declaration = statement.strip_suffix(';').ok_or_else(unreadable)?;
    let (type_and_name, spelling) = declaration.split_once('=').ok_or_else(unreadable)?;
    let mut words = type_and_name.split_whitespace().skip(1);
    let (Some(idl_type), Some(name), None) = (words.next(), words.next(), words.next()) else {
        return Err(unreadable());
    };
    if !name.bytes().all(|byte| byte.is_ascii_alphanumeric() || byte == b'_') {
        return Err(unreadable());
    }
    let (rust_type, values_of_type) = match idl_type {
        "GLenum" => ("u32", 0..=i128::from(u32::MAX)),
        "GLint64" => ("i64", i128::from(i64::MIN)..=i128::from(i64::MAX)),
        _ => {
            return Err(format!(
                "the WebGL constant {name} has the type {idl_type}, which has no Rust type yet"
            ));
        }
    };
    let spelling = spelling.trim();
    let value = integer_value(spelling)
        .filter(|value| values_of_type.contains(value))
        .ok_or_else(unreadable)?;
    Ok(Constant {
        name: name.to_string(),
        rust_type,
        spelling: spelling.to_string(),
        value,
    })
}

/// Several interfaces define the same constant, which then has to have the same value in each, however each spells
/// it.
pub fn collect_constants(idl: &str, constants: &mut Vec<Constant>) -> Result<(), String> {
    // NB: The first member of an interface follows a `{`, and every other one follows the `;` of the member before it.
    for statement in without_comments_and_string_contents(idl).split_inclusive([';', '{', '}']) {
        let statement = without_leading_extended_attributes(statement);
        if statement.split_whitespace().next() != Some("const") {
            continue;
        }
        let constant = parse_constant(statement)?;
        match constants.iter().find(|existing| existing.name == constant.name) {
            None => constants.push(constant),
            Some(existing) if existing.rust_type == constant.rust_type && existing.value == constant.value => {}
            Some(existing) => {
                return Err(format!(
                    "the WebGL constant {} is defined as the {} {} and as the {} {}",
                    constant.name, existing.rust_type, existing.spelling, constant.rust_type, constant.spelling
                ));
            }
        }
    }
    Ok(())
}

pub fn write_constants(constants: &[Constant]) -> String {
    let mut out =
        String::from("// Generated by build.rs from the constants of the IDL files of Libraries/LibWeb/WebGL.\n\n");
    for constant in constants {
        writeln!(
            out,
            "pub(crate) const GL_{}: {} = {};",
            constant.name, constant.rust_type, constant.spelling
        )
        .unwrap();
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn written_constants(idl_files: &[&str]) -> Result<Vec<String>, String> {
        let mut constants = Vec::new();
        for idl in idl_files {
            collect_constants(idl, &mut constants)?;
        }
        let written = write_constants(&constants);
        Ok(written.lines().skip(2).map(str::to_string).collect())
    }

    fn unreadable(statement: &str) -> String {
        format!("the WebGL constants cannot be read from the IDL statement {statement:?}")
    }

    #[test]
    fn constants_are_collected_once() {
        let idl_files = [
            "interface mixin A {\n    const GLenum DEPTH_BUFFER_BIT = 0x00000100;\n    const GLenum ZERO = 0;\n\
             // const GLenum NOT_THIS = 1;\n    const GLint64 TIMEOUT_IGNORED = -1; // forever\n    undefined f();\n};\n",
            "interface B {\n    const GLenum   ZERO   = 0;\n};\n",
        ];
        assert_eq!(
            written_constants(&idl_files).unwrap(),
            [
                "pub(crate) const GL_DEPTH_BUFFER_BIT: u32 = 0x00000100;",
                "pub(crate) const GL_ZERO: u32 = 0;",
                "pub(crate) const GL_TIMEOUT_IGNORED: i64 = -1;",
            ]
        );
    }

    #[test]
    fn a_constant_keeps_its_first_spelling_whatever_the_later_ones_are() {
        let idl_files = [
            "interface A { const GLenum DEPTH_BUFFER_BIT = 0x0100; };",
            "interface B { const GLenum DEPTH_BUFFER_BIT = 256; };",
            "interface C { const GLenum DEPTH_BUFFER_BIT = 0x00000100; };",
        ];
        assert_eq!(
            written_constants(&idl_files).unwrap(),
            ["pub(crate) const GL_DEPTH_BUFFER_BIT: u32 = 0x0100;"]
        );
    }

    #[test]
    fn a_constant_has_one_value_and_one_type() {
        assert_eq!(
            written_constants(&["const GLenum ONE = 1;", "const GLenum ONE = 0x2;"]).unwrap_err(),
            "the WebGL constant ONE is defined as the u32 1 and as the u32 0x2"
        );
        assert_eq!(
            written_constants(&["const GLenum ONE = 1;", "const GLint64 ONE = 1;"]).unwrap_err(),
            "the WebGL constant ONE is defined as the u32 1 and as the i64 1"
        );
    }

    #[test]
    fn constants_are_read_by_statement_and_not_by_line() {
        let idl = "interface A { const GLenum ONE = 1; const GLenum TWO = 2;\n    const GLenum\n        THREE\n        =\n\
                   3;const GLenum FOUR=4;\n    undefined f(); const GLenum FIVE = 5 ; };";
        assert_eq!(
            written_constants(&[idl]).unwrap(),
            [
                "pub(crate) const GL_ONE: u32 = 1;",
                "pub(crate) const GL_TWO: u32 = 2;",
                "pub(crate) const GL_THREE: u32 = 3;",
                "pub(crate) const GL_FOUR: u32 = 4;",
                "pub(crate) const GL_FIVE: u32 = 5;",
            ]
        );
    }

    #[test]
    fn a_constant_can_follow_extended_attributes() {
        let idl = "interface A {\n    [Exposed=Window] const GLenum ON_THE_SAME_LINE = 1;\n    \
                   [Exposed=(Window,Worker)]\n    const GLenum ON_THE_NEXT_LINE = 2;\n    \
                   [Exposed=Window]const GLenum WITHOUT_A_SPACE = 3;\n    \
                   [Exposed=Window] undefined f(long const);\n};";
        assert_eq!(
            written_constants(&[idl]).unwrap(),
            [
                "pub(crate) const GL_ON_THE_SAME_LINE: u32 = 1;",
                "pub(crate) const GL_ON_THE_NEXT_LINE: u32 = 2;",
                "pub(crate) const GL_WITHOUT_A_SPACE: u32 = 3;",
            ]
        );
        assert_eq!(
            written_constants(&["interface A { [Exposed=Window] const GLenum NAMED = OTHER; };"]).unwrap_err(),
            unreadable("const GLenum NAMED = OTHER;")
        );
    }

    #[test]
    fn braces_that_are_not_of_an_interface_end_a_statement_too() {
        let idl = "dictionary D { long member = 1; };\ninterface A {\n    \
                   undefined f(optional D d = {}); const GLenum AFTER_A_DEFAULT_VALUE = 1;\n};\n\
                   namespace N { const GLenum IN_A_NAMESPACE = 2; };";
        assert_eq!(
            written_constants(&[idl]).unwrap(),
            [
                "pub(crate) const GL_AFTER_A_DEFAULT_VALUE: u32 = 1;",
                "pub(crate) const GL_IN_A_NAMESPACE: u32 = 2;",
            ]
        );
    }

    #[test]
    fn a_line_can_end_with_a_carriage_return_before_its_line_feed() {
        let idl = "interface A {\r\n    const GLenum BEFORE = 0x10;\r\n    // const GLenum IN_A_LINE = 1;\r\n    \
                   const GLenum AFTER = 2;\r\n};\r\n";
        assert_eq!(
            written_constants(&[idl]).unwrap(),
            [
                "pub(crate) const GL_BEFORE: u32 = 0x10;",
                "pub(crate) const GL_AFTER: u32 = 2;",
            ]
        );
    }

    #[test]
    fn comments_and_string_literals_define_no_constants() {
        let idl = "interface A {\n    /* const GLenum IN_A_BLOCK = 1; */\n    /*\n    const GLenum IN_A_LONG_BLOCK = 2;\n\
                   */\n    // const GLenum IN_A_LINE = 3;\n    const GLenum /* between */ KEPT = 4; // const GLenum AFTER = 5;\n\
                   attribute DOMString a = \"; const GLenum IN_A_STRING = 6;\"; const GLenum AFTER_A_STRING = 7;\n\
                   attribute DOMString b = \"//\"; const GLenum AFTER_A_STRING_WITH_SLASHES = 8;\n\
                   [Reflect=a/b] attribute DOMString c; const GLenum AFTER_A_SLASH = 9;\n};\n\
                   /* const GLenum IN_A_BLOCK_WITHOUT_AN_END = 10;";
        assert_eq!(
            written_constants(&[idl]).unwrap(),
            [
                "pub(crate) const GL_KEPT: u32 = 4;",
                "pub(crate) const GL_AFTER_A_STRING: u32 = 7;",
                "pub(crate) const GL_AFTER_A_STRING_WITH_SLASHES: u32 = 8;",
                "pub(crate) const GL_AFTER_A_SLASH: u32 = 9;",
            ]
        );
    }

    #[test]
    fn a_value_is_within_the_range_of_its_type() {
        let idl = "const GLenum HIGHEST = 0xFFFFFFFF; const GLenum HIGHEST_IN_DECIMAL = 4294967295;\n\
                   const GLint64 LOWEST_OF_64_BITS = -9223372036854775808;\n\
                   const GLint64 LOWEST_OF_64_BITS_IN_HEXADECIMAL = -0x8000000000000000;\n\
                   const GLint64 HIGHEST_OF_64_BITS = 0x7fffffffffffffff;";
        assert_eq!(
            written_constants(&[idl]).unwrap(),
            [
                "pub(crate) const GL_HIGHEST: u32 = 0xFFFFFFFF;",
                "pub(crate) const GL_HIGHEST_IN_DECIMAL: u32 = 4294967295;",
                "pub(crate) const GL_LOWEST_OF_64_BITS: i64 = -9223372036854775808;",
                "pub(crate) const GL_LOWEST_OF_64_BITS_IN_HEXADECIMAL: i64 = -0x8000000000000000;",
                "pub(crate) const GL_HIGHEST_OF_64_BITS: i64 = 0x7fffffffffffffff;",
            ]
        );
        for statement in [
            "const GLenum TOO_HIGH = 0x100000000;",
            "const GLenum TOO_HIGH = 4294967296;",
            "const GLenum NEGATIVE = -1;",
            "const GLenum NEGATIVE_ZERO = -0;",
            "const GLint64 TOO_LOW = -9223372036854775809;",
            "const GLint64 TOO_LOW = -0x8000000000000001;",
            "const GLint64 TOO_HIGH = 9223372036854775808;",
            "const GLint64 TOO_HIGH_FOR_ANY_TYPE = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF;",
        ] {
            assert_eq!(written_constants(&[statement]).unwrap_err(), unreadable(statement));
        }
    }

    #[test]
    fn a_statement_that_cannot_be_read_is_named() {
        for statement in [
            "const GLenum NAMED = OTHER;",
            "const GLenum OCTAL = 010;",
            "const GLenum UPPER_CASE_PREFIX = 0X10;",
            "const GLenum WITHOUT_DIGITS = 0x;",
            "const GLenum SIGN_AFTER_PREFIX = 0x-1;",
            "const GLenum PLUS = +1;",
            "const GLenum WITHOUT_VALUE = ;",
            "const GLenum WITHOUT_VALUE;",
            "const GLenum = 1;",
            "const unsigned long TYPE_OF_TWO_WORDS = 1;",
            "const GLenum NOT-A-NAME = 1;",
            "const GLenum UNFINISHED = 1",
        ] {
            assert_eq!(written_constants(&[statement]).unwrap_err(), unreadable(statement));
        }
        assert_eq!(
            written_constants(&["interface A {\n    const GLenum UNFINISHED = 1\n};"]).unwrap_err(),
            unreadable("const GLenum UNFINISHED = 1 }")
        );
        assert_eq!(
            written_constants(&["interface A {\n    const GLenum\n        FIRST = 1,\n        SECOND = 2;\n};"])
                .unwrap_err(),
            unreadable("const GLenum FIRST = 1, SECOND = 2;")
        );
        assert_eq!(
            written_constants(&["const GLfloat HALF = 0.5;"]).unwrap_err(),
            "the WebGL constant HALF has the type GLfloat, which has no Rust type yet"
        );
    }
}
