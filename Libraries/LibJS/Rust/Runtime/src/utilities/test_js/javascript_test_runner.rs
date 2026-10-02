/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! LibTest's JavaScriptTestRunner.h: the global object the tests run with, and the run of one test file, which runs
//! test-common.js and the file in a new realm and collects the results the harness recorded.

use core::cell::Cell;

use ak::{Utf16FlyString, Utf16String};
use libjs_runtime_macros::Trace;

use super::json::{JsonParser, JsonValue};
use super::test_js_functions::{EXPOSED_GLOBAL_FUNCTIONS, RunFileHookResult, run_file};
use super::test_runner::{
    COUNTS, Case, Modifier, Suite, TOP_LEVEL_TEST_NAME, TestResult, TestRunner, cleanup_and_exit, format_file_time,
    get_time_in_ms, iterate_directory_recursively, print_modifiers, set_currently_running_test,
};
use crate::gc::class::{GcCell, define_cell};
use crate::hash_table::HashTable;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::parser_error::ParserError;
use crate::runtime::array::Array;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::error::Error;
use crate::runtime::error_data::CompactTraceback;
use crate::runtime::global_object::GlobalObject;
use crate::runtime::json_object::JSONObject;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::property_attributes::{Attribute, DEFAULT_ATTRIBUTES, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::source_text_module::SourceTextModule;
use crate::script::Script;
use crate::source_code::SourceCode;
use crate::standard_output::{out, outln};
use crate::utf16::{Utf16View, utf16_from_wtf8};
use crate::utilities::js::system_error_string;

fn key(name: &str) -> PropertyKey {
    PropertyKey::from(Utf16FlyString::from_utf8(name))
}

fn wtf8_of(string: &Utf16String) -> Vec<u8> {
    Utf16View::of_string(string).to_wtf8()
}

/// Test::JS::TestRunnerGlobalObject.
#[repr(C)]
#[derive(Trace)]
pub struct TestRunnerGlobalObject {
    base: GlobalObject,
    /// Whether on_test_reported is set, which it is to print_test_timings() when the runner shows the time of each
    /// test.
    prints_test_timings: Cell<bool>,
}

define_object_class!(TestRunnerGlobalObject, extends: [GlobalObject, Object], methods: {
    initialize: TestRunnerGlobalObject::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl TestRunnerGlobalObject {
    /// realm->create<TestRunnerGlobalObject>(), which already runs initialize() once before InitializeHostDefinedRealm
    /// runs it again after SetDefaultGlobalBindings, so the properties it defines come before the default bindings.
    pub fn create(vm: &Vm, realm: Gc<Realm>, prints_test_timings: bool) -> Gc<TestRunnerGlobalObject> {
        realm.create_object(
            vm,
            TestRunnerGlobalObject {
                base: GlobalObject::new(vm, Self::CLASS, realm),
                prints_test_timings: Cell::new(prints_test_timings),
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
        object.define_native_function(
            vm,
            realm,
            &key("__reportTest__"),
            raw_native!(TestRunnerGlobalObject::report_test),
            2,
            DEFAULT_ATTRIBUTES,
            None,
        );

        // NB: The C++ runner keeps these functions in a HashMap, and defines them in the order of its buckets.
        let mut names = HashTable::default();
        for (name, _) in EXPOSED_GLOBAL_FUNCTIONS {
            names.set(Utf16FlyString::from_utf8(name));
        }
        for name in names.iter() {
            let (_, function) = EXPOSED_GLOBAL_FUNCTIONS
                .iter()
                .find(|(function_name, _)| Utf16FlyString::from_utf8(function_name) == *name)
                .expect("every name is the name of a function");
            object.define_native_function(
                vm,
                realm,
                &PropertyKey::from(name.clone()),
                *function,
                1,
                DEFAULT_ATTRIBUTES,
                None,
            );
        }
    }

    fn report_test(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("__reportTest__ runs in a realm");
        let this = realm
            .global_object()
            .downcast::<TestRunnerGlobalObject>()
            .expect("the tests run with a TestRunnerGlobalObject");
        if !this.prints_test_timings.get() {
            return Ok(Value::UNDEFINED);
        }

        let test_name_value = vm.argument(0);
        let test_name = wtf8_of(&test_name_value.to_utf16_string(vm)?);
        let state_value = vm.argument(1);
        print_test_timings(&test_name, state_value);
        Ok(Value::UNDEFINED)
    }
}

/// A file that failed to parse: the first error, and the line it points at.
pub struct FileParserError {
    pub error: ParserError,
    pub hint: Vec<u16>,
}

pub struct JSFileResult {
    pub name: String,
    pub error: Option<FileParserError>,
    pub time_taken: f64,
    // A failed test takes precedence over a skipped test, which both have
    // precedence over a passed test
    pub most_severe_test_result: TestResult,
    pub suites: Vec<Suite>,
    pub logged_messages: Vec<Vec<u8>>,
}

impl JSFileResult {
    fn named(name: String) -> Self {
        Self {
            name,
            error: None,
            time_taken: 0.0,
            most_severe_test_result: TestResult::Pass,
            suites: Vec::new(),
            logged_messages: Vec::new(),
        }
    }
}

fn load_entire_file(path: &str) -> Vec<u8> {
    let contents = std::fs::File::open(path)
        .map_err(|error| system_error_string("open", &error))
        .and_then(|mut file| {
            let mut contents = Vec::new();
            std::io::Read::read_to_end(&mut file, &mut contents)
                .map(|_| contents)
                .map_err(|error| system_error_string("read", &error))
        });
    match contents {
        Ok(contents) => contents,
        Err(error) => {
            eprintln!("Failed to open the following file: \"{path}\", error: {error}");
            cleanup_and_exit();
        }
    }
}

/// The code units of a test file, which Utf16String::from_utf8() decodes, stopping the process if it is not UTF-8.
fn source_text_of(path: &str) -> Vec<u16> {
    let contents = load_entire_file(path);
    utf16_from_wtf8(&contents).expect("Utf16String::from_utf8() takes valid UTF-8")
}

fn file_parser_error(errors: Vec<ParserError>, source_text: &[u16]) -> FileParserError {
    let error = errors.into_iter().next().expect("a failed parse reports an error");
    let hint = error.source_location_hint(source_text, b' ', b'^');
    FileParserError { error, hint }
}

pub fn parse_script(vm: &Vm, path: &str, realm: Gc<Realm>) -> Result<Gc<Script>, FileParserError> {
    let source_text = source_text_of(path);
    Script::parse_with_filename(vm, &source_text, realm, path).map_err(|errors| file_parser_error(errors, &source_text))
}

pub fn parse_module(vm: &Vm, path: &str, realm: Gc<Realm>) -> Result<Gc<SourceTextModule>, FileParserError> {
    let source_text = source_text_of(path);
    let source_code = SourceCode::create(Utf16String::from_utf8(path), Utf16String::from_utf16(&source_text));
    SourceTextModule::parse(vm, source_code, realm, path).map_err(|errors| file_parser_error(errors, &source_text))
}

fn get_test_results(vm: &Vm, realm: Gc<Realm>) -> Result<JsonValue, &'static str> {
    let results = realm.global_object().get(vm, &key("__TestResults__")).must();
    let maybe_json_string = JSONObject::stringify_impl(vm, results, Value::UNDEFINED, Value::UNDEFINED).must();
    match maybe_json_string {
        Some(json_string) => JsonParser::parse(&wtf8_of(&json_string)),
        None => Ok(JsonValue::Null),
    }
}

fn print_test_timings(test_name: &[u8], state_value: Value) {
    if !state_value.is_string() {
        return;
    }
    let state_string = state_value.as_string().to_utf8();
    let (modifiers, label, suffix): (&[Modifier], &str, &str) = match state_string.as_str() {
        "pass" => (&[Modifier::FgBold], "Finished: ", " (PASS)"),
        "fail" => (&[Modifier::FgRed, Modifier::FgBold], "Finished: ", " (FAIL)"),
        "xfail" => (&[Modifier::FgOrange, Modifier::FgBold], "Finished: ", " (XFAIL)"),
        "start" => (&[Modifier::BgGreen, Modifier::FgOrange], "Running: ", ""),
        _ => return,
    };
    print_modifiers(modifiers);
    out(label.as_bytes());
    print_modifiers(&[Modifier::Clear]);
    let mut line = test_name.to_vec();
    line.extend_from_slice(suffix.as_bytes());
    outln(&line);
}

/// The details of the test that reports what the top level of a file threw: the name and the message of an error,
/// and the stack of an Error.
fn describe_top_level_error(vm: &Vm, error: Value) -> Vec<u8> {
    if !error.is_object() {
        return wtf8_of(&error.to_utf16_string_without_side_effects());
    }

    let mut detail_builder = Vec::new();

    let error_object = error.as_object();
    let name = error_object.get_without_side_effects(vm, &vm.names.name);
    let message = error_object.get_without_side_effects(vm, &vm.names.message);

    if name.is_accessor() || message.is_accessor() {
        detail_builder.extend(wtf8_of(&error.to_utf16_string_without_side_effects()));
    } else {
        detail_builder.extend(wtf8_of(&name.to_utf16_string_without_side_effects()));
        detail_builder.extend_from_slice(b": ");
        detail_builder.extend(wtf8_of(&message.to_utf16_string_without_side_effects()));
    }

    if let Some(error_as_error) = error_object.downcast::<Error>() {
        detail_builder.push(b'\n');
        detail_builder.extend(wtf8_of(&error_as_error.stack_string(CompactTraceback::No)));
    }

    detail_builder
}

impl TestRunner {
    pub fn do_run_single_test(&mut self, vm: &Vm, test_path: &str) {
        let file_result = self.run_file_test(vm, test_path);
        if !self.print_json {
            self.print_file_result(&file_result);
        }

        if self.needs_detailed_suites() {
            self.ensure_suites().extend(file_result.suites);
        }
    }

    pub fn get_test_paths(&self) -> Vec<String> {
        let mut paths = Vec::new();
        iterate_directory_recursively(&self.test_root, &mut |file_path| {
            if !file_path.ends_with(".js") {
                return;
            }
            if !file_path.ends_with("test-common.js") {
                paths.push(file_path);
            }
        });
        paths.sort_unstable();
        paths
    }

    fn run_file_test(&mut self, vm: &Vm, test_path: &str) -> JSFileResult {
        set_currently_running_test(test_path);

        let start_time = get_time_in_ms();

        let prints_test_timings = self.needs_timings();
        let create_global_object = |realm: Gc<Realm>| -> Gc<Object> {
            TestRunnerGlobalObject::create(vm, realm, prints_test_timings).upcast()
        };
        let root_execution_context = Realm::initialize_host_defined_realm(vm, Some(&create_global_object), None).must();
        let realm = root_execution_context
            .realm
            .get()
            .expect("InitializeHostDefinedRealm sets the realm of its context");
        let global_execution_context = root_execution_context.as_non_null();
        vm.pop_execution_context();

        vm.heap()
            .set_should_collect_on_every_allocation(self.collect_on_every_allocation);

        match run_file(vm, self.run_test262_parser_tests, &self.test_root, test_path, realm) {
            Err(RunFileHookResult::SkipFile) => {
                return JSFileResult {
                    most_severe_test_result: TestResult::Skip,
                    ..JSFileResult::named(test_path.to_string())
                };
            }
            Ok(value) => {
                for suite in &value.suites {
                    if suite.most_severe_test_result == TestResult::Pass {
                        COUNTS.suites_passed.increment();
                    } else if suite.most_severe_test_result == TestResult::Fail {
                        COUNTS.suites_failed.increment();
                    }
                    for test in &suite.tests {
                        match test.result {
                            TestResult::Pass => COUNTS.tests_passed.increment(),
                            TestResult::Fail => COUNTS.tests_failed.increment(),
                            TestResult::Skip => COUNTS.tests_skipped.increment(),
                            TestResult::ExpectedFail | TestResult::Crashed => {}
                        }
                    }
                }
                COUNTS.files_total.increment();
                self.total_elapsed_time_in_ms += value.time_taken;

                return value;
            }
            Err(RunFileHookResult::RunAsNormal) => {}
        }

        // FIXME: Since a new realm is created every time, we no longer cache the test-common.js file as scripts are parsed for the current realm only.
        //        Find a way to cache this.
        let test_script = match parse_script(vm, &self.common_path, realm) {
            Ok(test_script) => test_script,
            Err(error) => {
                eprintln!("Unable to parse test-common.js");
                eprintln!("{}", error.error);
                eprintln!("{}", String::from_utf8_lossy(&Utf16View::Utf16(&error.hint).to_wtf8()));
                cleanup_and_exit();
            }
        };

        vm.push_execution_context(global_execution_context);
        vm.run_script(test_script, None).must();
        vm.pop_execution_context();

        let file_script = match parse_script(vm, test_path, realm) {
            Ok(file_script) => file_script,
            Err(error) => {
                COUNTS.suites_failed.increment();
                COUNTS.files_total.increment();
                return JSFileResult {
                    error: Some(error),
                    ..JSFileResult::named(test_path.to_string())
                };
            }
        };
        vm.push_execution_context(global_execution_context);
        let top_level_result = vm.run_script(file_script, None);
        vm.pop_execution_context();

        vm.push_execution_context(global_execution_context);
        let test_json = get_test_results(vm, realm);
        vm.pop_execution_context();
        let Ok(test_json) = test_json else {
            eprintln!("Received malformed JSON from test \"{test_path}\"");
            cleanup_and_exit();
        };

        let mut file_result = JSFileResult::named(test_path[self.test_root.len() + 1..].to_string());

        // Collect logged messages
        let user_output = realm.global_object().get(vm, &key("__UserOutput__")).must();

        assert!(user_output.is_object(), "__UserOutput__ is an Array");
        let array = user_output
            .as_object()
            .downcast::<Array>()
            .expect("__UserOutput__ is an Array");
        for i in 0..array.indexed_array_like_size() {
            let message = array.get(vm, &PropertyKey::from(i)).must();
            file_result
                .logged_messages
                .push(wtf8_of(&message.to_utf16_string_without_side_effects()));
        }

        let test_results = test_json.as_object().expect("the test results are an object");
        for (suite_name, suite_value) in test_results.members() {
            let mut suite = Suite::new(test_path, suite_name.to_vec());

            let suite_value = suite_value.as_object().expect("a suite is an object");

            for (test_name, test_value) in suite_value.members() {
                let mut test = Case {
                    name: test_name.to_vec(),
                    result: TestResult::Fail,
                    details: Vec::new(),
                    duration_us: 0,
                };

                let test_value = test_value.as_object().expect("a test is an object");
                assert!(test_value.has("result"), "a test has a result");

                let result_string = test_value
                    .get_string("result")
                    .expect("the result of a test is a string");
                match result_string {
                    b"pass" => {
                        test.result = TestResult::Pass;
                        COUNTS.tests_passed.increment();
                    }
                    b"fail" => {
                        test.result = TestResult::Fail;
                        COUNTS.tests_failed.increment();
                        suite.most_severe_test_result = TestResult::Fail;
                        assert!(test_value.has("details"), "a failed test has details");
                        test.details = test_value
                            .get_string("details")
                            .expect("the details of a test are a string")
                            .to_vec();
                    }
                    b"xfail" => {
                        test.result = TestResult::ExpectedFail;
                        COUNTS.tests_expected_failed.increment();
                        if suite.most_severe_test_result != TestResult::Fail {
                            suite.most_severe_test_result = TestResult::ExpectedFail;
                        }
                    }
                    _ => {
                        test.result = TestResult::Skip;
                        if suite.most_severe_test_result == TestResult::Pass {
                            suite.most_severe_test_result = TestResult::Skip;
                        }
                        COUNTS.tests_skipped.increment();
                    }
                }

                test.duration_us = test_value.get_u64("duration").unwrap_or(0);

                suite.tests.push(test);
            }

            if suite.most_severe_test_result == TestResult::Fail {
                COUNTS.suites_failed.increment();
                file_result.most_severe_test_result = TestResult::Fail;
            } else {
                if suite.most_severe_test_result == TestResult::Skip
                    && file_result.most_severe_test_result == TestResult::Pass
                {
                    file_result.most_severe_test_result = TestResult::Skip;
                } else if suite.most_severe_test_result == TestResult::ExpectedFail
                    && (file_result.most_severe_test_result == TestResult::Pass
                        || file_result.most_severe_test_result == TestResult::Skip)
                {
                    file_result.most_severe_test_result = TestResult::ExpectedFail;
                }
                COUNTS.suites_passed.increment();
            }

            file_result.suites.push(suite);
        }

        if let Err(throw) = top_level_result {
            let mut suite = Suite::new(test_path, b"<top-level>".to_vec());
            suite.most_severe_test_result = TestResult::Crashed;

            suite.tests.push(Case {
                name: b"<top-level>".to_vec(),
                result: TestResult::Fail,
                details: describe_top_level_error(vm, throw.value()),
                duration_us: 0,
            });

            file_result.suites.push(suite);

            COUNTS.suites_failed.increment();
            file_result.most_severe_test_result = TestResult::Fail;
        }

        COUNTS.files_total.increment();

        file_result.time_taken = get_time_in_ms() - start_time;
        self.total_elapsed_time_in_ms += file_result.time_taken;

        drop(root_execution_context);
        file_result
    }

    fn print_file_result(&self, file_result: &JSFileResult) {
        if file_result.most_severe_test_result == TestResult::Fail || file_result.error.is_some() {
            print_modifiers(&[Modifier::BgRed, Modifier::FgBold]);
            out(b" FAIL ");
            print_modifiers(&[Modifier::Clear]);
        } else if self.print_times || file_result.most_severe_test_result != TestResult::Pass {
            print_modifiers(&[Modifier::BgGreen, Modifier::FgBlack, Modifier::FgBold]);
            out(b" PASS ");
            print_modifiers(&[Modifier::Clear]);
        } else {
            return;
        }

        out(format!(" {}", file_result.name).as_bytes());

        if self.print_times {
            print_modifiers(&[Modifier::Clear, Modifier::Italic, Modifier::FgGray]);
            outln(format_file_time(file_result.time_taken).as_bytes());
            print_modifiers(&[Modifier::Clear]);
        } else {
            outln(b"");
        }

        if !file_result.logged_messages.is_empty() {
            print_modifiers(&[Modifier::FgGray, Modifier::FgBold]);
            outln("    ℹ️  Console output:".as_bytes());
            print_modifiers(&[Modifier::Clear, Modifier::FgGray]);
            for message in &file_result.logged_messages {
                outln_indented(b"         ", message);
            }
        }

        if let Some(test_error) = &file_result.error {
            print_modifiers(&[Modifier::FgRed]);
            outln("    ❌ The file failed to parse".as_bytes());
            outln(b"");
            print_modifiers(&[Modifier::FgGray]);
            for message in test_error.hint.split(|&code_unit| code_unit == u16::from(b'\n')) {
                outln_indented(b"         ", &Utf16View::Utf16(message).to_wtf8());
            }
            print_modifiers(&[Modifier::FgRed]);
            outln(format!("         {}", test_error.error).as_bytes());
            outln(b"");
            return;
        }

        if file_result.most_severe_test_result != TestResult::Pass {
            for suite in &file_result.suites {
                if suite.most_severe_test_result == TestResult::Pass {
                    continue;
                }

                let failed = suite.most_severe_test_result == TestResult::Fail;

                print_modifiers(&[Modifier::FgGray, Modifier::FgBold]);

                if failed {
                    out("    ❌ Suite:  ".as_bytes());
                } else {
                    out("    ⚠️  Suite:  ".as_bytes());
                }

                print_modifiers(&[Modifier::Clear, Modifier::FgGray]);

                if suite.name == TOP_LEVEL_TEST_NAME {
                    outln(b"<top-level>");
                } else {
                    outln(&suite.name);
                }
                print_modifiers(&[Modifier::Clear]);

                for test in &suite.tests {
                    if test.result == TestResult::Pass {
                        continue;
                    }

                    print_modifiers(&[Modifier::FgGray, Modifier::FgBold]);
                    out(b"         Test:   ");
                    if test.result == TestResult::Fail {
                        print_modifiers(&[Modifier::Clear, Modifier::FgRed]);
                        let mut line = test.name.clone();
                        line.extend_from_slice(b" (failed):");
                        outln(&line);
                        outln_indented(b"                 ", &test.details);
                    } else if test.result == TestResult::ExpectedFail {
                        print_modifiers(&[Modifier::Clear, Modifier::FgOrange]);
                        let mut line = test.name.clone();
                        line.extend_from_slice(b" (expected fail)");
                        outln(&line);
                    } else {
                        print_modifiers(&[Modifier::Clear, Modifier::FgOrange]);
                        let mut line = test.name.clone();
                        line.extend_from_slice(b" (skipped)");
                        outln(&line);
                    }
                    print_modifiers(&[Modifier::Clear]);
                }
            }
        }
    }
}

fn outln_indented(indentation: &[u8], text: &[u8]) {
    let mut line = indentation.to_vec();
    line.extend_from_slice(text);
    outln(&line);
}
