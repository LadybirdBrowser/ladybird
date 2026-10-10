/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use flapc::{Architecture, CompilationUnit, CompileOptions, Compiler, ObjectFormat, SourceInput, Target};

const PROGRAM: &str = "
inline fn count_check() @profiling {
    let vm = load_vm();
    inc32_mem([vm, VM_EXECUTION_GENERATION]);
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
