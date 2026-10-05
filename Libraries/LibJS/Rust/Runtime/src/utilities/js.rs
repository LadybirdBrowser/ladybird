/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! js: runs scripts on the runtime.

use core::cell::{Cell, RefCell};
use core::ffi::{c_char, c_int};
use core::ops::Deref;
use core::sync::atomic::{AtomicBool, AtomicI32, Ordering};
use std::ffi::{CStr, CString};
use std::io::{self, Read, Write};
#[cfg(unix)]
use std::os::unix::ffi::{OsStrExt, OsStringExt};
#[cfg(unix)]
use std::os::unix::fs::OpenOptionsExt;

use ak::{Utf16FlyString, Utf16String};
use libjs_runtime_macros::Trace;

use crate::breakpoint::Breakpoint;
use crate::console::{Console, ConsoleClient, ConsoleClientMethods, LogLevel, PrinterArguments};
use crate::contrib::test262::global_object::Test262GlobalObject;
use crate::debugger::{Debugger, PauseInfo, PauseReason, ResumeMode};
use crate::gc::class::{GcCell, define_cell};
use crate::gc::root::Root;
use crate::hash_table::HashTable;
use crate::interpreter::run::set_dump_bytecode;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::lexical_path;
use crate::parser_error::ParserError;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::error::{Error, ErrorKind};
use crate::runtime::error_data::CompactTraceback;
use crate::runtime::error_types::ErrorType;
use crate::runtime::global_object::GlobalObject;
use crate::runtime::json_object::JSONObject;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{ORDINARY_OBJECT_METHODS, PropertyLookupPhase, allocate_object, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::print::{PrintContext, print};
use crate::runtime::promise::Promise;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::source_text_module::SourceTextModule;
use crate::script::Script;
use crate::source_code::SourceCode;
use crate::standard_output::{self, StandardOutputWriter, UnbufferedWriter};
use crate::utf16::{Utf16View, string_from_utf8_with_replacement_character, utf16_formatted, utf16_from_wtf8};
use crate::utilities::initialize_realm_with_global_object;
use crate::utilities::line_editor::{self, JSLineEditor};
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

/// s_strip_ansi and s_raw_strings of the C++ js, which the native functions of its global objects print with.
static STRIP_ANSI: AtomicBool = AtomicBool::new(false);
static RAW_STRINGS: AtomicBool = AtomicBool::new(false);

/// s_repl_line_level, s_keep_running_repl and s_exit_code of the C++ js. The line level counts the brackets that the
/// lines read so far have left open, and is not reset between pieces.
static REPL_LINE_LEVEL: AtomicI32 = AtomicI32::new(0);
static KEEP_RUNNING_REPL: AtomicBool = AtomicBool::new(true);
static EXIT_CODE: AtomicI32 = AtomicI32::new(0);

thread_local! {
    /// g_repl_statements of the C++ js: the pieces of input the REPL has run, which save() writes.
    static REPL_STATEMENTS: RefCell<Vec<Vec<u8>>> = const { RefCell::new(Vec::new()) };
    /// g_last_value of the C++ js: the completion value of the last script that ran, which `_` of the REPL reads.
    static LAST_VALUE: RefCell<Option<Root<'static, Value>>> = const { RefCell::new(None) };
}

/// The Error of AK's String::from_utf8() for bytes that are not UTF-8.
const STRING_FROM_UTF8_ERROR: &str = "String::from_utf8: Input was not valid UTF-8";

/// The global object of the realm the REPL runs its input in.
#[repr(C)]
#[derive(Trace)]
pub struct ReplObject {
    base: GlobalObject,
}

define_object_class!(ReplObject, extends: [GlobalObject, Object], methods: {
    initialize: ReplObject::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl ReplObject {
    pub fn allocate(vm: &Vm, realm: Gc<Realm>) -> Gc<ReplObject> {
        allocate_object(
            vm,
            ReplObject {
                base: GlobalObject::new(vm, Self::CLASS, realm),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let base_initialize = GlobalObject::CLASS
            .object_methods
            .expect("GlobalObject is an object class")
            .initialize;
        base_initialize(object, vm, realm);

        object.define_direct_property(
            vm,
            &key("global"),
            Value::from_object(object.as_gc()),
            PropertyAttributes::new(Attribute::ENUMERABLE),
        );
        let attr = PropertyAttributes::new(Attribute::CONFIGURABLE | Attribute::WRITABLE | Attribute::ENUMERABLE);
        let define = |name: &str, function, length| {
            object.define_native_function(vm, realm, &key(name), function, length, attr, None);
        };
        define("exit", raw_native!(ReplObject::exit_interpreter), 0);
        define("help", raw_native!(ReplObject::repl_help), 0);
        define("save", raw_native!(ReplObject::save_to_file), 1);
        define("loadINI", raw_native!(ReplObject::load_ini), 1);
        define("loadJSON", raw_native!(ReplObject::load_json), 1);
        define("print", raw_native!(ReplObject::print), 1);
        define("gc", raw_native!(ReplObject::gc), 0);

        object.define_native_accessor(
            vm,
            realm,
            &vm.names.underscore,
            raw_native!(ReplObject::last_value_getter),
            raw_native!(ReplObject::last_value_setter),
            attr,
        );
    }

    #[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
    fn last_value_getter(_vm: &Vm) -> ThrowCompletionOr<Value> {
        Ok(LAST_VALUE.with_borrow(|last_value| last_value.as_ref().map_or(Value::UNDEFINED, Root::value)))
    }

    fn last_value_setter(vm: &Vm) -> ThrowCompletionOr<Value> {
        let global_object = vm.get_global_object();
        assert!(global_object.downcast::<ReplObject>().is_some());
        standard_output::outln(b"Disable writing last value to '_'");

        // We must delete first otherwise this setter gets called recursively.
        global_object.internal_delete(vm, &vm.names.underscore)?;

        let value = vm.argument(0);
        global_object.internal_set(
            vm,
            &vm.names.underscore,
            value,
            Value::from_object(global_object),
            None,
            PropertyLookupPhase::OwnProperty,
        )?;
        Ok(value)
    }

    fn save_to_file(vm: &Vm) -> ThrowCompletionOr<Value> {
        if vm.argument_count() == 0 {
            return Ok(Value::from_bool(false));
        }
        let save_path = Utf16View::of_string(&vm.argument(0).to_utf16_string(vm)?).to_wtf8();
        Ok(Value::from_bool(write_to_file(&save_path).is_ok()))
    }

    fn exit_interpreter(vm: &Vm) -> ThrowCompletionOr<Value> {
        if vm.argument_count() != 0 {
            // NB: The C++ js converts the double to the int s_exit_code, which on AArch64 saturates and takes NaN to
            //     0, like this cast.
            EXIT_CODE.store(vm.argument(0).to_double(vm)? as i32, Ordering::Relaxed);
        }

        KEEP_RUNNING_REPL.store(false, Ordering::Relaxed);
        Ok(Value::UNDEFINED)
    }

    #[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
    fn repl_help(_vm: &Vm) -> ThrowCompletionOr<Value> {
        eprintln!("REPL commands:");
        eprintln!("    exit(code): exit the REPL with specified code. Defaults to 0.");
        eprintln!("    help(): display this menu");
        eprintln!("    loadINI(file): load the given file as INI.");
        eprintln!("    loadJSON(file): load the given file as JSON.");
        eprintln!("    print(value): pretty-print the given JS value.");
        eprintln!("    save(file): write REPL input history to the given file. For example: save(\"foo.txt\")");
        Ok(Value::UNDEFINED)
    }

    fn load_ini(vm: &Vm) -> ThrowCompletionOr<Value> {
        load_ini_impl(vm)
    }

    fn load_json(vm: &Vm) -> ThrowCompletionOr<Value> {
        load_json_impl(vm)
    }

    fn print(vm: &Vm) -> ThrowCompletionOr<Value> {
        print_all_arguments_or_throw(vm)
    }

    #[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
    fn gc(vm: &Vm) -> ThrowCompletionOr<Value> {
        vm.heap().collect_garbage();
        Ok(Value::UNDEFINED)
    }
}

/// The global object of the realm js runs scripts in.
#[repr(C)]
#[derive(Trace)]
pub struct ScriptObject {
    base: GlobalObject,
}

define_object_class!(ScriptObject, extends: [GlobalObject, Object], methods: {
    initialize: ScriptObject::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl ScriptObject {
    pub fn allocate(vm: &Vm, realm: Gc<Realm>) -> Gc<ScriptObject> {
        allocate_object(
            vm,
            ScriptObject {
                base: GlobalObject::new(vm, Self::CLASS, realm),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let base_initialize = GlobalObject::CLASS
            .object_methods
            .expect("GlobalObject is an object class")
            .initialize;
        base_initialize(object, vm, realm);

        object.define_direct_property(
            vm,
            &key("global"),
            Value::from_object(object.as_gc()),
            PropertyAttributes::new(Attribute::ENUMERABLE),
        );
        let attr = PropertyAttributes::new(Attribute::CONFIGURABLE | Attribute::WRITABLE | Attribute::ENUMERABLE);
        let define = |name: &str, function, length| {
            object.define_native_function(vm, realm, &key(name), function, length, attr, None);
        };
        define("loadINI", raw_native!(ScriptObject::load_ini), 1);
        define("loadJSON", raw_native!(ScriptObject::load_json), 1);
        define("print", raw_native!(ScriptObject::print), 1);
        define("gc", raw_native!(ScriptObject::gc), 0);
    }

    fn load_ini(vm: &Vm) -> ThrowCompletionOr<Value> {
        load_ini_impl(vm)
    }

    fn load_json(vm: &Vm) -> ThrowCompletionOr<Value> {
        load_json_impl(vm)
    }

    fn print(vm: &Vm) -> ThrowCompletionOr<Value> {
        print_all_arguments_or_throw(vm)
    }

    #[allow(clippy::unnecessary_wraps, reason = "native functions return a completion")]
    fn gc(vm: &Vm) -> ThrowCompletionOr<Value> {
        vm.heap().collect_garbage();
        Ok(Value::UNDEFINED)
    }
}

fn key(name: &str) -> PropertyKey {
    PropertyKey::from(Utf16FlyString::from_utf8(name))
}

fn debugger_pause_reason(reason: PauseReason) -> &'static str {
    match reason {
        PauseReason::Entry => "entry",
        PauseReason::Breakpoint => "breakpoint",
        PauseReason::DebuggerStatement => "debugger statement",
        PauseReason::Exception => "exception",
        PauseReason::Step => "step",
    }
}

struct BreakpointLocation {
    filename: Utf16String,
    line: u32,
    column: Option<u32>,
}

/// StringView::trim_whitespace()
fn trim_whitespace(mut string: &[u8]) -> &[u8] {
    const WHITESPACE: &[u8] = b" \n\t\x0b\x0c\r";
    while let [first, rest @ ..] = string
        && WHITESPACE.contains(first)
    {
        string = rest;
    }
    while let [rest @ .., last] = string
        && WHITESPACE.contains(last)
    {
        string = rest;
    }
    string
}

/// StringView::to_number<u32>(), which takes decimal digits without a sign, between whitespace.
fn to_number(string: &[u8]) -> Option<u32> {
    let digits = trim_whitespace(string);
    if digits.is_empty() || !digits.iter().all(u8::is_ascii_digit) {
        return None;
    }
    core::str::from_utf8(digits).ok()?.parse().ok()
}

fn breakpoint_filename(filename: &[u8], current_filename: Utf16View<'_>) -> Utf16String {
    if !current_filename.is_empty()
        && let Ok(filename) = core::str::from_utf8(filename)
    {
        let canonical_filename = lexical_path::canonicalized_path(filename);
        let canonical_current_filename = lexical_path::canonicalized_path(&current_filename.to_utf8());
        if canonical_filename == canonical_current_filename {
            return current_filename.to_utf16_string();
        }
    }
    Utf16String::from_utf8(&string_from_utf8_with_replacement_character(filename))
}

fn parse_breakpoint_location(input: &[u8], current_filename: Utf16View<'_>) -> Option<BreakpointLocation> {
    let input = trim_whitespace(input);
    if let Some(line) = to_number(input) {
        if line == 0 {
            return None;
        }
        return Some(BreakpointLocation {
            filename: current_filename.to_utf16_string(),
            line,
            column: None,
        });
    }

    let last_colon = input.iter().rposition(|&byte| byte == b':')?;

    let final_component = to_number(&input[last_colon + 1..])?;

    let prefix = &input[..last_colon];
    if let Some(line) = to_number(prefix) {
        if line == 0 {
            return None;
        }
        return Some(BreakpointLocation {
            filename: current_filename.to_utf16_string(),
            line,
            column: Some(final_component),
        });
    }

    if let Some(preceding_colon) = prefix.iter().rposition(|&byte| byte == b':')
        && let Some(line) = to_number(&prefix[preceding_colon + 1..])
    {
        let filename = &prefix[..preceding_colon];
        if filename.is_empty() || line == 0 {
            return None;
        }
        return Some(BreakpointLocation {
            filename: breakpoint_filename(filename, current_filename),
            line,
            column: Some(final_component),
        });
    }

    if prefix.is_empty() || final_component == 0 {
        return None;
    }
    Some(BreakpointLocation {
        filename: breakpoint_filename(prefix, current_filename),
        line: final_component,
        column: None,
    })
}

fn print_breakpoint(debugger: &Debugger, breakpoint: &Breakpoint) {
    let state = if debugger.is_breakpoint_resolved(breakpoint.id) {
        "resolved"
    } else {
        "pending"
    };
    let mut line = format!("{}: ", breakpoint.id).into_bytes();
    line.extend(Utf16View::of_string(&breakpoint.filename).to_wtf8());
    match breakpoint.column {
        Some(column) => line.extend_from_slice(format!(":{}:{column} ({state})", breakpoint.line).as_bytes()),
        None => line.extend_from_slice(format!(":{} ({state})", breakpoint.line).as_bytes()),
    }
    standard_output::outln(&line);
}

fn print_debugger_help() {
    standard_output::outln(b"Debugger commands:");
    standard_output::outln(b"    .break <line>[:column]");
    standard_output::outln(b"    .break <file>:<line>[:column]");
    standard_output::outln(b"    .breakpoints");
    standard_output::outln(b"    .continue");
    standard_output::outln(b"    .delete <id>");
    standard_output::outln(b"    .help");
}

/// warnln() of bytes that need not be UTF-8.
fn warn_bytes_line(bytes: &[u8]) {
    let mut line = bytes.to_vec();
    line.push(b'\n');
    let _ = io::stderr().write_all(&line);
}

fn run_debugger_prompt(vm: &Vm, pause_info: &PauseInfo, line_editor: Option<&JSLineEditor>) {
    let debugger = vm.debugger().expect("execution pauses in the attached debugger");
    let reason = debugger_pause_reason(pause_info.reason);
    if let Some(range) = &pause_info.source_range {
        let filename = Utf16View::of_string(range.filename()).to_wtf8();
        let mut line = Vec::new();
        if range.start.line > 0 {
            line.extend_from_slice(b"Paused at ");
            line.extend(filename);
            line.extend_from_slice(format!(":{}:{} ({reason})", range.start.line, range.start.column).as_bytes());
        } else {
            line.extend_from_slice(b"Paused in ");
            line.extend(filename);
            line.extend_from_slice(format!(" ({reason})").as_bytes());
        }
        standard_output::outln(&line);
    } else {
        standard_output::outln(
            format!("Paused at bytecode offset {} ({reason})", pause_info.bytecode_offset).as_bytes(),
        );
    }

    loop {
        let raw_line = match line_editor {
            Some(line_editor) => line_editor.read_line(c"(debug) "),
            None => line_editor::read_line_without_line_editor(c"(debug) "),
        };
        let Some(raw_line) = raw_line else {
            debugger.continue_execution(ResumeMode::Continue);
            return;
        };
        let command = trim_whitespace(&raw_line);

        if command == b".continue" {
            debugger.continue_execution(ResumeMode::Continue);
            return;
        }

        if command == b".help" {
            print_debugger_help();
            continue;
        }

        if command == b".breakpoints" {
            let mut breakpoints = debugger.breakpoints();
            if breakpoints.is_empty() {
                standard_output::outln(b"No breakpoints.");
                continue;
            }
            breakpoints.sort_by_key(|breakpoint| breakpoint.id);
            for breakpoint in &breakpoints {
                print_breakpoint(&debugger, breakpoint);
            }
            continue;
        }

        if let Some(location) = command.strip_prefix(b".break ") {
            let current_filename = pause_info
                .source_range
                .as_ref()
                .map_or(Utf16View::EMPTY, |source_range| {
                    Utf16View::of_string(source_range.filename())
                });

            let location = parse_breakpoint_location(location, current_filename);
            let Some(location) = location.filter(|location| !Utf16View::of_string(&location.filename).is_empty())
            else {
                eprintln!("Usage: .break <line>[:column] or .break <file>:<line>[:column]");
                continue;
            };

            let breakpoint_id =
                match debugger.add_breakpoint(Utf16View::of_string(&location.filename), location.line, location.column)
                {
                    Ok(breakpoint_id) => breakpoint_id,
                    Err(error) => {
                        eprintln!("Unable to set breakpoint: {error}");
                        continue;
                    }
                };

            let breakpoints = debugger.breakpoints();
            let breakpoint = breakpoints
                .iter()
                .find(|breakpoint| breakpoint.id == breakpoint_id)
                .expect("the debugger has the breakpoint it returned");
            print_breakpoint(&debugger, breakpoint);
            continue;
        }

        if let Some(breakpoint_id) = command.strip_prefix(b".delete ") {
            let Some(breakpoint_id) = to_number(trim_whitespace(breakpoint_id)) else {
                eprintln!("Usage: .delete <id>");
                continue;
            };
            if !debugger.remove_breakpoint(breakpoint_id) {
                eprintln!("No breakpoint with id {breakpoint_id}.");
                continue;
            }
            standard_output::outln(format!("Deleted breakpoint {breakpoint_id}.").as_bytes());
            continue;
        }

        let mut message = b"Unknown debugger command '".to_vec();
        message.extend_from_slice(command);
        message.extend_from_slice(b"'. Enter .help for a list of commands.");
        warn_bytes_line(&message);
    }
}

fn print_inline(vm: &Vm, value: Value, stream: &mut dyn Write) -> io::Result<()> {
    let mut print_context = PrintContext {
        vm,
        stream,
        strip_ansi: STRIP_ANSI.load(Ordering::Relaxed),
        raw_strings: RAW_STRINGS.load(Ordering::Relaxed),
    };
    print(value, &mut print_context)
}

#[derive(Clone, Copy)]
enum PrintTarget {
    StandardError,
    StandardOutput,
}

/// The stream a print goes to: past stdio's buffer, which is flushed first.
fn flushed_print_stream(target: PrintTarget) -> UnbufferedWriter {
    match target {
        PrintTarget::StandardOutput => {
            standard_output::flush();
            UnbufferedWriter::STANDARD_OUTPUT
        }
        // NB: The standard error is unbuffered, so there is nothing to flush.
        PrintTarget::StandardError => UnbufferedWriter::STANDARD_ERROR,
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum PrintEnd {
    Newline,
    None,
}

fn print_value(vm: &Vm, value: Value, target: PrintTarget, end: PrintEnd) -> io::Result<()> {
    let mut stream = flushed_print_stream(target);

    print_inline(vm, value, &mut stream)?;

    if end == PrintEnd::Newline {
        stream.write_all(b"\n")?;
    }

    Ok(())
}

fn warn_about_promise(vm: &Vm, warning: &str, promise: Gc<Promise>) {
    eprint!("{warning}");
    eprint!(" (result: ");
    let _ = print_value(vm, promise.result(), PrintTarget::StandardError, PrintEnd::None);
    eprintln!(")");
}

fn print_all_arguments(vm: &Vm, target: PrintTarget, end: PrintEnd) -> io::Result<()> {
    let mut stream = flushed_print_stream(target);

    for i in 0..vm.argument_count() {
        print_inline(vm, vm.argument(i), &mut stream)?;

        if i < vm.argument_count() - 1 {
            stream.write_all(b" ")?;
        }
    }

    if end == PrintEnd::Newline {
        stream.write_all(b"\n")?;
    }

    Ok(())
}

/// The print() of the global objects of js.
fn print_all_arguments_or_throw(vm: &Vm) -> ThrowCompletionOr<Value> {
    if let Err(error) = print_all_arguments(vm, PrintTarget::StandardOutput, PrintEnd::Newline) {
        return vm.throw_completion_with_message(
            ErrorKind::InternalError,
            format!("Failed to print value(s): {}", write_error_string(&error)),
        );
    }

    Ok(Value::UNDEFINED)
}

fn prompt_for_level(level: i32) -> CString {
    let mut prompt = String::from("> ");

    for _ in 0..level {
        prompt.push_str("    ");
    }

    CString::new(prompt).expect("the prompt has no NULs")
}

/// Writes the statements that the REPL ran before the current one to `path`, each followed by a newline.
fn write_to_file(path: &[u8]) -> io::Result<()> {
    let mut options = std::fs::OpenOptions::new();
    options.write(true).create(true).truncate(true);
    #[cfg(unix)]
    options.mode(0o666);
    #[cfg(unix)]
    let path = std::ffi::OsStr::from_bytes(path);
    #[cfg(windows)]
    let path = std::str::from_utf8(path).map_err(|error| io::Error::new(io::ErrorKind::InvalidInput, error))?;
    let mut file = options.open(path)?;
    REPL_STATEMENTS.with_borrow(|statements| {
        for (i, line) in statements.iter().enumerate() {
            if !line.is_empty() && i != statements.len() - 1 {
                file.write_all(line)?;
            }
            if i != statements.len() - 1 {
                file.write_all(b"\n")?;
            }
        }
        Ok(())
    })
}

/// error->stack_string(JS::CompactTraceback::Yes) of a thrown Error, which the C++ js prints after the error.
fn stack_string_of_thrown_error(thrown_value: Value) -> Option<Utf16String> {
    if !thrown_value.is_object() {
        return None;
    }
    let error = thrown_value.as_object().downcast::<Error>()?;
    Some(error.stack_string(CompactTraceback::Yes))
}

fn handle_exception(vm: &Vm, thrown_value: Value) -> io::Result<()> {
    eprintln!("Uncaught exception: ");
    print_value(vm, thrown_value, PrintTarget::StandardError, PrintEnd::Newline)?;

    if let Some(stack_string) = stack_string_of_thrown_error(thrown_value) {
        let mut line = Utf16View::of_string(&stack_string).to_wtf8();
        line.push(b'\n');
        let _ = io::stderr().write_all(&line);
    }
    Ok(())
}

/// Returns whether the source ran without throwing, or the error that LibMain reports when printing fails.
fn parse_and_run(
    vm: &'static Vm,
    realm: Gc<Realm>,
    options: &Options,
    source: &[u8],
    source_name: &str,
    parse_only: bool,
) -> Result<bool, String> {
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
            standard_output::outln(&Utf16View::Utf16(&hint).to_wtf8());
        }

        let error_string = error.to_string();
        standard_output::outln(error_string.as_bytes());
        result = vm.throw_completion_with_message(ErrorKind::SyntaxError, error_string);
    } else {
        // NB: The C++ js dumps the AST in color unless -i is given, which the frontend only offers to standard output
        //     directly, so this dumps it without color. Like the frontend, it prints through the standard output of
        //     std, which writes each line out past the buffer that js prints its other output into.
        if options.dump_ast {
            let mut stdout = io::stdout().lock();
            let _ = stdout.write_all(parsed.ast_dump().as_bytes());
            let _ = stdout.write_all(b"\n");
            let _ = stdout.flush();
        }
        let source_code = SourceCode::create(
            Utf16String::from_utf8(source_name),
            Utf16String::from_utf16(&utf16_source),
        );
        if !options.as_module {
            let script = Script::create_from_parsed_with_filename(vm, parsed, source_code, realm, source_name);
            if !parse_only {
                result = vm.run_script(script, None);
            }
        } else {
            let module = SourceTextModule::create_from_parsed(vm, parsed, source_code, realm, source_name);
            if !parse_only {
                result = vm.run_module(module);
            }
        }
    }

    if let Ok(value) = result {
        LAST_VALUE.set(Some(Root::new(vm, value)));
    }

    match result {
        Err(throw) => {
            handle_exception(vm, throw.value()).map_err(|error| write_error_string(&error))?;
            Ok(false)
        }
        Ok(value) => {
            if options.print_last_result {
                print_value(vm, value, PrintTarget::StandardOutput, PrintEnd::Newline)
                    .map_err(|error| write_error_string(&error))?;
            }
            Ok(true)
        }
    }
}

/// How AK formats the Error of a failed write.
fn write_error_string(error: &io::Error) -> String {
    system_error_string("write", error)
}

/// How AK formats the Error of a failed system call: "<syscall>: <strerror> (errno=<code>)".
pub(crate) fn system_error_string(syscall: &str, error: &io::Error) -> String {
    match error.raw_os_error() {
        Some(code) => {
            let message = io::Error::from_raw_os_error(code).to_string();
            let message = message
                .strip_suffix(&format!(" (os error {code})"))
                .unwrap_or(&message)
                .to_string();
            format!("{syscall}: {message} (errno={code})")
        }
        None => error.to_string(),
    }
}

/// Opens a file and reads it to its end, failing with what Core::File reports for the call that failed: the open, or
/// a read.
fn open_and_read_file(path: &str) -> Result<Vec<u8>, ReadFileError> {
    let mut file =
        std::fs::File::open(path).map_err(|error| ReadFileError::Open(system_error_string("open", &error)))?;
    let mut contents = Vec::new();
    file.read_to_end(&mut contents)
        .map_err(|error| ReadFileError::Read(system_error_string("read", &error)))?;
    Ok(contents)
}

enum ReadFileError {
    Open(String),
    Read(String),
}

fn load_ini_impl(vm: &Vm) -> ThrowCompletionOr<Value> {
    let realm = vm.current_realm().expect("loadINI runs in a realm");

    let filename = vm.argument(0).to_utf16_string(vm)?;
    let contents = match open_and_read_file(&Utf16View::of_string(&filename).to_utf8()) {
        Ok(contents) => contents,
        Err(ReadFileError::Open(error)) => {
            return vm.throw_completion_with_utf16_message(
                ErrorKind::Error,
                utf16_formatted("Failed to open '{}': {}", &[&filename, &error]),
            );
        }
        Err(ReadFileError::Read(error)) => {
            return vm.throw_completion_with_utf16_message(
                ErrorKind::Error,
                utf16_formatted("Failed to read '{}': {}", &[&filename, &error]),
            );
        }
    };

    let config_file = ConfigFile::parse(&contents);
    let object = Object::create(vm, realm, Some(realm.object_prototype()));
    let attributes = PropertyAttributes::new(Attribute::ENUMERABLE | Attribute::CONFIGURABLE | Attribute::WRITABLE);
    let utf16_from_utf8 = |bytes: &[u8]| {
        Utf16String::from_utf8(core::str::from_utf8(bytes).expect("Utf16String::from_utf8() takes valid UTF-8"))
    };
    for group in config_file.groups() {
        let group_object = Object::create(vm, realm, Some(realm.object_prototype()));
        for (entry_key, entry) in config_file.entries(group) {
            group_object.define_direct_property(
                vm,
                &PropertyKey::from_utf16_string(&utf16_from_utf8(entry_key)),
                Value::from_string(PrimitiveString::create(vm, utf16_from_utf8(entry))),
                attributes,
            );
        }
        object.define_direct_property(
            vm,
            &PropertyKey::from_utf16_string(&utf16_from_utf8(group)),
            Value::from_object(group_object),
            attributes,
        );
    }
    Ok(Value::from_object(object))
}

/// The keys and values of a group of an INI file.
type ConfigFileEntries = Vec<(Vec<u8>, Vec<u8>)>;

/// What Core::ConfigFile reads from an INI file: the groups and their entries, which it keeps in AK HashMaps that
/// are walked in the order of their buckets.
struct ConfigFile {
    groups: Vec<(Vec<u8>, ConfigFileEntries)>,
}

/// The values of a HashMap keyed by ByteStrings, in the order the C++ map visits them.
fn in_hash_map_order<T>(entries: &[(Vec<u8>, T)]) -> Vec<&(Vec<u8>, T)> {
    let mut table = HashTable::default();
    for (key, _) in entries {
        table.set(key.clone());
    }
    table
        .iter()
        .map(|key| {
            entries
                .iter()
                .find(|(entry_key, _)| entry_key == key)
                .expect("every key has an entry")
        })
        .collect()
}

impl ConfigFile {
    /// ConfigFile::parse(), which reads the lines of the file as InputBufferedFile::read_line() splits them.
    fn parse(contents: &[u8]) -> Self {
        let mut groups: Vec<(Vec<u8>, ConfigFileEntries)> = Vec::new();
        let mut current_group: Option<usize> = None;
        let ensure_group = |groups: &mut Vec<(Vec<u8>, ConfigFileEntries)>, name: Vec<u8>| {
            if let Some(index) = groups.iter().position(|(group_name, _)| *group_name == name) {
                return index;
            }
            groups.push((name, Vec::new()));
            groups.len() - 1
        };

        let mut lines: Vec<&[u8]> = contents.split(|&byte| byte == b'\n').collect();
        if contents.ends_with(b"\n") || contents.is_empty() {
            lines.pop();
        }
        for line in lines {
            let mut i = 0;

            while i < line.len() && (line[i] == b' ' || line[i] == b'\t' || line[i] == b'\n') {
                i += 1;
            }

            if i >= line.len() {
                continue;
            }

            match line[i] {
                // Comment, skip entire line.
                b'#' | b';' => {}
                // Start of new group.
                b'[' => {
                    let mut builder = Vec::new();
                    i += 1; // Skip the '['
                    while i < line.len() && line[i] != b']' {
                        builder.push(line[i]);
                        i += 1;
                    }
                    current_group = Some(ensure_group(&mut groups, builder));
                }
                // Start of key
                _ => {
                    let mut key_builder = Vec::new();
                    let mut value_builder = Vec::new();
                    while i < line.len() && line[i] != b'=' {
                        key_builder.push(line[i]);
                        i += 1;
                    }
                    i += 1; // Skip the '='
                    while i < line.len() && line[i] != b'\n' {
                        value_builder.push(line[i]);
                        i += 1;
                    }
                    // We're not in a group yet, create one with the name ""...
                    let group = *current_group.get_or_insert_with(|| ensure_group(&mut groups, Vec::new()));
                    while value_builder
                        .last()
                        .is_some_and(|byte| b" \n\t\x0b\x0c\r".contains(byte))
                    {
                        value_builder.pop();
                    }
                    let entries = &mut groups[group].1;
                    match entries.iter_mut().find(|(entry_key, _)| *entry_key == key_builder) {
                        Some(entry) => entry.1 = value_builder,
                        None => entries.push((key_builder, value_builder)),
                    }
                }
            }
        }
        Self { groups }
    }

    fn groups(&self) -> Vec<&[u8]> {
        in_hash_map_order(&self.groups)
            .into_iter()
            .map(|(name, _)| name.as_slice())
            .collect()
    }

    fn entries(&self, group: &[u8]) -> Vec<(&[u8], &[u8])> {
        let (_, entries) = self
            .groups
            .iter()
            .find(|(name, _)| name == group)
            .expect("the group exists");
        in_hash_map_order(entries)
            .into_iter()
            .map(|(entry_key, entry)| (entry_key.as_slice(), entry.as_slice()))
            .collect()
    }
}

fn load_json_impl(vm: &Vm) -> ThrowCompletionOr<Value> {
    let filename = vm.argument(0).to_utf16_string(vm)?;
    let file_contents = match open_and_read_file(&Utf16View::of_string(&filename).to_utf8()) {
        Ok(contents) => contents,
        Err(ReadFileError::Open(error)) => {
            return vm.throw_completion_with_utf16_message(
                ErrorKind::Error,
                utf16_formatted("Failed to open '{}': {}", &[&filename, &error]),
            );
        }
        Err(ReadFileError::Read(error)) => {
            return vm.throw_completion_with_utf16_message(
                ErrorKind::Error,
                utf16_formatted("Failed to read '{}': {}", &[&filename, &error]),
            );
        }
    };

    let Ok(json_text) = core::str::from_utf8(&file_contents) else {
        return vm.throw_completion(ErrorKind::SyntaxError, ErrorType::JsonMalformed, &[]);
    };
    let json_text = Utf16String::from_utf8(json_text);

    JSONObject::parse_json(vm, Utf16View::of_string(&json_text), None)
}

/// ReplConsoleClient of the C++ js: prints what the console logs to the standard output.
#[repr(C)]
#[derive(Trace)]
pub struct ReplConsoleClient {
    base: ConsoleClient,
    group_stack_depth: Cell<i32>,
}

define_cell!(ReplConsoleClient, Other, extends: [ConsoleClient]);

static REPL_CONSOLE_CLIENT_METHODS: ConsoleClientMethods = ConsoleClientMethods {
    printer: ReplConsoleClient::printer,
    add_css_style_to_current_message: |_, _| {},
    report_exception: |_, _, _, _, _| {},
    clear: ReplConsoleClient::clear,
    end_group: ReplConsoleClient::end_group,
};

impl ReplConsoleClient {
    pub fn create(vm: &Vm, console: Gc<Console>) -> Gc<ReplConsoleClient> {
        vm.heap().allocate(ReplConsoleClient {
            base: ConsoleClient::new(Self::CLASS, &REPL_CONSOLE_CLIENT_METHODS, console),
            group_stack_depth: Cell::new(0),
        })
    }

    fn of(client: &ConsoleClient) -> &ReplConsoleClient {
        // SAFETY: Only ReplConsoleClient has these methods.
        unsafe { &*core::ptr::from_ref(client).cast::<ReplConsoleClient>() }
    }

    fn clear(client: &ConsoleClient) {
        let this = Self::of(client);
        standard_output::out(b"\x1b[3J\x1b[H\x1b[2J");
        this.group_stack_depth.set(0);
        standard_output::flush();
    }

    fn end_group(client: &ConsoleClient) {
        let this = Self::of(client);
        if this.group_stack_depth.get() > 0 {
            this.group_stack_depth.set(this.group_stack_depth.get() - 1);
        }
    }

    // 2.3. Printer(logLevel, args[, options]), https://console.spec.whatwg.org/#printer
    fn printer<'vm>(
        client: &ConsoleClient,
        vm: &'vm Vm,
        log_level: LogLevel,
        arguments: PrinterArguments<'vm>,
    ) -> ThrowCompletionOr<Value> {
        let this = Self::of(client);
        let indent = " ".repeat(usize::try_from(this.group_stack_depth.get() * 2).unwrap_or(0));

        if log_level == LogLevel::Trace {
            let PrinterArguments::Trace(trace) = arguments else {
                unreachable!("trace() prints a trace");
            };
            let mut builder = String::new();
            if !Utf16View::of_string(&trace.label).is_empty() {
                builder.push_str(&format!(
                    "{indent}\x1b[36;1m{}\x1b[0m\n",
                    Utf16View::of_string(&trace.label).to_utf8()
                ));
            }

            for frame in &trace.stack {
                builder.push_str(&format!(
                    "{indent}-> {}\n",
                    Utf16View::of_string(&frame.function_name).to_utf8()
                ));
            }

            standard_output::outln(builder.as_bytes());
            return Ok(Value::UNDEFINED);
        }

        if log_level == LogLevel::Group || log_level == LogLevel::GroupCollapsed {
            let PrinterArguments::Group(group) = arguments else {
                unreachable!("group() and groupCollapsed() print a group");
            };
            let mut line = format!("{indent}\x1b[36;1m").into_bytes();
            line.extend(Utf16View::of_string(&group.label).to_wtf8());
            line.extend_from_slice(b"\x1b[0m");
            standard_output::outln(&line);
            this.group_stack_depth.set(this.group_stack_depth.get() + 1);
            return Ok(Value::UNDEFINED);
        }

        let PrinterArguments::Values(values) = arguments else {
            unreachable!("the other log levels print values");
        };
        let output = Utf16View::of_string(&client.generically_format_values(vm, &values)?).to_wtf8();

        let (prefix, suffix): (&[u8], &[u8]) = match log_level {
            LogLevel::Debug => (b"\x1b[36;1m", b"\x1b[0m"),
            LogLevel::Error | LogLevel::Assert => (b"\x1b[31;1m", b"\x1b[0m"),
            LogLevel::Info => (b"(i) ", b""),
            LogLevel::Warn | LogLevel::CountReset => (b"\x1b[33;1m", b"\x1b[0m"),
            _ => (b"", b""),
        };
        let mut line = indent.into_bytes();
        line.extend_from_slice(prefix);
        line.extend(output);
        line.extend_from_slice(suffix);
        standard_output::outln(&line);
        Ok(Value::UNDEFINED)
    }
}

impl Deref for ReplConsoleClient {
    type Target = ConsoleClient;

    fn deref(&self) -> &ConsoleClient {
        &self.base
    }
}

mod repl {
    use core::ops::ControlFlow;

    use super::*;
    use crate::bytecode::property_access::Strict;
    use crate::runtime::global_environment::GlobalEnvironment;
    use crate::runtime::shape::Shape;
    use crate::runtime::string_prototype::WHITESPACE_CHARACTER_CODE_UNITS;
    use crate::utf16::TrimMode;
    use libjs_rust::lexer::Lexer;
    use libjs_rust::token::TokenType;

    /// s_repl_realm and s_repl_global_environment of the C++ js, which complete_repl_line() completes names in.
    struct ReplRealm {
        vm: &'static Vm,
        realm: Root<'static, Gc<Realm>>,
        global_environment: Root<'static, Gc<GlobalEnvironment>>,
    }

    thread_local! {
        static REPL_REALM: RefCell<Option<ReplRealm>> = const { RefCell::new(None) };
    }

    #[derive(Clone, Copy, PartialEq, Eq)]
    enum CompletionMode {
        Initial,
        CompleteVariable,
        CompleteNullProperty,
        CompleteProperty,
    }

    /// The names that the end of the line can be completed to: the global variables that start with its last
    /// identifier, or the properties of a variable that start with what follows the period after it.
    fn complete_repl_line(line: &[u8]) -> Vec<Vec<u8>> {
        let Some((vm, realm, global_environment)) = REPL_REALM.with_borrow(|repl_realm| {
            repl_realm.as_ref().map(|repl_realm| {
                (
                    repl_realm.vm,
                    repl_realm.realm.value(),
                    repl_realm.global_environment.value(),
                )
            })
        }) else {
            return Vec::new();
        };
        // Like Utf16String::from_utf8(), this stops the process for a line that is not valid UTF-8.
        let code = utf16_from_wtf8(line).expect("the line is valid UTF-8");

        let mut mode = CompletionMode::Initial;

        let mut variable_name: &[u16] = &[];
        let mut property_name: &[u16] = &[];

        // We're only going to complete either
        //    - <N>
        //        where N is part of the name of a variable
        //    - <N>.<P>
        //        where N is the complete name of a variable and
        //        P is part of the name of one of its properties
        let mut lexer = Lexer::new(&code, 1, 0);
        let mut last_token_has_trivia = loop {
            let token = lexer.next();
            if token.token_type == TokenType::Eof {
                break token.trivia_len > 0;
            }

            let token_value = &code[token.value_start as usize..][..token.value_len as usize];

            match mode {
                CompletionMode::CompleteVariable => {
                    if token.token_type == TokenType::Period {
                        mode = CompletionMode::CompleteNullProperty;
                    } else {
                        mode = CompletionMode::Initial;
                    }
                }
                CompletionMode::CompleteNullProperty => {
                    if token.token_type.is_identifier_name() {
                        mode = CompletionMode::CompleteProperty;
                        property_name = token_value;
                    } else {
                        mode = CompletionMode::Initial;
                    }
                }
                CompletionMode::CompleteProperty | CompletionMode::Initial => {
                    if token.token_type == TokenType::Identifier {
                        mode = CompletionMode::CompleteVariable;
                        variable_name = token_value;
                    } else {
                        mode = CompletionMode::Initial;
                    }
                }
            }
        };

        if mode == CompletionMode::CompleteNullProperty {
            mode = CompletionMode::CompleteProperty;
            property_name = &[];
            last_token_has_trivia = false; // <name> <dot> [tab] is sensible to complete.
        }

        if mode == CompletionMode::Initial || last_token_has_trivia {
            return Vec::new(); // we do not know how to complete this
        }

        let mut results = Vec::new();

        match mode {
            CompletionMode::CompleteProperty => {
                let Ok(reference) = vm.resolve_binding(
                    &Utf16FlyString::from_utf16(variable_name),
                    Strict::No,
                    Some(global_environment.upcast()),
                ) else {
                    return Vec::new();
                };
                let Ok(variable) = reference.get_value(vm) else {
                    return Vec::new();
                };

                if variable.is_object() {
                    list_all_properties(
                        variable.as_object().shape(),
                        Utf16View::Utf16(property_name),
                        &mut results,
                    );
                    let variable_name = Utf16View::Utf16(variable_name).to_wtf8();
                    for result in &mut results {
                        let mut builder = variable_name.clone();
                        builder.push(b'.');
                        builder.append(result);
                        *result = builder;
                    }
                }
            }
            CompletionMode::CompleteVariable => {
                let variable = realm.global_object();
                list_all_properties(variable.shape(), Utf16View::Utf16(variable_name), &mut results);

                for name in global_environment.declarative_record().bindings() {
                    let name = Utf16View::of_fly_string(&name);
                    if name.starts_with(Utf16View::Utf16(variable_name)) {
                        results.push(name.to_wtf8());
                    }
                }
            }
            CompletionMode::Initial | CompletionMode::CompleteNullProperty => unreachable!(),
        }

        results
    }

    /// Adds the string-keyed properties of `shape` and of its prototypes that start with `property_pattern` to `results`,
    /// in insertion order and once each.
    fn list_all_properties(shape: Gc<Shape>, property_pattern: Utf16View<'_>, results: &mut Vec<Vec<u8>>) {
        shape.for_each_property_in_insertion_order(|property_key, _| {
            if !property_key.is_string() {
                return ControlFlow::Continue(());
            }

            let key = Utf16View::of_fly_string(property_key.as_string());

            if key.starts_with(property_pattern) {
                let completion = key.to_wtf8();
                if !results.contains(&completion) {
                    // hide duplicates
                    results.push(completion);
                }
            }
            ControlFlow::Continue(())
        });
        if let Some(prototype) = shape.prototype() {
            list_all_properties(prototype.shape(), property_pattern, results);
        }
    }

    #[derive(Clone, Copy, PartialEq, Eq)]
    enum LabelState {
        NotInLabelOrObjectKey,
        InLabelOrObjectKeyIdentifier,
        InLabelOrObjectKey,
    }

    /// Counts the brackets of `line` into the line level, and returns whether it ends in a label or an object literal
    /// key.
    fn update_line_level_for_line(line: &[u16]) -> bool {
        let mut label_state = LabelState::NotInLabelOrObjectKey;

        let mut lexer = Lexer::new(line, 1, 0);
        loop {
            let token = lexer.next();
            match token.token_type {
                TokenType::BracketOpen | TokenType::CurlyOpen | TokenType::ParenOpen => {
                    label_state = LabelState::NotInLabelOrObjectKey;
                    REPL_LINE_LEVEL.fetch_add(1, Ordering::Relaxed);
                }
                TokenType::BracketClose | TokenType::CurlyClose | TokenType::ParenClose => {
                    label_state = LabelState::NotInLabelOrObjectKey;
                    REPL_LINE_LEVEL.fetch_sub(1, Ordering::Relaxed);
                }
                TokenType::Identifier | TokenType::StringLiteral => {
                    if label_state == LabelState::NotInLabelOrObjectKey {
                        label_state = LabelState::InLabelOrObjectKeyIdentifier;
                    } else {
                        label_state = LabelState::NotInLabelOrObjectKey;
                    }
                }
                TokenType::Colon => {
                    if label_state == LabelState::InLabelOrObjectKeyIdentifier {
                        label_state = LabelState::InLabelOrObjectKey;
                    } else {
                        label_state = LabelState::NotInLabelOrObjectKey;
                    }
                }
                TokenType::Eof => break,
                _ => {}
            }
        }

        label_state == LabelState::InLabelOrObjectKey
    }

    /// Reads lines until the brackets they open are closed. Returns an empty piece at the end of the input.
    fn read_next_piece(line_editor: &JSLineEditor) -> Result<Vec<u8>, String> {
        let mut piece = Vec::new();

        loop {
            let prompt = prompt_for_level(REPL_LINE_LEVEL.load(Ordering::Relaxed));
            let raw_line = line_editor.read_line(&prompt);

            let mut line_level_delta_for_next_line = 0;

            let Some(line) = raw_line else {
                KEEP_RUNNING_REPL.store(false, Ordering::Relaxed);
                return Ok(Vec::new());
            };

            let Some(code) = utf16_from_wtf8(&line) else {
                return Err(STRING_FROM_UTF8_ERROR.to_string());
            };
            if !line.is_empty() {
                line_editor.add_line_to_history(
                    &CString::new(line.as_slice()).expect("a line from the line editor has no NULs"),
                );
            }

            piece.extend_from_slice(&line);
            piece.push(b'\n');

            if update_line_level_for_line(&code) {
                // If there's a label or object literal key at the end of this line,
                // prompt for more lines but do not change the line level.
                line_level_delta_for_next_line += 1;
            }

            if REPL_LINE_LEVEL.load(Ordering::Relaxed) + line_level_delta_for_next_line <= 0 {
                return Ok(piece);
            }
        }
    }

    fn repl(vm: &'static Vm, realm: Gc<Realm>, options: &Options, line_editor: &JSLineEditor) -> Result<(), String> {
        while KEEP_RUNNING_REPL.load(Ordering::Relaxed) {
            let piece = read_next_piece(line_editor)?;
            let code = utf16_from_wtf8(&piece).expect("the lines of a piece are valid UTF-8");
            if Utf16View::Utf16(&code)
                .trim(&WHITESPACE_CHARACTER_CODE_UNITS, TrimMode::Both)
                .is_empty()
            {
                continue;
            }

            REPL_STATEMENTS.with_borrow_mut(|statements| statements.push(piece.clone()));
            parse_and_run(vm, realm, options, &piece, "REPL", false)?;
        }
        Ok(())
    }

    pub(super) fn run_repl(
        vm: &'static Vm,
        mut options: Options,
        history_path: &CStr,
        line_editor: &JSLineEditor,
    ) -> Result<c_int, String> {
        options.print_last_result = true;

        let root_execution_context =
            initialize_realm_with_global_object(vm, &|realm| ReplObject::allocate(vm, realm).upcast());
        let realm = root_execution_context.realm();

        let console_object = realm.intrinsics().console_object(vm);
        let console_client = ReplConsoleClient::create(vm, console_object.console());
        console_object.console().set_client(console_client.upcast());
        vm.heap()
            .set_should_collect_on_every_allocation(options.gc_on_every_allocation);

        let global_environment = realm.global_environment();
        REPL_REALM.set(Some(ReplRealm {
            vm,
            realm: Root::new(vm, realm),
            global_environment: Root::new(vm, global_environment),
        }));

        line_editor.read_history_file(history_path);
        line_editor.complete_lines_with(complete_repl_line);

        repl(vm, realm, &options, line_editor)?;
        line_editor.write_history_file(history_path);
        Ok(EXIT_CODE.load(Ordering::Relaxed))
    }
}

/// LexicalPath::canonicalized_path() of a path that need not be UTF-8, which it only splits at slashes and dots.
fn canonicalized_path_of_bytes(path: &[u8]) -> Vec<u8> {
    let path_as_latin1: String = path.iter().copied().map(char::from).collect();
    lexical_path::canonicalized_path(&path_as_latin1)
        .chars()
        .map(|character| u8::try_from(character).expect("canonicalizing a path only removes parts of it"))
        .collect()
}

/// Core::StandardPaths::home_directory() without HOME: the home directory of the user in the user database.
#[cfg(unix)]
fn home_directory_of_user() -> Vec<u8> {
    // SAFETY: getpwuid() returns NULL or an entry whose home directory is NUL-terminated, which is copied before
    // endpwent() releases the entry.
    unsafe {
        let entry = libc::getpwuid(libc::getuid());
        let home_directory = if entry.is_null() {
            b"/".to_vec()
        } else {
            CStr::from_ptr((*entry).pw_dir).to_bytes().to_vec()
        };
        libc::endpwent();
        home_directory
    }
}

/// s_history_path of the C++ js: .js-history in Core::StandardPaths::home_directory(), which String::formatted()
/// fails to make if the home directory is not UTF-8.
fn history_path() -> Result<CString, String> {
    #[cfg(unix)]
    let home_directory = match std::env::var_os("HOME") {
        Some(home_directory) => home_directory.into_vec(),
        None => home_directory_of_user(),
    };
    // Core::StandardPaths::home_directory() on Windows.
    #[cfg(windows)]
    let home_directory = std::env::var_os("USERPROFILE")
        .map(std::ffi::OsString::into_encoded_bytes)
        .unwrap_or_default();
    let mut history_path = canonicalized_path_of_bytes(&home_directory);
    history_path.extend_from_slice(b"/.js-history");
    if utf16_from_wtf8(&history_path).is_none() {
        return Err(STRING_FROM_UTF8_ERROR.to_string());
    }
    Ok(CString::new(history_path).expect("a home directory has no NULs"))
}

/// What LibMain prints for the Error that ladybird_main() returns.
fn report_runtime_error(error: &str) {
    eprintln!("\x1b[31;1mRuntime error\x1b[0m: {error}");
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
    let mut file =
        std::fs::File::open(path).map_err(|error| report_runtime_error(&system_error_string("open", &error)))?;
    let mut file_contents = Vec::new();
    file.read_to_end(&mut file_contents)
        .map_err(|error| report_runtime_error(&system_error_string("read", &error)))?;
    Ok(file_contents)
}

fn ladybird_main(arguments: &[String], line_editor: Option<JSLineEditor>) -> c_int {
    let options = match parse_arguments(arguments, &mut StandardOutputWriter) {
        Ok(options) => options,
        Err(exit_code) => return exit_code,
    };
    STRIP_ANSI.store(options.strip_ansi, Ordering::Relaxed);
    RAW_STRINGS.store(options.raw_strings, Ordering::Relaxed);

    let history_path = match history_path() {
        Ok(history_path) => history_path,
        Err(error) => {
            report_runtime_error(&error);
            return 1;
        }
    };

    // NB: The -h and -s options change nothing, since the C++ js reads neither. Besides the warnings about rejected
    //     promises, --disable-debug-output silences debug output, which the runtime prints none of.
    set_dump_bytecode(options.dump_bytecode);

    // NB: Like the VM of the C++ js, which is NeverDestroyed, this one lives until the process exits, so that exiting does
    //     not first destroy every cell of the heap.
    let vm: &'static Vm = Box::leak(Vm::create());
    vm.set_dynamic_imports_allowed(true);

    if options.debug {
        vm.enable_debugging();
        let debugger = vm.debugger().expect("debugging was just enabled");
        debugger.set_pause_callback(move |vm, pause_info| {
            run_debugger_prompt(vm, pause_info, line_editor.as_ref());
        });
        debugger.request_pause_on_next_bytecode_execution();
    }

    if !options.disable_debug_printing {
        // NOTE: These will print out both warnings when using something like Promise.reject().catch(...) -
        // which is, as far as I can tell, correct - a promise is created, rejected without handler, and a
        // handler then attached to it. The Node.js REPL doesn't warn in this case, so it's something we
        // might want to revisit at a later point and disable warnings for promises created this way.
        vm.set_on_promise_unhandled_rejection(Some(|vm, promise| {
            warn_about_promise(vm, "WARNING: A promise was rejected without any handlers", promise);
        }));
        vm.set_on_promise_rejection_handled(Some(|vm, promise| {
            warn_about_promise(
                vm,
                "WARNING: A handler was added to an already rejected promise",
                promise,
            );
        }));
    }

    if options.evaluate_script.is_empty() && options.script_paths.is_empty() {
        let Some(line_editor) = &line_editor else {
            eprintln!("REPL functionality is not supported on this platform");
            unreachable!("the REPL is only supported with a line editor");
        };
        return match repl::run_repl(vm, options, &history_path, line_editor) {
            Ok(exit_code) => exit_code,
            Err(error) => {
                report_runtime_error(&error);
                1
            }
        };
    }

    let root_execution_context = if options.use_test262_global {
        initialize_realm_with_global_object(vm, &|realm| Test262GlobalObject::allocate(vm, realm).upcast())
    } else {
        initialize_realm_with_global_object(vm, &|realm| ScriptObject::allocate(vm, realm).upcast())
    };

    let realm = root_execution_context.realm();
    let console_object = realm.intrinsics().console_object(vm);
    let console_client = ReplConsoleClient::create(vm, console_object.console());
    console_object.console().set_client(console_client.upcast());
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

    match parse_and_run(vm, realm, &options, &builder, source_name, options.parse_only) {
        Ok(true) => EXIT_CODE.load(Ordering::Relaxed),
        Ok(false) => 1,
        Err(error) => {
            report_runtime_error(&error);
            1
        }
    }
}

/// The entry point of js, called from its C++ main on the main thread. The REPL and the debugger prompt read
/// their input with `line_editor`, which is copied. Without one, the REPL is not supported, and the debugger prompt
/// reads the standard input as the C++ js does without libedit.
///
/// # Safety
///
/// `argv` must hold `argc` NUL-terminated strings, and `line_editor` must be NULL or point to a JSLineEditor whose
/// functions stay callable until this returns.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn libjs_runtime_rust_js_main(
    argc: c_int,
    argv: *const *const c_char,
    line_editor: *const JSLineEditor,
) -> c_int {
    let arguments: Vec<String> = (0..usize::try_from(argc).unwrap_or(0))
        // SAFETY: The caller passes argc valid strings.
        .map(|index| {
            unsafe { CStr::from_ptr(*argv.add(index)) }
                .to_string_lossy()
                .into_owned()
        })
        .collect();
    // SAFETY: The caller passes NULL or a valid line editor.
    let line_editor = unsafe { line_editor.as_ref() }.copied();
    let result = ladybird_main(&arguments, line_editor);
    // Like exit(), which flushes C stdio.
    standard_output::flush();
    result
}
