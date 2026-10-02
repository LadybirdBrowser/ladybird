/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! LibTest's TestRunner, Results.h and TestRunnerUtil.h: running the test files that match the globs, with the live
//! display or the progress the options ask for, and printing the totals as text or as JSON.

use core::ffi::c_char;
use core::sync::atomic::{AtomicBool, AtomicPtr, AtomicU32, Ordering};
use std::ffi::CString;
use std::io::Write;
use std::time::{SystemTime, UNIX_EPOCH};

use super::json::{JsonObject, JsonValue, format_double, format_double_with_precision};
use super::live_display::{Color, Counter, LabelColor, LiveDisplay, RenderTarget, stdout_is_tty};
use crate::standard_output::{out, outln};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TestResult {
    Pass,
    Fail,
    Skip,
    ExpectedFail,
    Crashed,
}

#[derive(Clone)]
pub struct Case {
    pub name: Vec<u8>,
    pub result: TestResult,
    pub details: Vec<u8>,
    pub duration_us: u64,
}

#[derive(Clone)]
pub struct Suite {
    pub path: String,
    pub name: Vec<u8>,
    // A failed test takes precedence over a skipped test, which both have
    // precedence over a passed test
    pub most_severe_test_result: TestResult,
    pub tests: Vec<Case>,
}

impl Suite {
    pub fn new(path: &str, name: Vec<u8>) -> Self {
        Self {
            path: path.to_string(),
            name,
            most_severe_test_result: TestResult::Pass,
            tests: Vec::new(),
        }
    }
}

/// A count the SIGINFO handler reads while the runner updates it.
#[derive(Default)]
pub struct Count(AtomicU32);

impl Count {
    pub fn get(&self) -> u32 {
        self.0.load(Ordering::Relaxed)
    }

    pub fn increment(&self) {
        self.0.fetch_add(1, Ordering::Relaxed);
    }
}

#[derive(Default)]
pub struct Counts {
    pub tests_failed: Count,
    pub tests_passed: Count,
    pub tests_skipped: Count,
    pub tests_expected_failed: Count,
    pub suites_failed: Count,
    pub suites_passed: Count,
    pub files_total: Count,
}

/// The counts of the one runner a process has, TestRunner::the()->counts() in C++.
pub static COUNTS: Counts = Counts {
    tests_failed: Count(AtomicU32::new(0)),
    tests_passed: Count(AtomicU32::new(0)),
    tests_skipped: Count(AtomicU32::new(0)),
    tests_expected_failed: Count(AtomicU32::new(0)),
    suites_failed: Count(AtomicU32::new(0)),
    suites_passed: Count(AtomicU32::new(0)),
    files_total: Count(AtomicU32::new(0)),
};

/// TestRunner::the()->is_printing_progress(), which cleanup() reads, also from the SIGABRT handler.
static PRINTING_PROGRESS: AtomicBool = AtomicBool::new(false);

/// g_currently_running_test, as a C string the SIGINFO handler prints.
static CURRENTLY_RUNNING_TEST: AtomicPtr<c_char> = AtomicPtr::new(core::ptr::null_mut());

pub fn set_currently_running_test(test_path: &str) {
    let test_path = CString::new(test_path).unwrap_or_default().into_raw();
    let previous = CURRENTLY_RUNNING_TEST.swap(test_path, Ordering::Relaxed);
    if !previous.is_null() {
        // SAFETY: Every pointer stored here came from CString::into_raw(). The SIGINFO handler runs on this thread, so it
        //         is not reading the previous string while it is freed.
        drop(unsafe { CString::from_raw(previous) });
    }
}

/// The test that is running, and the counts so far, in the format of the C++ SIGINFO handler.
pub fn write_counts_for_siginfo(buffer: &mut [u8]) -> usize {
    let mut cursor = std::io::Cursor::new(buffer);
    let current_test = CURRENTLY_RUNNING_TEST.load(Ordering::Relaxed);
    let _ = write!(
        cursor,
        "Pass: {}, Fail: {}, Skip: {}\nCurrent test: ",
        COUNTS.tests_passed.get(),
        COUNTS.tests_failed.get(),
        COUNTS.tests_skipped.get()
    );
    if !current_test.is_null() {
        // SAFETY: The pointer came from CString::into_raw() and is only freed on this thread.
        let _ = cursor.write_all(unsafe { core::ffi::CStr::from_ptr(current_test) }.to_bytes());
    }
    let _ = cursor.write_all(b"\n");
    usize::try_from(cursor.position()).unwrap_or(0)
}

pub fn cleanup() {
    // Clear the taskbar progress.
    if PRINTING_PROGRESS.load(Ordering::Relaxed) {
        eprint!("\x1b]9;-1;\x1b\\");
    }
}

pub fn cleanup_and_exit() -> ! {
    cleanup();
    crate::standard_output::flush();
    std::process::exit(1);
}

pub fn get_time_in_ms() -> f64 {
    let since_epoch = SystemTime::now().duration_since(UNIX_EPOCH).unwrap_or_default();
    since_epoch.as_millis() as f64
}

/// Every file below `directory_path`, depth first in the order the directory lists them, with symbolic links not
/// followed.
pub fn iterate_directory_recursively(directory_path: &str, callback: &mut dyn FnMut(String)) {
    let Ok(entries) = std::fs::read_dir(directory_path) else {
        return;
    };
    for entry in entries.flatten() {
        let name = entry.file_name().to_string_lossy().into_owned();
        let full_path = format!("{directory_path}/{name}");
        let Ok(file_type) = entry.file_type() else {
            continue;
        };
        // NB: The C++ runner also skips directories named "/Fixtures", which no name is.
        if file_type.is_dir() {
            iterate_directory_recursively(&full_path, callback);
        } else {
            callback(full_path);
        }
    }
}

/// AK's StringUtils::matches() with CaseSensitivity::CaseInsensitive: `*` matches any run of characters, `?` any one,
/// and a backslash escapes the character after it.
pub fn matches_glob(string: &[u8], mask: &[u8]) -> bool {
    if mask == b"*" {
        return true;
    }

    let mut string_index = 0;
    let mut mask_index = 0;
    while string_index < string.len() && mask_index < mask.len() {
        match mask[mask_index] {
            b'*' => {
                if mask_index == mask.len() - 1 {
                    return true;
                }
                while string_index < string.len() && !matches_glob(&string[string_index..], &mask[mask_index + 1..]) {
                    string_index += 1;
                }
                // The loop below advances past the character the rest of the mask matched from.
                string_index = string_index.wrapping_sub(1);
            }
            b'?' => {}
            mask_character => {
                let mut mask_character = mask_character;
                // if backslash is last character in mask, just treat it as an exact match
                // otherwise use it as escape for next character
                if mask_character == b'\\' && mask_index + 1 < mask.len() {
                    mask_index += 1;
                    mask_character = mask[mask_index];
                }
                if !mask_character.eq_ignore_ascii_case(&string[string_index]) {
                    return false;
                }
            }
        }
        string_index = string_index.wrapping_add(1);
        mask_index += 1;
    }

    if string_index == string.len() {
        // Allow ending '*' to contain nothing.
        while mask_index != mask.len() && mask[mask_index] == b'*' {
            mask_index += 1;
        }
    }

    string_index == string.len() && mask_index == mask.len()
}

/// LexicalPath::canonicalized_path() of an absolute path: without empty and "." parts, and with ".." resolved.
fn canonicalized_absolute_path(path: &str) -> String {
    let mut parts: Vec<&str> = Vec::new();
    for part in path.split('/') {
        match part {
            "" | "." => {}
            ".." => {
                parts.pop();
            }
            _ => parts.push(part),
        }
    }
    format!("/{}", parts.join("/"))
}

/// LexicalPath::relative_path(), which has no answer unless both paths are absolute.
pub fn relative_path(path: &str, prefix: &str) -> Option<String> {
    if !path.starts_with('/') || !prefix.starts_with('/') {
        return None;
    }

    if path == prefix {
        return Some(".".to_string());
    }

    // NOTE: Strip optional trailing slashes, except if the full path is only "/".
    let path = canonicalized_absolute_path(path);
    let prefix = canonicalized_absolute_path(prefix);

    if path == prefix {
        return Some(".".to_string());
    }

    // NOTE: Handle this special case first.
    if prefix == "/" {
        return Some(path[1..].to_string());
    }

    // NOTE: This means the path is a direct child of the prefix.
    if path.starts_with(&prefix) && path.as_bytes().get(prefix.len()) == Some(&b'/') {
        return Some(path[prefix.len() + 1..].to_string());
    }

    let path_parts: Vec<&str> = path.split('/').filter(|part| !part.is_empty()).collect();
    let prefix_parts: Vec<&str> = prefix.split('/').filter(|part| !part.is_empty()).collect();
    let index_of_first_part_that_differs = path_parts
        .iter()
        .zip(&prefix_parts)
        .take_while(|(path_part, prefix_part)| path_part == prefix_part)
        .count();

    let mut builder = "../".repeat(prefix_parts.len() - index_of_first_part_that_differs);
    builder.push_str(&path_parts[index_of_first_part_that_differs..].join("/"));
    Some(builder)
}

#[derive(Clone, Copy)]
pub enum Modifier {
    BgRed,
    BgGreen,
    FgRed,
    FgGreen,
    FgOrange,
    FgGray,
    FgBlack,
    FgBold,
    Italic,
    Clear,
}

pub fn print_modifiers(modifiers: &[Modifier]) {
    // Strip ANSI escapes for non-tty output.
    if !stdout_is_tty() {
        return;
    }
    for modifier in modifiers {
        let code = match modifier {
            Modifier::BgRed => "\x1b[41m",
            Modifier::BgGreen => "\x1b[42m",
            Modifier::FgRed => "\x1b[31m",
            Modifier::FgGreen => "\x1b[32m",
            Modifier::FgOrange => "\x1b[33m",
            Modifier::FgGray => "\x1b[90m",
            Modifier::FgBlack => "\x1b[30m",
            Modifier::FgBold => "\x1b[1m",
            Modifier::Italic => "\x1b[3m",
            Modifier::Clear => "\x1b[0m",
        };
        out(code.as_bytes());
    }
}

/// The JavaScript test runner, Test::JS::TestRunner with the ::Test::TestRunner it extends.
pub struct TestRunner {
    pub test_root: String,
    pub common_path: String,
    pub print_times: bool,
    pub print_progress: bool,
    pub print_json: bool,
    pub detailed_json: bool,
    pub print_each_test: bool,
    pub collect_on_every_allocation: bool,
    pub run_test262_parser_tests: bool,

    pub total_elapsed_time_in_ms: f64,
    pub suites: Option<Vec<Suite>>,

    live_display: LiveDisplay,
    current_test_label: String,
    invocation_cwd: String,
    program_name: String,
}

pub struct TestRunnerOptions {
    pub test_root: String,
    pub common_path: String,
    pub print_times: bool,
    pub print_progress: bool,
    pub print_json: bool,
    pub detailed_json: bool,
    pub print_each_test: bool,
    pub collect_on_every_allocation: bool,
    pub run_test262_parser_tests: bool,
    pub invocation_cwd: String,
    pub program_name: String,
}

pub const TOP_LEVEL_TEST_NAME: &[u8] = b"__$$TOP_LEVEL$$__";

impl TestRunner {
    pub fn new(options: TestRunnerOptions) -> Self {
        PRINTING_PROGRESS.store(options.print_progress, Ordering::Relaxed);
        Self {
            test_root: options.test_root,
            common_path: options.common_path,
            print_times: options.print_times,
            print_progress: options.print_progress,
            print_json: options.print_json,
            detailed_json: options.detailed_json,
            print_each_test: options.print_each_test,
            collect_on_every_allocation: options.collect_on_every_allocation,
            run_test262_parser_tests: options.run_test262_parser_tests,
            total_elapsed_time_in_ms: 0.0,
            suites: None,
            live_display: LiveDisplay::default(),
            current_test_label: String::new(),
            invocation_cwd: options.invocation_cwd,
            program_name: options.program_name,
        }
    }

    pub fn needs_detailed_suites(&self) -> bool {
        self.detailed_json
    }

    pub fn needs_timings(&self) -> bool {
        self.print_times
    }

    pub fn ensure_suites(&mut self) -> &mut Vec<Suite> {
        self.suites.get_or_insert_with(Vec::new)
    }

    fn begin_live_display(&mut self) -> bool {
        let program = if self.program_name.is_empty() {
            "test-runner"
        } else {
            self.program_name.as_str()
        };
        let log_path = if self.invocation_cwd.is_empty() {
            format!("./{program}.log")
        } else {
            format!("{}/{program}.log", self.invocation_cwd)
        };

        self.live_display.begin(3, log_path)
    }

    fn render_live_display(&mut self, completed: usize, total: usize) {
        let current_test_label = self.current_test_label.as_bytes();
        self.live_display.render(|target: &mut RenderTarget<'_>| {
            target.line(|target| {
                if current_test_label.is_empty() {
                    target.label(
                        "⏺ (idle)",
                        b"",
                        LabelColor {
                            prefix: Color::Gray,
                            text: Color::None,
                        },
                    );
                } else {
                    target.label(
                        "⏺ ",
                        current_test_label,
                        LabelColor {
                            prefix: Color::Yellow,
                            text: Color::None,
                        },
                    );
                }
            });
            target.line(|target| {
                target.counter(&[
                    Counter {
                        label: "Pass",
                        color: Color::Green,
                        value: COUNTS.tests_passed.get(),
                    },
                    Counter {
                        label: "Fail",
                        color: Color::Red,
                        value: COUNTS.tests_failed.get(),
                    },
                    Counter {
                        label: "Skipped",
                        color: Color::Gray,
                        value: COUNTS.tests_skipped.get(),
                    },
                    Counter {
                        label: "XFail",
                        color: Color::Yellow,
                        value: COUNTS.tests_expected_failed.get(),
                    },
                ]);
            });
            target.line(|target| target.progress_bar(completed, total));
        });
    }

    pub fn run(&mut self, vm: &crate::interpreter::vm::Vm, test_globs: &[String]) {
        let test_paths = self.get_test_paths();
        let matches_any_glob = |path: &str| {
            test_globs
                .iter()
                .any(|glob| matches_glob(path.as_bytes(), glob.as_bytes()))
        };
        let total_tests = test_paths.iter().filter(|path| matches_any_glob(path)).count();

        let live_display_enabled =
            !self.print_json && !self.print_each_test && stdout_is_tty() && self.begin_live_display();

        if live_display_enabled {
            self.render_live_display(0, total_tests);
        }

        let mut progress_counter = 0;
        for path in &test_paths {
            if !matches_any_glob(path) {
                continue;
            }
            progress_counter += 1;

            if live_display_enabled {
                self.current_test_label = relative_path(path, &self.test_root).unwrap_or_else(|| path.clone());
                self.render_live_display(progress_counter - 1, total_tests);
            }

            if self.print_each_test {
                let label = relative_path(path, &self.test_root).unwrap_or_else(|| path.clone());
                eprintln!("[{progress_counter}/{total_tests}] {label}");
            }

            self.do_run_single_test(vm, path);

            if live_display_enabled {
                self.render_live_display(progress_counter, total_tests);
            }

            if self.print_progress {
                eprint!("\x1b]9;{progress_counter};{total_tests};\x1b\\");
            }
        }

        let saved_log_path = self.live_display.log_file_path().to_string();
        self.live_display.end();

        if self.print_progress {
            eprint!("\x1b]9;-1;\x1b\\");
        }

        if self.print_json {
            self.print_test_results_as_json();
        } else {
            self.print_test_results();
        }

        if live_display_enabled && !saved_log_path.is_empty() {
            outln(format!("Full test output: {saved_log_path}").as_bytes());
        }
    }

    fn print_test_results(&self) {
        let print_count = |modifier: Modifier, text: String| {
            print_modifiers(&[modifier]);
            out(text.as_bytes());
            print_modifiers(&[Modifier::Clear]);
        };

        out(b"\nTest Suites: ");
        if COUNTS.suites_failed.get() != 0 {
            print_count(Modifier::FgRed, format!("{} failed, ", COUNTS.suites_failed.get()));
        }
        if COUNTS.suites_passed.get() != 0 {
            print_count(Modifier::FgGreen, format!("{} passed, ", COUNTS.suites_passed.get()));
        }
        outln(format!("{} total", COUNTS.suites_failed.get() + COUNTS.suites_passed.get()).as_bytes());

        out(b"Tests:       ");
        if COUNTS.tests_failed.get() != 0 {
            print_count(Modifier::FgRed, format!("{} failed, ", COUNTS.tests_failed.get()));
        }
        if COUNTS.tests_skipped.get() != 0 {
            print_count(Modifier::FgOrange, format!("{} skipped, ", COUNTS.tests_skipped.get()));
        }
        if COUNTS.tests_expected_failed.get() != 0 {
            print_count(
                Modifier::FgOrange,
                format!("{} expected failed, ", COUNTS.tests_expected_failed.get()),
            );
        }
        if COUNTS.tests_passed.get() != 0 {
            print_count(Modifier::FgGreen, format!("{} passed, ", COUNTS.tests_passed.get()));
        }
        outln(
            format!(
                "{} total",
                COUNTS.tests_failed.get()
                    + COUNTS.tests_skipped.get()
                    + COUNTS.tests_passed.get()
                    + COUNTS.tests_expected_failed.get()
            )
            .as_bytes(),
        );

        outln(format!("Files:       {} total", COUNTS.files_total.get()).as_bytes());

        out(b"Time:        ");
        if self.total_elapsed_time_in_ms < 1000.0 {
            outln(format!("{}ms", self.total_elapsed_time_in_ms as i32).as_bytes());
        } else {
            outln(
                format!(
                    "{}s",
                    format_double_with_precision(self.total_elapsed_time_in_ms / 1000.0, 3)
                )
                .as_bytes(),
            );
        }
        outln(b"");
    }

    fn print_test_results_as_json(&self) {
        let mut root = JsonObject::default();
        if self.needs_detailed_suites() {
            let suites = self.suites.as_deref().unwrap_or_default();
            let mut duration_us: u64 = 0;
            let mut tests = JsonObject::default();

            for suite in suites {
                for case in &suite.tests {
                    duration_us = duration_us.wrapping_add(case.duration_us);
                    let result_name = match case.result {
                        TestResult::Pass => "PASSED",
                        TestResult::Fail => "FAILED",
                        TestResult::Skip => "SKIPPED",
                        TestResult::ExpectedFail => "XFAIL",
                        TestResult::Crashed => "PROCESS_ERROR",
                    };

                    let name: &[u8] = if suite.name == TOP_LEVEL_TEST_NAME {
                        b""
                    } else {
                        &suite.name
                    };

                    let path = relative_path(&suite.path, &self.test_root)
                        .expect("the path of a suite is below the test root");

                    let mut key = path.into_bytes();
                    key.push(b'/');
                    key.extend_from_slice(name);
                    key.extend_from_slice(b"::");
                    key.extend_from_slice(&case.name);
                    tests.set(key, result_name);
                }
            }

            root.set("duration", duration_us as f64 / 1_000_000.0);
            root.set("results", tests);
        } else {
            let mut suites = JsonObject::default();
            suites.set("failed", COUNTS.suites_failed.get());
            suites.set("passed", COUNTS.suites_passed.get());
            suites.set("total", COUNTS.suites_failed.get() + COUNTS.suites_passed.get());

            let mut tests = JsonObject::default();
            tests.set("failed", COUNTS.tests_failed.get());
            tests.set("passed", COUNTS.tests_passed.get());
            tests.set("skipped", COUNTS.tests_skipped.get());
            tests.set("xfail", COUNTS.tests_expected_failed.get());
            tests.set(
                "total",
                COUNTS.tests_failed.get()
                    + COUNTS.tests_passed.get()
                    + COUNTS.tests_skipped.get()
                    + COUNTS.tests_expected_failed.get(),
            );

            let mut results = JsonObject::default();
            results.set("suites", suites);
            results.set("tests", tests);

            root.set("results", results);
            root.set("files_total", COUNTS.files_total.get());
            root.set("duration", self.total_elapsed_time_in_ms / 1000.0);
        }
        outln(&JsonValue::from(root).serialized());
    }
}

/// The time a file took, as print_file_result() shows it: whole milliseconds below a second, and the seconds as "{:3}"
/// formats them otherwise, at least three characters wide and padded on the right.
pub fn format_file_time(time_taken_in_ms: f64) -> String {
    if time_taken_in_ms < 1000.0 {
        format!(" ({}ms)", time_taken_in_ms as i32)
    } else {
        format!(" ({:<3}s)", format_double(time_taken_in_ms / 1000.0))
    }
}
