/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use flapc::{Architecture, CompilationUnit, CompileOptions, Compiler, ObjectFormat, SourceInput, Target};

fn compile(source: &str, architecture: Architecture) -> Result<String, String> {
    Compiler::new(CompileOptions {
        target: Target {
            architecture,
            object_format: ObjectFormat::Elf,
        },
        has_jscvt: false,
        enable_assertions: true,
        profiling: false,
    })
    .compile(CompilationUnit {
        source: SourceInput {
            name: "closures.flap",
            contents: source,
        },
        constants: Some(SourceInput {
            name: "layout.conf",
            contents: include_str!("interpreter-layout.conf"),
        }),
    })
    .map(|assembly| assembly.as_str().to_owned())
    .map_err(|error| format!("{error:?}"))
}

fn assert_compiles(source: &str) {
    for architecture in [Architecture::X86_64, Architecture::Aarch64] {
        if let Err(error) = compile(source, architecture) {
            panic!("{error}\n{source}");
        }
    }
}

#[test]
fn compiles_closures_that_no_path_reaches() {
    // The body of an unused closure is unreachable. Inlining the call in it
    // splits the block, which must not make the uses after the call invalid.
    assert_compiles(
        r#"
inline fn note(value: Value) {
}

handler Unused(dst: out Operand, src: in Operand) {
    let slow = || @cold {
        exit;
    };
    let unused = |raw: i32| {
        let produced: Value = box_i32(raw);
        note(produced);
        store(dst, produced);
        dispatch_next;
    };
    let unused_with_label = |raw: i32| {
        guard let Value<i32>(value) = load(src) else slow;
        store(dst, box_i32(value + raw));
        dispatch_next;
    };
    store(dst, load(src));
    dispatch_next;
}
"#,
    );
}

#[test]
fn compiles_closures_passed_as_the_failure_label_of_an_inline_function() {
    // The closure is entered from inside the inline function, and its body
    // passes a label of its own to another inline function call.
    assert_compiles(
        r#"
inline fn check(value: i32, else fail: Label) -> i32 {
    guard value != 0 else fail;
    value
}

inline fn pick(value: i32, other: Label, else fail: Label) -> i32 {
    guard value != 1 else fail;
    branch_eq(value, 2, other);
    value
}

handler Checked(dst: out Operand, src: in Operand) {
    let slow = || @cold {
        exit;
    };
    let other = || @cold {
        exit;
    };
    let fallback = |raw: i32| {
        guard let picked = pick(raw, other) else slow;
        store(dst, box_i32(picked));
        dispatch_next;
    };
    guard let Value<i32>(value) = load(src) else slow;
    guard let checked = check(value) else fallback(value);
    store(dst, box_i32(checked));
    dispatch_next;
}
"#,
    );
}
