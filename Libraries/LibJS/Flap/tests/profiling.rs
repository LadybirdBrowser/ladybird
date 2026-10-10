/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use flapc::{Architecture, CompilationUnit, CompileOptions, Compiler, ObjectFormat, SourceInput, Target};

const PROGRAM: &str = "
inline fn count_check() @profiling {
    let vm = load_vm();
    or8_mem([vm, VM_EXECUTION_GENERATION], 2);
    or32_mem([vm, VM_EXECUTION_GENERATION], 0x30000);
    sub32_mem([vm, VM_EXECUTION_GENERATION], 5);
}

handler Check() {
    count_check();
    dispatch_next;
}
";

fn compile(architecture: Architecture, profiling: bool) -> String {
    Compiler::new(CompileOptions {
        target: Target {
            architecture,
            object_format: ObjectFormat::Elf,
        },
        has_jscvt: false,
        enable_assertions: true,
        profiling,
    })
    .compile(CompilationUnit {
        source: SourceInput {
            name: "profiling.flap",
            contents: PROGRAM,
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

/// The hot code of the Check handler.
fn check_handler(assembly: &str) -> &str {
    let start = assembly.find("asm_handler_Check:").expect("the handler is emitted");
    let end = assembly.find("asm_handler_Check_hot_end:").expect("the handler ends");
    &assembly[start..end]
}

#[test]
fn compiles_profiling_code_only_into_the_profiling_variant() {
    for architecture in [Architecture::X86_64, Architecture::Aarch64] {
        let plain = compile(architecture, false);
        assert!(plain.contains("CSYM(js_interpreter):"), "{plain}");
        assert!(!plain.contains("_profiling"), "{plain}");
        assert!(!check_handler(&plain).contains("48]"), "{plain}");

        let profiling = compile(architecture, true);
        assert!(profiling.contains("CSYM(js_interpreter_profiling):"), "{profiling}");
        assert!(
            profiling.contains("CSYM(js_interpreter_handler_ranges_profiling):"),
            "{profiling}"
        );
        assert!(!profiling.contains("CSYM(js_interpreter):"), "{profiling}");
        assert!(check_handler(&profiling).contains("48]"), "{profiling}");
    }
}

#[test]
fn updates_memory_with_one_instruction_on_x86_64() {
    let assembly = compile(Architecture::X86_64, true);
    let handler = check_handler(&assembly);
    for instruction in ["or BYTE PTR [", "or DWORD PTR [", "sub DWORD PTR ["] {
        assert_eq!(handler.matches(instruction).count(), 1, "{instruction}\n{handler}");
    }
}

#[test]
fn updates_memory_through_scratch_registers_on_aarch64() {
    let assembly = compile(Architecture::Aarch64, true);
    let handler = check_handler(&assembly);
    for (instruction, count) in [
        ("ldrb w9, [x0, #48]", 1),
        ("orr w9, w9, #0x2", 1),
        ("strb w9, [x0, #48]", 1),
        ("ldr w9, [x0, #48]", 2),
        ("orr w9, w9, #0x30000", 1),
        ("sub x9, x9, #5", 1),
        ("str w9, [x0, #48]", 2),
    ] {
        assert_eq!(handler.matches(instruction).count(), count, "{instruction}\n{handler}");
    }
}
