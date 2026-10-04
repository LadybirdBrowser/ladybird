/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! test-js-runtime-rust, the runner of the LibJS runtime tests in Tests/LibJS, as LibTest's JavaScriptTestRunner and
//! Tests/LibJS/test-js.cpp are for the C++ runtime. This is its main, JavaScriptTestRunnerMain.cpp.

mod javascript_test_runner;
mod json;
mod live_display;
mod test_js_functions;
mod test_runner;

use core::ffi::{c_char, c_int};
use core::sync::atomic::{AtomicBool, Ordering};
use std::ffi::CStr;
use std::io::Write;

use ak::Utf16String;

use self::test_runner::{TestRunner, TestRunnerOptions, cleanup, write_counts_for_siginfo};
use crate::interpreter::run::set_dump_bytecode;
use crate::interpreter::vm::Vm;
use crate::utilities::js::system_error_string;

const TEST_ROOT_FRAGMENT: &str = "Tests/LibJS/Runtime";

#[derive(Default)]
struct Options {
    show_help: bool,
    show_version: bool,
    print_times: bool,
    print_progress: bool,
    print_json: bool,
    per_file: bool,
    print_each_test: bool,
    collect_on_every_allocation: bool,
    dump_bytecode: bool,
    test_globs: Vec<String>,
    test262_parser_tests: bool,
    specified_test_root: String,
    common_path: String,
}

#[derive(Clone, Copy)]
enum OptionTarget {
    ShowHelp,
    ShowVersion,
    ShowTime,
    ShowProgress,
    Json,
    PerFile,
    Verbose,
    CollectOften,
    DumpBytecode,
    Filter,
    Test262ParserTests,
}

/// An option as Core::ArgsParser describes it. Options with a value take one, and those with a value name show it.
struct OptionDescription {
    help_string: &'static str,
    long_name: &'static str,
    short_name: Option<u8>,
    takes_value: bool,
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
        takes_value: false,
        value_name: None,
        shown_in_synopsis: true,
        target,
    }
}

/// The options in the order JavaScriptTestRunnerMain.cpp registers them, after the two every Core::ArgsParser has,
/// with the one test-js.cpp adds last.
const OPTIONS: &[OptionDescription] = &[
    OptionDescription {
        shown_in_synopsis: false,
        ..option("Display help message and exit", "help", None, OptionTarget::ShowHelp)
    },
    OptionDescription {
        shown_in_synopsis: false,
        ..option("Print version", "version", None, OptionTarget::ShowVersion)
    },
    option(
        "Show duration of each test",
        "show-time",
        Some(b't'),
        OptionTarget::ShowTime,
    ),
    OptionDescription {
        takes_value: true,
        ..option(
            "Show progress with OSC 9 (true, false)",
            "show-progress",
            Some(b'p'),
            OptionTarget::ShowProgress,
        )
    },
    option("Show results as JSON", "json", Some(b'j'), OptionTarget::Json),
    option(
        "Show detailed per-file results as JSON (implies -j)",
        "per-file",
        None,
        OptionTarget::PerFile,
    ),
    option(
        "Print each test file before running it",
        "verbose",
        Some(b'v'),
        OptionTarget::Verbose,
    ),
    option(
        "Collect garbage after every allocation",
        "collect-often",
        Some(b'g'),
        OptionTarget::CollectOften,
    ),
    option(
        "Dump the bytecode",
        "dump-bytecode",
        Some(b'd'),
        OptionTarget::DumpBytecode,
    ),
    OptionDescription {
        takes_value: true,
        value_name: Some("glob"),
        ..option(
            "Only run tests matching the given glob",
            "filter",
            Some(b'f'),
            OptionTarget::Filter,
        )
    },
    option(
        "Run test262 parser tests",
        "test262-parser-tests",
        None,
        OptionTarget::Test262ParserTests,
    ),
];

/// The positional arguments, each of which takes at most one value.
const POSITIONAL_ARGUMENTS: &[(&str, &str)] = &[
    ("path", "Tests root directory"),
    ("common-path", "Path to tests-common.js"),
];

impl OptionDescription {
    fn name_for_display(&self) -> String {
        format!("--{}", self.long_name)
    }

    /// Option::accept_value(), which fails for a value the option does not take.
    fn accept_value(&self, options: &mut Options, value: &str) -> bool {
        match self.target {
            OptionTarget::ShowHelp => options.show_help = true,
            OptionTarget::ShowVersion => options.show_version = true,
            OptionTarget::ShowTime => options.print_times = true,
            OptionTarget::ShowProgress => match value {
                "true" => options.print_progress = true,
                "false" => options.print_progress = false,
                _ => return false,
            },
            OptionTarget::Json => options.print_json = true,
            OptionTarget::PerFile => options.per_file = true,
            OptionTarget::Verbose => options.print_each_test = true,
            OptionTarget::CollectOften => options.collect_on_every_allocation = true,
            OptionTarget::DumpBytecode => options.dump_bytecode = true,
            OptionTarget::Filter => options.test_globs.push(value.to_string()),
            OptionTarget::Test262ParserTests => options.test262_parser_tests = true,
        }
        true
    }
}

/// ArgsParser::print_usage_terminal().
fn print_usage(file: &mut dyn Write, argv0: &str) -> std::io::Result<()> {
    write!(file, "Usage:\n\t\x1b[1m{argv0}\x1b[0m")?;
    for option in OPTIONS.iter().filter(|option| option.shown_in_synopsis) {
        if option.takes_value {
            // NB: Core::ArgsParser prints a missing value name as "(null)".
            let value_name = option.value_name.unwrap_or("(null)");
            write!(file, " [{} {value_name}]", option.name_for_display())?;
        } else {
            write!(file, " [{}]", option.name_for_display())?;
        }
    }
    for (name, _) in POSITIONAL_ARGUMENTS {
        write!(file, " [{name}]")?;
    }
    writeln!(file)?;

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
    for (name, help_string) in POSITIONAL_ARGUMENTS {
        writeln!(file, "\t\x1b[1m{name}\x1b[0m\t{help_string}")?;
    }
    Ok(())
}

/// Core::ArgsParser::parse() on the runner's options, with AK::OptionParser's getopt rules: options and arguments may
/// come in any order, short options may be grouped, values follow their option or are attached to it, and `--` ends
/// the options. Returns the exit code instead when it fails, or when it shows the help or the version.
fn parse_arguments(arguments: &[String], output: &mut dyn Write) -> Result<Options, c_int> {
    let argv0 = arguments.first().map_or("<exe>", String::as_str);
    let fail = || -> c_int {
        let _ = print_usage(&mut std::io::stderr(), argv0);
        1
    };
    let invalid_value = |option: &OptionDescription| -> c_int {
        eprintln!(
            "\x1b[31mInvalid value for option \x1b[1m{}\x1b[22m\x1b[0m",
            option.name_for_display()
        );
        fail()
    };

    let mut options = Options::default();
    let mut positional_values = Vec::new();
    let mut index = 1;
    while index < arguments.len() {
        let argument = arguments[index].as_str();
        index += 1;
        if argument == "--" {
            positional_values.extend(arguments[index..].iter().cloned());
            break;
        }
        // Anything that doesn't start with a "-" is not an option.
        // As a special case, a single "-" is not an option either.
        if !argument.starts_with('-') || argument == "-" {
            positional_values.push(argument.to_string());
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
            let value = match (option.takes_value, value) {
                (false, Some(_)) => {
                    eprintln!(
                        "Option \x1b[1m--{}\x1b[22m doesn't accept an argument",
                        option.long_name
                    );
                    return Err(fail());
                }
                (false, None) => "",
                (true, Some(value)) => value,
                (true, None) => {
                    let Some(value) = arguments.get(index) else {
                        eprintln!("Missing value for option \x1b[1m--{}\x1b[22m", option.long_name);
                        return Err(fail());
                    };
                    index += 1;
                    value.as_str()
                }
            };
            if !option.accept_value(&mut options, value) {
                return Err(invalid_value(option));
            }
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
            if !option.takes_value {
                if !option.accept_value(&mut options, "") {
                    return Err(invalid_value(option));
                }
                continue;
            }
            // The rest of the argument is the value, the "-ovalue" syntax, or else the next argument is.
            let attached_value = &argument[position + 2..];
            let value = if !attached_value.is_empty() {
                attached_value
            } else if let Some(value) = arguments.get(index) {
                index += 1;
                value.as_str()
            } else {
                eprintln!("Missing value for option \x1b[1m-{}\x1b[22m", char::from(short_name));
                return Err(fail());
            };
            if !option.accept_value(&mut options, value) {
                return Err(invalid_value(option));
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

    if positional_values.len() > POSITIONAL_ARGUMENTS.len() {
        return Err(fail());
    }
    let mut positional_values = positional_values.into_iter();
    options.specified_test_root = positional_values.next().unwrap_or_default();
    options.common_path = positional_values.next().unwrap_or_default();
    Ok(options)
}

/// AK's set_debug_enabled(), which DISABLE_DBG_OUTPUT turns off.
static DEBUG_ENABLED: AtomicBool = AtomicBool::new(true);

/// g_program_name, the name the SIGABRT handler reports, as a NUL-terminated string.
static PROGRAM_NAME: std::sync::OnceLock<Vec<u8>> = std::sync::OnceLock::new();

extern "C" fn handle_sigabrt(_: c_int) {
    if DEBUG_ENABLED.load(Ordering::Relaxed) {
        let program_name = PROGRAM_NAME.get().map_or(&b"test-js"[..], Vec::as_slice);
        write_to_standard_error_from_signal_handler(program_name);
        write_to_standard_error_from_signal_handler(b": SIGABRT received, cleaning up.\n");
    }
    cleanup();
    if !set_abort_action(libc::SIG_DFL) {
        std::process::exit(1);
    }
    // SAFETY: abort() has no preconditions.
    unsafe { libc::abort() };
}

#[cfg(unix)]
fn write_to_standard_error_from_signal_handler(bytes: &[u8]) {
    // SAFETY: The bytes are valid for their length.
    unsafe { libc::write(libc::STDERR_FILENO, bytes.as_ptr().cast(), bytes.len()) };
}

#[cfg(windows)]
fn write_to_standard_error_from_signal_handler(bytes: &[u8]) {
    const STDERR_FILENO: c_int = 2;
    let length = libc::c_uint::try_from(bytes.len()).unwrap_or(libc::c_uint::MAX);
    // SAFETY: The bytes are valid for `length` bytes.
    unsafe { libc::write(STDERR_FILENO, bytes.as_ptr().cast(), length) };
}

#[cfg(unix)]
fn set_abort_action(handler: libc::sighandler_t) -> bool {
    // SAFETY: An all-zero sigaction is valid, and sigaction only reads the struct.
    unsafe {
        let mut action: libc::sigaction = core::mem::zeroed();
        action.sa_sigaction = handler;
        if libc::sigaction(libc::SIGABRT, &raw const action, core::ptr::null_mut()) < 0 {
            eprintln!("sigaction: {}", std::io::Error::last_os_error());
            return false;
        }
    }
    true
}

/// set_abort_action() on Windows, which has no sigaction().
#[cfg(windows)]
fn set_abort_action(handler: libc::sighandler_t) -> bool {
    // SAFETY: signal() only installs the handler.
    if unsafe { libc::signal(libc::SIGABRT, handler) } == libc::SIG_ERR as libc::sighandler_t {
        // NB: signal() only fails with EINVAL, which perror() reports like this.
        eprintln!("sigaction: Invalid argument");
        return false;
    }
    true
}

#[cfg(any(
    target_vendor = "apple",
    target_os = "freebsd",
    target_os = "netbsd",
    target_os = "openbsd",
    target_os = "dragonfly"
))]
fn install_siginfo_handler() {
    extern "C" fn handle_siginfo(_: c_int) {
        let mut buffer = [0u8; 4096];
        let length = write_counts_for_siginfo(&mut buffer);
        // SAFETY: The buffer holds `length` bytes.
        unsafe { libc::write(libc::STDOUT_FILENO, buffer.as_ptr().cast(), length) };
    }
    // SAFETY: The handler only formats into a buffer on its stack and writes it.
    unsafe { libc::signal(libc::SIGINFO, handle_siginfo as *const () as libc::sighandler_t) };
}

#[cfg(not(any(
    target_vendor = "apple",
    target_os = "freebsd",
    target_os = "netbsd",
    target_os = "openbsd",
    target_os = "dragonfly"
)))]
fn install_siginfo_handler() {
    let _ = write_counts_for_siginfo;
}

/// LexicalPath::join() of the source directory and a relative path.
fn join_path(directory: &str, path: &str) -> String {
    let joined = format!("{directory}/{path}");
    let mut parts: Vec<&str> = Vec::new();
    for part in joined.split('/') {
        match part {
            "" | "." => {}
            ".." if parts.last().is_some_and(|last| *last != "..") => {
                parts.pop();
            }
            _ => parts.push(part),
        }
    }
    let joined_parts = parts.join("/");
    if joined.starts_with('/') {
        format!("/{joined_parts}")
    } else {
        joined_parts
    }
}

/// FileSystem::real_path(), which is FileSystem::absolute_path() on Windows, with the error Core::System::realpath()
/// reports.
fn real_path(path: &str) -> Result<String, String> {
    #[cfg(not(windows))]
    let resolved = std::fs::canonicalize(path);
    #[cfg(windows)]
    let resolved = std::path::absolute(path);
    resolved
        .map(|path| path.to_string_lossy().into_owned())
        .map_err(|error| system_error_string("realpath", &error))
}

fn host_get_supported_import_attributes(_: &Vm) -> Vec<Utf16String> {
    vec![
        Utf16String::from_utf8("type"),
        Utf16String::from_utf8("key"),  // Used in modules/import-with-attributes.mjs test
        Utf16String::from_utf8("key1"), // Used in modules/basic-modules.js
        Utf16String::from_utf8("key2"), // Used in modules/import-with-attributes.mjs test
        Utf16String::from_utf8("default"), // Used in modules/import-with-attributes.mjs test
    ]
}

fn ladybird_main(arguments: &[String]) -> c_int {
    let invocation_cwd = std::env::current_dir()
        .map(|path| path.to_string_lossy().into_owned())
        .unwrap_or_default();
    let program_name = arguments
        .first()
        .map(|argv0| argv0.rsplit('/').next().unwrap_or(argv0).to_string())
        .unwrap_or_default();
    let _ = PROGRAM_NAME.set(program_name.clone().into_bytes());

    if !set_abort_action(handle_sigabrt as *const () as libc::sighandler_t) {
        return 1;
    }

    install_siginfo_handler();

    let mut options = match parse_arguments(arguments, &mut crate::standard_output::StandardOutputWriter) {
        Ok(options) => options,
        Err(exit_code) => return exit_code,
    };

    if options.per_file {
        options.print_json = true;
    }

    let mut test_globs: Vec<String> = options.test_globs.iter().map(|glob| format!("*{glob}*")).collect();
    if test_globs.is_empty() {
        test_globs.push("*".to_string());
    }

    if std::env::var_os("DISABLE_DBG_OUTPUT").is_some() {
        DEBUG_ENABLED.store(false, Ordering::Relaxed);
    }

    let source_directory_is_required = || {
        eprintln!("No test root given, {program_name} requires the LADYBIRD_SOURCE_DIR environment variable to be set");
        1
    };

    let mut common_path = options.common_path.clone();
    let mut test_root = if options.specified_test_root.is_empty() {
        let Some(ladybird_source_dir) = std::env::var_os("LADYBIRD_SOURCE_DIR") else {
            return source_directory_is_required();
        };
        let ladybird_source_dir = ladybird_source_dir.to_string_lossy().into_owned();
        common_path = join_path(&ladybird_source_dir, "Tests/LibJS/Runtime/test-common.js");
        join_path(&ladybird_source_dir, TEST_ROOT_FRAGMENT)
    } else {
        options.specified_test_root.clone()
    };
    if !std::path::Path::new(&test_root).is_dir() {
        eprintln!("Test root is not a directory: {test_root}");
        return 1;
    }

    if common_path.is_empty() {
        let Some(ladybird_source_dir) = std::env::var_os("LADYBIRD_SOURCE_DIR") else {
            return source_directory_is_required();
        };
        common_path = join_path(
            &ladybird_source_dir.to_string_lossy(),
            "Tests/LibJS/Runtime/test-common.js",
        );
    }

    test_root = match real_path(&test_root) {
        Ok(test_root) => test_root,
        Err(error) => {
            eprintln!("Failed to resolve test root: {error}");
            return 1;
        }
    };

    common_path = match real_path(&common_path) {
        Ok(common_path) => common_path,
        Err(error) => {
            eprintln!("Failed to resolve common path: {error}");
            return 1;
        }
    };

    if let Err(error) = std::env::set_current_dir(&test_root) {
        eprintln!("chdir failed: {}", system_error_string("chdir", &error));
        return 1;
    }

    set_dump_bytecode(options.dump_bytecode);

    let vm = Vm::create();
    vm.set_dynamic_imports_allowed(true);

    // Configure the test VM to support additional import attributes
    // This allows tests to use import attributes beyond just "type"
    vm.set_host_get_supported_import_attributes(host_get_supported_import_attributes);

    let mut test_runner = TestRunner::new(TestRunnerOptions {
        test_root,
        common_path,
        print_times: options.print_times,
        print_progress: options.print_progress,
        print_json: options.print_json,
        detailed_json: options.per_file,
        print_each_test: options.print_each_test,
        collect_on_every_allocation: options.collect_on_every_allocation,
        run_test262_parser_tests: options.test262_parser_tests,
        invocation_cwd,
        program_name,
    });
    test_runner.run(&vm, &test_globs);

    drop(vm);

    i32::from(test_runner::COUNTS.tests_failed.get() > 0)
}

/// The entry point the C++ main of test-js-runtime-rust calls.
///
/// # Safety
///
/// `argv` must point to `argc` valid C strings, as main() receives them.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn libjs_runtime_rust_test_js_main(argc: c_int, argv: *const *const c_char) -> c_int {
    let arguments: Vec<String> = (0..usize::try_from(argc).unwrap_or(0))
        // SAFETY: The caller passes argc valid strings.
        .map(|index| {
            unsafe { CStr::from_ptr(*argv.add(index)) }
                .to_string_lossy()
                .into_owned()
        })
        .collect();
    let result = ladybird_main(&arguments);
    // Like exit(), which flushes C stdio.
    crate::standard_output::flush();
    result
}
