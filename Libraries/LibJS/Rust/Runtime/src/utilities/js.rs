/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! js-rust: runs scripts on the Rust runtime, with the command line of Utilities/js.cpp.

use core::ffi::{c_char, c_int};
use std::ffi::CStr;
use std::io::{IsTerminal, Write};

use crate::interpreter::runtime_functions::unimplemented_runtime_function;
use crate::interpreter::vm::Vm;
use crate::parser_error::ParserError;
use crate::runtime::print::{PrintContext, print_value};
use crate::script::Script;
use libjs_rust::ast::ProgramType;
use libjs_rust::compile::parse;

#[derive(Default)]
struct Options {
    script_source: Option<String>,
    script_paths: Vec<String>,
    print_last_result: bool,
    dump_ast: bool,
    parse_only: bool,
    as_module: bool,
    disable_ansi_colors: bool,
    gc_on_every_allocation: bool,
}

fn parse_options(arguments: &[String]) -> Result<Options, String> {
    let mut options = Options::default();
    let mut arguments = arguments.iter().skip(1);
    while let Some(argument) = arguments.next() {
        match argument.as_str() {
            "-c" | "--evaluate" => {
                options.script_source = Some(arguments.next().ok_or("--evaluate needs a script")?.clone());
            }
            "-l" | "--print-last-result" => options.print_last_result = true,
            "-A" | "--dump-ast" => options.dump_ast = true,
            "-p" | "--parse-only" => options.parse_only = true,
            "-m" | "--as-module" => options.as_module = true,
            "-i" | "--disable-ansi-colors" => options.disable_ansi_colors = true,
            "-g" | "--gc-on-every-allocation" => options.gc_on_every_allocation = true,
            _ if argument.starts_with('-') => return Err(format!("unknown option {argument}")),
            _ => options.script_paths.push(argument.clone()),
        }
    }
    Ok(options)
}

fn read_source(options: &Options) -> Result<String, String> {
    if let Some(source) = &options.script_source {
        return Ok(source.clone());
    }
    if options.script_paths.is_empty() {
        return Err("no script given".to_string());
    }
    let mut source = String::new();
    for path in &options.script_paths {
        let bytes = std::fs::read(path).map_err(|error| format!("{path}: {error}"))?;
        if !source.is_empty() {
            source.push('\n');
        }
        source.push_str(&String::from_utf8_lossy(&bytes));
    }
    Ok(source)
}

fn report_parse_errors(output: &mut impl Write, errors: &[ParserError]) {
    for error in errors {
        let _ = writeln!(output, "{error}");
    }
}

fn run(options: &Options, output: &mut impl Write) -> c_int {
    let source = match read_source(options) {
        Ok(source) => source,
        Err(error) => {
            eprintln!("js-rust: {error}");
            return 1;
        }
    };
    let source: Vec<u16> = source.encode_utf16().collect();
    let program_type = if options.as_module {
        ProgramType::Module
    } else {
        ProgramType::Script
    };
    let mut parsed = parse(&source, program_type, 1);
    if parsed.has_errors() {
        report_parse_errors(output, &ParserError::all_from_parsed_program(&parsed));
        return 1;
    }
    if options.dump_ast {
        let _ = writeln!(output, "{}", parsed.ast_dump().trim_end());
    }
    if options.parse_only {
        return 0;
    }
    if options.as_module {
        unimplemented_runtime_function("running modules", 0);
    }
    let script = Script::compile_parsed_program(parsed, source.len());

    let vm = Vm::create();
    if options.gc_on_every_allocation {
        vm.heap().set_should_collect_on_every_allocation(true);
    }
    let print_context = PrintContext {
        strip_ansi: options.disable_ansi_colors,
    };
    match vm.run_script(script) {
        Ok(value) => {
            if options.print_last_result {
                let mut text = String::new();
                print_value(&mut text, &print_context, value);
                let _ = writeln!(output, "{text}");
            }
            0
        }
        Err(exception) => {
            let _ = output.flush();
            let mut text = String::new();
            print_value(&mut text, &print_context, exception.value());
            eprintln!("Uncaught exception: \n{text}");
            1
        }
    }
}

/// The entry point of js-rust, called from its C++ main.
///
/// # Safety
///
/// `argv` must hold `argc` NUL-terminated strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn libjs_runtime_rust_js_main(argc: c_int, argv: *const *const c_char) -> c_int {
    let arguments: Vec<String> = (0..usize::try_from(argc).unwrap_or(0))
        // SAFETY: The caller passes argc valid strings.
        .map(|index| {
            unsafe { CStr::from_ptr(*argv.add(index)) }
                .to_string_lossy()
                .into_owned()
        })
        .collect();
    let options = match parse_options(&arguments) {
        Ok(options) => options,
        Err(error) => {
            eprintln!("js-rust: {error}");
            return 2;
        }
    };
    // Like C stdio, buffer whole blocks when stdout is not a terminal, so that output written before a crash or a
    // dump to stderr keeps the order the C++ js produces.
    let stdout = std::io::stdout();
    if stdout.is_terminal() {
        run(&options, &mut stdout.lock())
    } else {
        let mut output = std::io::BufWriter::with_capacity(64 * 1024, stdout.lock());
        let result = run(&options, &mut output);
        let _ = output.flush();
        result
    }
}
