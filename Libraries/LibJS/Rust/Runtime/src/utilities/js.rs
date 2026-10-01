/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! js-rust: runs scripts on the Rust runtime, with the command line of Utilities/js.cpp.

use core::ffi::{c_char, c_int};
use std::ffi::CStr;
use std::io::{self, IsTerminal, Read, Write};

use ak::Utf16String;

use crate::interpreter::run::set_dump_bytecode;
use crate::interpreter::runtime_functions::unimplemented_runtime_function;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::value::Value;
use crate::parser_error::ParserError;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::{Error, ErrorKind};
use crate::runtime::error_data::CompactTraceback;
use crate::runtime::print::{PrintContext, print};
use crate::runtime::realm::Realm;
use crate::script::Script;
use crate::source_code::SourceCode;
use crate::utf16::{Utf16View, utf16_from_wtf8};
use crate::utilities::initialize_realm;
use libjs_rust::ast::ProgramType;
use libjs_rust::compile::parse;

#[derive(Default)]
struct Options {
    show_help: bool,
    show_version: bool,
    parse_only: bool,
    dump_ast: bool,
    dump_bytecode: bool,
    as_module: bool,
    print_last_result: bool,
    strip_ansi: bool,
    disable_source_location_hints: bool,
    gc_on_every_allocation: bool,
    raw_strings: bool,
    disable_syntax_highlight: bool,
    disable_debug_printing: bool,
    debug: bool,
    evaluate_script: String,
    use_test262_global: bool,
    script_paths: Vec<String>,
}

#[derive(Clone, Copy)]
enum OptionTarget {
    ShowHelp,
    ShowVersion,
    ParseOnly,
    DumpAst,
    DumpBytecode,
    AsModule,
    PrintLastResult,
    StripAnsi,
    DisableSourceLocationHints,
    GcOnEveryAllocation,
    RawStrings,
    DisableSyntaxHighlight,
    DisableDebugPrinting,
    Debug,
    EvaluateScript,
    UseTest262Global,
}

/// An option as Core::ArgsParser describes it. Options with a value name take a value.
struct OptionDescription {
    help_string: &'static str,
    long_name: &'static str,
    short_name: Option<u8>,
    value_name: Option<&'static str>,
    shown_in_synopsis: bool,
    target: OptionTarget,
}

const fn option(
    help_string: &'static str,
    long_name: &'static str,
    short_name: Option<u8>,
    target: OptionTarget,
) -> OptionDescription {
    OptionDescription {
        help_string,
        long_name,
        short_name,
        value_name: None,
        shown_in_synopsis: true,
        target,
    }
}

/// The options in the order the C++ js registers them, after the two every Core::ArgsParser has.
const OPTIONS: &[OptionDescription] = &[
    OptionDescription {
        shown_in_synopsis: false,
        ..option("Display help message and exit", "help", None, OptionTarget::ShowHelp)
    },
    OptionDescription {
        shown_in_synopsis: false,
        ..option("Print version", "version", None, OptionTarget::ShowVersion)
    },
    option("Parse only", "parse-only", Some(b'p'), OptionTarget::ParseOnly),
    option("Dump the AST", "dump-ast", Some(b'A'), OptionTarget::DumpAst),
    option(
        "Dump the bytecode",
        "dump-bytecode",
        Some(b'd'),
        OptionTarget::DumpBytecode,
    ),
    option("Treat as module", "as-module", Some(b'm'), OptionTarget::AsModule),
    option(
        "Print last result",
        "print-last-result",
        Some(b'l'),
        OptionTarget::PrintLastResult,
    ),
    option(
        "Disable ANSI colors",
        "disable-ansi-colors",
        Some(b'i'),
        OptionTarget::StripAnsi,
    ),
    option(
        "Disable source location hints",
        "disable-source-location-hints",
        Some(b'h'),
        OptionTarget::DisableSourceLocationHints,
    ),
    option(
        "GC on every allocation",
        "gc-on-every-allocation",
        Some(b'g'),
        OptionTarget::GcOnEveryAllocation,
    ),
    option(
        "Display strings without quotes or escape sequences",
        "raw-strings",
        Some(b'r'),
        OptionTarget::RawStrings,
    ),
    option(
        "Disable live syntax highlighting",
        "no-syntax-highlight",
        Some(b's'),
        OptionTarget::DisableSyntaxHighlight,
    ),
    option(
        "Disable debug output",
        "disable-debug-output",
        None,
        OptionTarget::DisableDebugPrinting,
    ),
    option("Run with the JavaScript debugger", "debug", None, OptionTarget::Debug),
    OptionDescription {
        value_name: Some("script"),
        ..option(
            "Evaluate argument as a script",
            "evaluate",
            Some(b'c'),
            OptionTarget::EvaluateScript,
        )
    },
    option(
        "Use test262 global ($262)",
        "use-test262-global",
        None,
        OptionTarget::UseTest262Global,
    ),
];

const GENERAL_HELP: &str = "This is a JavaScript interpreter.";
const POSITIONAL_ARGUMENT_NAME: &str = "scripts";
const POSITIONAL_ARGUMENT_HELP: &str = "Path to script files";

impl OptionDescription {
    fn name_for_display(&self) -> String {
        format!("--{}", self.long_name)
    }

    fn accept_value(&self, options: &mut Options, value: Option<&str>) {
        match self.target {
            OptionTarget::ShowHelp => options.show_help = true,
            OptionTarget::ShowVersion => options.show_version = true,
            OptionTarget::ParseOnly => options.parse_only = true,
            OptionTarget::DumpAst => options.dump_ast = true,
            OptionTarget::DumpBytecode => options.dump_bytecode = true,
            OptionTarget::AsModule => options.as_module = true,
            OptionTarget::PrintLastResult => options.print_last_result = true,
            OptionTarget::StripAnsi => options.strip_ansi = true,
            OptionTarget::DisableSourceLocationHints => options.disable_source_location_hints = true,
            OptionTarget::GcOnEveryAllocation => options.gc_on_every_allocation = true,
            OptionTarget::RawStrings => options.raw_strings = true,
            OptionTarget::DisableSyntaxHighlight => options.disable_syntax_highlight = true,
            OptionTarget::DisableDebugPrinting => options.disable_debug_printing = true,
            OptionTarget::Debug => options.debug = true,
            OptionTarget::EvaluateScript => options.evaluate_script = value.unwrap_or_default().to_string(),
            OptionTarget::UseTest262Global => options.use_test262_global = true,
        }
    }
}

/// ArgsParser::print_usage_terminal().
fn print_usage(file: &mut dyn Write, argv0: &str) -> io::Result<()> {
    write!(file, "Usage:\n\t\x1b[1m{argv0}\x1b[0m")?;
    for option in OPTIONS.iter().filter(|option| option.shown_in_synopsis) {
        match option.value_name {
            Some(value_name) => write!(file, " [{} {value_name}]", option.name_for_display())?,
            None => write!(file, " [{}]", option.name_for_display())?,
        }
    }
    writeln!(file, " [{POSITIONAL_ARGUMENT_NAME}...]")?;

    writeln!(file, "\nDescription:")?;
    writeln!(file, "{GENERAL_HELP}")?;

    writeln!(file, "\nOptions:")?;
    for option in OPTIONS {
        write!(file, "\t")?;
        if let Some(short_name) = option.short_name {
            write!(file, "\x1b[1m-{}\x1b[0m", char::from(short_name))?;
            if let Some(value_name) = option.value_name {
                write!(file, " {value_name}")?;
            }
            write!(file, ", ")?;
        }
        write!(file, "\x1b[1m--{}\x1b[0m", option.long_name)?;
        if let Some(value_name) = option.value_name {
            write!(file, " {value_name}")?;
        }
        writeln!(file, "\t{}", option.help_string)?;
    }

    writeln!(file, "\nArguments:")?;
    writeln!(
        file,
        "\t\x1b[1m{POSITIONAL_ARGUMENT_NAME}\x1b[0m\t{POSITIONAL_ARGUMENT_HELP}"
    )
}

/// Core::ArgsParser::parse() on the C++ js's options, with AK::OptionParser's getopt rules: options and scripts may
/// come in any order, short options may be grouped, values follow their option or are attached to it, and `--` ends
/// the options. Returns the exit code instead when it fails, or when it shows the help or the version.
fn parse_arguments(arguments: &[String], output: &mut dyn Write) -> Result<Options, c_int> {
    let argv0 = arguments.first().map_or("<exe>", String::as_str);
    let fail = || -> c_int {
        let _ = print_usage(&mut io::stderr(), argv0);
        1
    };

    let mut options = Options::default();
    let mut index = 1;
    while index < arguments.len() {
        let argument = arguments[index].as_str();
        index += 1;
        if argument == "--" {
            options.script_paths.extend(arguments[index..].iter().cloned());
            break;
        }
        // Anything that doesn't start with a "-" is not an option.
        // As a special case, a single "-" is not an option either.
        if !argument.starts_with('-') || argument == "-" {
            options.script_paths.push(argument.to_string());
            continue;
        }

        if let Some(long_argument) = argument.strip_prefix("--") {
            let found = OPTIONS.iter().find_map(|option| {
                let rest = long_argument.strip_prefix(option.long_name)?;
                if rest.is_empty() {
                    return Some((option, None));
                }
                rest.strip_prefix('=').map(|value| (option, Some(value)))
            });
            let Some((option, value)) = found else {
                eprintln!("Unrecognized option \x1b[1m{argument}\x1b[22m");
                return Err(fail());
            };
            let value = match (option.value_name, value) {
                (None, Some(_)) => {
                    eprintln!(
                        "Option \x1b[1m--{}\x1b[22m doesn't accept an argument",
                        option.long_name
                    );
                    return Err(fail());
                }
                (None, None) => None,
                (Some(_), Some(value)) => Some(value),
                (Some(_), None) => {
                    let Some(value) = arguments.get(index) else {
                        eprintln!("Missing value for option \x1b[1m--{}\x1b[22m", option.long_name);
                        return Err(fail());
                    };
                    index += 1;
                    Some(value.as_str())
                }
            };
            option.accept_value(&mut options, value);
            continue;
        }

        let short_options = &argument.as_bytes()[1..];
        for (position, &short_name) in short_options.iter().enumerate() {
            let Some(option) = OPTIONS.iter().find(|option| option.short_name == Some(short_name)) else {
                eprintln!(
                    "Unrecognized option \x1b[1m-{}\x1b[22m",
                    String::from_utf8_lossy(&[short_name])
                );
                return Err(fail());
            };
            if option.value_name.is_none() {
                option.accept_value(&mut options, None);
                continue;
            }
            // The rest of the argument is the value, the "-ovalue" syntax, or else the next argument is.
            let attached_value = &argument[position + 2..];
            if !attached_value.is_empty() {
                option.accept_value(&mut options, Some(attached_value));
            } else if let Some(value) = arguments.get(index) {
                index += 1;
                option.accept_value(&mut options, Some(value));
            } else {
                eprintln!("Missing value for option \x1b[1m-{}\x1b[22m", char::from(short_name));
                return Err(fail());
            }
            break;
        }
    }

    if options.show_version {
        let _ = writeln!(output, "Version 1.0");
        return Err(0);
    }
    if options.show_help {
        let _ = print_usage(output, argv0);
        return Err(0);
    }
    Ok(options)
}

/// print() in the C++ js: the value, then a newline.
fn print_value_to(vm: &Vm, options: &Options, stream: &mut dyn Write, value: Value) -> io::Result<()> {
    let mut print_context = PrintContext {
        vm,
        stream,
        strip_ansi: options.strip_ansi,
        raw_strings: options.raw_strings,
    };
    print(value, &mut print_context)?;
    stream.write_all(b"\n")
}

/// error->stack_string(JS::CompactTraceback::Yes) of a thrown Error, which the C++ js prints after the error.
fn stack_string_of_thrown_error(thrown_value: Value) -> Option<Utf16String> {
    if !thrown_value.is_object() {
        return None;
    }
    let error = thrown_value.as_object().downcast::<Error>()?;
    Some(error.stack_string(CompactTraceback::Yes))
}

fn handle_exception(vm: &Vm, options: &Options, thrown_value: Value) -> io::Result<()> {
    let mut stream = io::stderr().lock();
    stream.write_all(b"Uncaught exception: \n")?;
    print_value_to(vm, options, &mut stream, thrown_value)?;

    if let Some(stack_string) = stack_string_of_thrown_error(thrown_value) {
        stream.write_all(&Utf16View::of_string(&stack_string).to_wtf8())?;
        stream.write_all(b"\n")?;
    }
    Ok(())
}

fn parse_and_run(
    vm: &Vm,
    realm: Gc<Realm>,
    options: &Options,
    output: &mut dyn Write,
    source: &[u8],
    source_name: &str,
) -> bool {
    let mut result: ThrowCompletionOr<Value> = Ok(Value::UNDEFINED);
    // Like Utf16String::from_utf8(), this stops the process for a source that is not valid UTF-8, which the caller
    // has ruled out.
    let utf16_source = utf16_from_wtf8(source).expect("the source is valid UTF-8");

    let program_type = if options.as_module {
        ProgramType::Module
    } else {
        ProgramType::Script
    };
    let mut parsed = parse(&utf16_source, program_type, 1);
    if parsed.has_errors() {
        let error = ParserError::all_from_parsed_program(&parsed).swap_remove(0);
        let hint = error.source_location_hint(&utf16_source, b' ', b'^');
        if !hint.is_empty() {
            let _ = output.write_all(&Utf16View::Utf16(&hint).to_wtf8());
            let _ = output.write_all(b"\n");
        }

        let error_string = error.to_string();
        let _ = writeln!(output, "{error_string}");
        result = vm.throw_completion_with_message(ErrorKind::SyntaxError, error_string);
    } else {
        // NB: The C++ js dumps the AST in color unless -i is given, which the frontend only offers to standard output
        //     directly, so this dumps it without color.
        if options.dump_ast {
            let _ = writeln!(output, "{}", parsed.ast_dump());
        }
        if !options.as_module {
            let source_code = SourceCode::create(
                Utf16String::from_utf8(source_name),
                Utf16String::from_utf16(&utf16_source),
            );
            let script = Script::create_from_parsed(vm, parsed, source_code, realm);
            if !options.parse_only {
                result = vm.run_script(script, None);
            }
        } else if !options.parse_only {
            unimplemented_runtime_function("running modules", 0);
        }
    }

    match result {
        Err(throw) => {
            let _ = handle_exception(vm, options, throw.value());
            false
        }
        Ok(value) => {
            if options.print_last_result {
                let _ = output.flush();
                let _ = print_value_to(vm, options, output, value);
                let _ = output.flush();
            }
            true
        }
    }
}

fn report_runtime_error(syscall: &str, error: &io::Error) {
    let description = match error.raw_os_error() {
        Some(code) => {
            let message = io::Error::from_raw_os_error(code).to_string();
            let message = message
                .strip_suffix(&format!(" (os error {code})"))
                .unwrap_or(&message)
                .to_string();
            format!("{syscall}: {message} (errno={code})")
        }
        None => error.to_string(),
    };
    eprintln!("\x1b[31;1mRuntime error\x1b[0m: {description}");
}

/// What windows-1252 decodes the bytes 0x80 to 0x9F to; every other byte is its own code point.
const WINDOWS_1252_HIGH_CONTROLS: [char; 32] = [
    '\u{20AC}', '\u{0081}', '\u{201A}', '\u{0192}', '\u{201E}', '\u{2026}', '\u{2020}', '\u{2021}', '\u{02C6}',
    '\u{2030}', '\u{0160}', '\u{2039}', '\u{0152}', '\u{008D}', '\u{017D}', '\u{008F}', '\u{0090}', '\u{2018}',
    '\u{2019}', '\u{201C}', '\u{201D}', '\u{2022}', '\u{2013}', '\u{2014}', '\u{02DC}', '\u{2122}', '\u{0161}',
    '\u{203A}', '\u{0153}', '\u{009D}', '\u{017E}', '\u{0178}',
];

fn decode_utf16_with_replacement(bytes: &[u8], code_unit_from_bytes: fn([u8; 2]) -> u16) -> String {
    let (chunks, remainder) = bytes.as_chunks::<2>();
    let has_trailing_byte = !remainder.is_empty();
    let code_units = chunks.iter().map(|&chunk| code_unit_from_bytes(chunk));
    let mut output: String = char::decode_utf16(code_units)
        .map(|decoded| decoded.unwrap_or(char::REPLACEMENT_CHARACTER))
        .collect();
    if has_trailing_byte {
        output.push(char::REPLACEMENT_CHARACTER);
    }
    output
}

/// TextCodec::convert_input_to_utf8_using_given_decoder_unless_there_is_a_byte_order_mark() with the windows-1252
/// decoder.
fn convert_input_to_utf8_using_windows_1252_unless_there_is_a_byte_order_mark(input: &[u8]) -> String {
    if let Some(input) = input.strip_prefix(b"\xEF\xBB\xBF") {
        return String::from_utf8_lossy(input).into_owned();
    }
    if let Some(input) = input.strip_prefix(b"\xFE\xFF") {
        return decode_utf16_with_replacement(input, u16::from_be_bytes);
    }
    if let Some(input) = input.strip_prefix(b"\xFF\xFE") {
        return decode_utf16_with_replacement(input, u16::from_le_bytes);
    }
    input
        .iter()
        .map(|&byte| match byte {
            0x80..=0x9F => WINDOWS_1252_HIGH_CONTROLS[usize::from(byte - 0x80)],
            _ => char::from(byte),
        })
        .collect()
}

fn read_file(path: &str) -> Result<Vec<u8>, ()> {
    let mut file = std::fs::File::open(path).map_err(|error| report_runtime_error("open", &error))?;
    let mut file_contents = Vec::new();
    file.read_to_end(&mut file_contents)
        .map_err(|error| report_runtime_error("read", &error))?;
    Ok(file_contents)
}

fn ladybird_main(arguments: &[String], output: &mut dyn Write) -> c_int {
    let options = match parse_arguments(arguments, output) {
        Ok(options) => options,
        Err(exit_code) => return exit_code,
    };

    // NB: The -h, -s and --disable-debug-output options change nothing yet: the C++ js does not read the first, the
    //     second is for the REPL, and the runtime prints no debug output.
    set_dump_bytecode(options.dump_bytecode);

    let vm = Vm::create();
    // FIXME: Allow dynamic imports, once the runtime has modules.

    if options.debug {
        unimplemented_runtime_function("the JavaScript debugger, which --debug runs scripts in", 0);
    }

    // FIXME: Unless --disable-debug-output is given, warn about promises rejected without handlers and about handlers
    //        added to rejected promises, printing their results, once the runtime has promises.

    if options.evaluate_script.is_empty() && options.script_paths.is_empty() {
        unimplemented_runtime_function("the REPL, which js runs when it is given no script", 0);
    }

    if options.use_test262_global {
        unimplemented_runtime_function("the test262 global object, for --use-test262-global", 0);
    }
    // FIXME: Run scripts in a realm whose global object is a ScriptObject, with the global, loadINI, loadJSON, print and
    //        gc properties, once realms have intrinsics.
    let root_execution_context = initialize_realm(&vm);
    let realm = root_execution_context.realm();
    // FIXME: Give the realm's console object a client that prints like ReplConsoleClient, once realms have one.
    vm.heap()
        .set_should_collect_on_every_allocation(options.gc_on_every_allocation);

    let mut builder = Vec::new();
    let source_name = if options.evaluate_script.is_empty() {
        if options.script_paths.len() > 1 {
            eprintln!(
                "Warning: Multiple files supplied, this will concatenate the sources and resolve modules as if it was the first file"
            );
        }

        for path in &options.script_paths {
            let Ok(file_contents) = read_file(path) else {
                return 1;
            };
            if utf16_from_wtf8(&file_contents).is_some() {
                builder.extend_from_slice(&file_contents);
            } else {
                builder.extend_from_slice(
                    convert_input_to_utf8_using_windows_1252_unless_there_is_a_byte_order_mark(&file_contents)
                        .as_bytes(),
                );
            }
        }

        options.script_paths[0].as_str()
    } else {
        builder.extend_from_slice(options.evaluate_script.as_bytes());
        "eval"
    };

    // We resolve modules as if it is the first file

    if !parse_and_run(&vm, realm, &options, output, &builder, source_name) {
        return 1;
    }

    0
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
    // Like C stdio, buffer whole blocks when stdout is not a terminal, so that output written before a crash or a
    // dump to stderr keeps the order the C++ js produces.
    let stdout = io::stdout();
    if stdout.is_terminal() {
        ladybird_main(&arguments, &mut stdout.lock())
    } else {
        let mut output = io::BufWriter::with_capacity(64 * 1024, stdout.lock());
        let result = ladybird_main(&arguments, &mut output);
        let _ = output.flush();
        result
    }
}
