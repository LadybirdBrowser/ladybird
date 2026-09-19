/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use flapc::{Architecture, CompilationUnit, CompileOptions, Compiler, ObjectFormat, SourceInput, Target};

fn compile(body: &str, architecture: Architecture, enabled: bool) -> String {
    let source = format!("handler Check(input: in Operand) {{ {body} dispatch_next; }}");
    Compiler::new(CompileOptions {
        target: Target {
            architecture,
            object_format: ObjectFormat::Elf,
        },
        has_jscvt: false,
        enable_assertions: enabled,
    })
    .compile(CompilationUnit {
        source: SourceInput {
            name: "assertions.flap",
            contents: &source,
        },
        constants: Some(SourceInput {
            name: "layout.conf",
            contents: include_str!("interpreter-layout.conf"),
        }),
    })
    .unwrap()
    .as_str()
    .to_owned()
}

fn traps(assembly: &str, architecture: Architecture) -> usize {
    assembly
        .matches(match architecture {
            Architecture::X86_64 => "    ud2",
            Architecture::Aarch64 => "    brk",
        })
        .count()
}

#[test]
fn removes_assertions_proven_by_constants_ranges_and_successful_guards() {
    for architecture in [Architecture::X86_64, Architecture::Aarch64] {
        for body in [
            "assert(true);",
            "assert(!false);",
            "let number = unbox_i32(load(input)); assert(extract_tag(box_i32(number)) == INT32_TAG);",
            "let number: u64 = 42; assert(number > 0);",
            "let number = unbox_i32(load(input)); let masked = number & 255; assert(u32(masked) < 256);",
            "let number = unbox_i32(load(input)); guard number < 10 else @cold { exit; }; assert(number < 10);",
        ] {
            let assembly = compile(body, architecture, true);
            assert_eq!(traps(&assembly, architecture), 0, "{body}\n{assembly}");
        }
    }
}

#[test]
fn preserves_dynamic_and_always_false_assertions() {
    for architecture in [Architecture::X86_64, Architecture::Aarch64] {
        for body in [
            "assert(false);",
            "let number = unbox_i32(load(input)); assert(!(number < 10));",
            "let number = unbox_i32(load(input)); assert(number has 1);",
            "let number = i32(0xffffffff); assert(u32(number) < 128);",
            "let number = unbox_i32(load(input)); let masked = number & 0x80000000; assert(masked >= 0);",
            "let number = unbox_i32(load(input)); if number < 10 { } else { assert(number < 10); }",
            "let number = unbox_i32(load(input)); assert(number < 10);",
            "assert(extract_tag(load(input)) == STRING_TAG);",
            "let number = unbox_i32(load(input)); if number < 10 { let ignored: u64 = 1; } assert(number < 10);",
        ] {
            let assembly = compile(body, architecture, true);
            assert_eq!(traps(&assembly, architecture), 1, "{body}\n{assembly}");
            let (normal, _) = assembly.split_once("asm_assertion_failure_traps:").unwrap();
            assert_eq!(traps(normal, architecture), 0);
            assert_eq!(traps(&compile(body, architecture, false), architecture), 0);
        }
    }
}

#[test]
fn lowers_comparison_assertions_directly_to_failure_branches() {
    for (architecture, branch, boolean_materialization) in [
        (Architecture::X86_64, "    jge ", "    set"),
        (Architecture::Aarch64, "    b.ge ", "    cset "),
    ] {
        let assembly = compile(
            "let number = unbox_i32(load(input)); assert(number < 10);",
            architecture,
            true,
        );
        assert!(
            assembly
                .lines()
                .any(|line| line.starts_with(branch) && line.contains("assert_predicate_failure")),
            "{assembly}"
        );
        assert!(!assembly.contains(boolean_materialization), "{assembly}");
    }
}

#[test]
fn aarch64_bit_assertions_fall_through_on_success() {
    let assembly = compile(
        "let number = unbox_i32(load(input)); assert(number has 1);",
        Architecture::Aarch64,
        true,
    );
    assert!(assembly.contains("    tst "));
    assert!(
        assembly
            .lines()
            .any(|line| line.starts_with("    b.eq ") && line.contains("assert_predicate_failure"))
    );
    let handler = assembly
        .split_once("asm_handler_Check:")
        .unwrap()
        .1
        .split_once("asm_handler_Check_hot_end:")
        .unwrap()
        .0;
    assert!(!handler.contains("    tbz ") && !handler.contains("    tbnz "));
}

#[test]
fn folds_layout_value_tags_into_immediate_comparisons() {
    let assembly = compile(
        "assert(extract_tag(load(input)) != extract_tag(Value<Empty>));",
        Architecture::X86_64,
        true,
    );
    assert!(
        assembly
            .lines()
            .any(|line| line.starts_with("    cmp ") && line.ends_with(", 32763")),
        "{assembly}"
    );
    assert!(
        !assembly
            .lines()
            .any(|line| line.starts_with("    mov ") && line.ends_with(", 32763"))
    );
}

#[test]
fn reuses_tag_extractions_across_assertions() {
    for architecture in [Architecture::X86_64, Architecture::Aarch64] {
        for (body, expected_traps) in [
            (
                "let value = load(input); assert(extract_tag(value) == STRING_TAG); assert(extract_tag(value) == STRING_TAG);",
                1,
            ),
            (
                "let tag = extract_tag(load(input)); assert(tag == STRING_TAG); assert(tag == STRING_TAG);",
                1,
            ),
            (
                "let value = load(input); assert(extract_tag(value) != STRING_TAG); assert(extract_tag(value) != INT32_TAG);",
                2,
            ),
        ] {
            let assembly = compile(body, architecture, true);
            assert_eq!(traps(&assembly, architecture), expected_traps, "{body}\n{assembly}");
            let shift = match architecture {
                Architecture::X86_64 => "    shr ",
                Architecture::Aarch64 => "    lsr ",
            };
            assert_eq!(
                assembly.lines().filter(|line| line.starts_with(shift)).count(),
                1,
                "{body}\n{assembly}"
            );
        }
    }
}
