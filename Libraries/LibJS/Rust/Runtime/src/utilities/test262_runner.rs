/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! test262-runner-rust: runs test262 tests on the Rust runtime, with the protocol of Utilities/test262-runner.cpp.
//!
//! The runner reads test paths from standard input, one per line. For each test it writes `RESULT <json>`, a NUL and a
//! newline to standard output, and after the last one `DONE <count>`. While a test runs, standard output is a
//! non-blocking pipe, and the start of whatever the test printed is reported in its result. A test that runs longer
//! than the timeout is killed by SIGALRM, which the driver records as a timeout. A panic, including one from a runtime
//! function that is not implemented yet, reports `assert_fail` for the running test and exits with status 12, like a
//! failed assertion in the C++ runner.
//!
//! With --agent, the runner is instead one of the agents that a test starts with $262.agent.start(): it runs the
//! agent's script, which the test sends it, in a VM of its own (see contrib/test262/agents.rs).

use core::ffi::{c_char, c_int};
use std::collections::HashMap;
use std::ffi::CStr;
use std::fs::File;
use std::io::{BufRead, Write};
use std::mem::ManuallyDrop;
use std::os::fd::{FromRawFd, RawFd};
use std::sync::{Arc, Mutex, PoisonError};
use std::time::Instant;

use ak::Utf16String;

#[cfg(unix)]
use crate::contrib::test262::agents::{self, AgentFailure};
use crate::contrib::test262::global_object::Test262GlobalObject;
use crate::interpreter::vm::{Vm, VmOptions};
use crate::layout::cell::Gc;
use crate::layout::realm::Realm;
use crate::layout::value::Value;
use crate::parser_error::ParserError;
use crate::runtime::source_text_module::SourceTextModule;
use crate::script::Script;
use crate::source_code::SourceCode;
use crate::utf16::{Utf16View, string_from_utf8_with_replacement_character, utf16_from_wtf8};
use crate::utilities::initialize_realm_with_global_object;
use libjs_rust::ast::ProgramType;
use libjs_rust::compile::parse;

const EXIT_STDOUT_SETUP_FAILED: c_int = 1;
const EXIT_WRONG_ARGUMENTS: c_int = 2;
const EXIT_READ_FILE_FAILURE: c_int = 3;
const EXIT_SETUP_INPUT_FAILURE: c_int = 7;
const EXIT_ASSERTION_FAILED: c_int = 12;

const STA_HARNESS_FILE: &str = "sta.js";
const ASSERT_HARNESS_FILE: &str = "assert.js";
const ASYNC_INCLUDE: &str = "doneprintHandle.js";
const USE_STRICT_PREFIX: &str = "'use strict';\n";
const COLLECTED_OUTPUT_LIMIT: usize = 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum NegativePhase {
    ParseOrEarly,
    Resolution,
    Runtime,
    Harness,
}

impl NegativePhase {
    fn name(self) -> &'static str {
        match self {
            Self::ParseOrEarly => "parse",
            Self::Resolution => "resolution",
            Self::Runtime => "runtime",
            Self::Harness => "harness",
        }
    }
}

struct TestError {
    phase: NegativePhase,
    error_type: String,
    details: String,
    harness_file: String,
}

impl TestError {
    fn syntax_error(phase: NegativePhase, details: String, harness_file: &str) -> Self {
        Self {
            phase,
            error_type: "SyntaxError".to_string(),
            details,
            harness_file: harness_file.to_string(),
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum StrictMode {
    Both,
    NoStrict,
    OnlyStrict,
}

struct TestMetadata<'source> {
    harness_files: Vec<&'source str>,
    skip_test: bool,
    strict_mode: StrictMode,
    program_type: ProgramType,
    is_async: bool,
    is_negative: bool,
    phase: NegativePhase,
    negative_type: &'source str,
}

impl Default for TestMetadata<'_> {
    fn default() -> Self {
        Self {
            harness_files: vec![STA_HARNESS_FILE, ASSERT_HARNESS_FILE],
            skip_test: false,
            strict_mode: StrictMode::Both,
            program_type: ProgramType::Script,
            is_async: false,
            is_negative: false,
            phase: NegativePhase::ParseOrEarly,
            negative_type: "",
        }
    }
}

/// The lines of `text` as AK's StringView::lines() splits them: at "\n", "\r" and "\r\n", keeping empty lines but not
/// an empty last line.
fn lines(text: &str) -> Vec<&str> {
    let mut lines = Vec::new();
    let mut line_start = 0;
    let mut last_byte_was_carriage_return = false;
    for (index, byte) in text.bytes().enumerate() {
        let mut ends_line = false;
        match byte {
            b'\n' => {
                if last_byte_was_carriage_return {
                    line_start = index + 1;
                } else {
                    ends_line = true;
                }
                last_byte_was_carriage_return = false;
            }
            b'\r' => {
                ends_line = true;
                last_byte_was_carriage_return = true;
            }
            _ => last_byte_was_carriage_return = false,
        }
        if ends_line {
            lines.push(&text[line_start..index]);
            line_start = index + 1;
        }
    }
    if line_start < text.len() {
        lines.push(&text[line_start..]);
    }
    lines
}

fn trim_whitespace(text: &str) -> &str {
    text.trim_matches([' ', '\n', '\t', '\x0b', '\x0c', '\r'])
}

/// The items of a `[a, b]` list in `line`. Records a failure if the list is not closed.
fn parse_list<'source>(line: &'source str, failed_message: &mut Option<String>) -> Vec<&'source str> {
    let Some(start) = line.find('[') else {
        return Vec::new();
    };
    let end = match line.rfind(']') {
        Some(end) if end > start => end,
        _ => {
            *failed_message = Some(format!("Can't parse list in '{line}'"));
            return Vec::new();
        }
    };
    line[start + 1..end]
        .split(',')
        .filter(|item| !item.is_empty())
        .map(trim_whitespace)
        .collect()
}

/// Everything after the first space in `line`. Records a failure if nothing follows a space.
fn second_word<'source>(line: &'source str, failed_message: &mut Option<String>) -> &'source str {
    match line.find(' ') {
        Some(separator) if separator + 1 < line.len() => &line[separator + 1..],
        _ => {
            *failed_message = Some(format!("Can't parse value after space in '{line}'"));
            ""
        }
    }
}

fn apply_flag(metadata: &mut TestMetadata<'_>, flag: &str) {
    match flag {
        "raw" => {
            metadata.strict_mode = StrictMode::NoStrict;
            metadata.harness_files.clear();
        }
        "noStrict" => metadata.strict_mode = StrictMode::NoStrict,
        "onlyStrict" => metadata.strict_mode = StrictMode::OnlyStrict,
        "module" => {
            assert!(
                metadata.strict_mode == StrictMode::Both,
                "the module flag follows a strictness flag"
            );
            metadata.program_type = ProgramType::Module;
            metadata.strict_mode = StrictMode::NoStrict;
        }
        "async" => {
            metadata.harness_files.push(ASYNC_INCLUDE);
            metadata.is_async = true;
        }
        // This should only skip the test if the agent can suspend, which it always can here, as in the C++ runner.
        "CanBlockIsFalse" => metadata.skip_test = true,
        _ => {}
    }
}

/// Reads the metadata block of a test, line by line exactly as the C++ runner does, so that both runners accept and
/// reject the same files.
fn extract_metadata(source: &str) -> Result<TestMetadata<'_>, String> {
    let mut metadata = TestMetadata::default();
    let mut failed_message = None;
    let mut parsing_negative = false;
    let mut include_list = Vec::new();
    let mut parsing_includes_list = false;
    let mut parsing_flags_block = false;
    let mut has_phase = false;

    for raw_line in lines(source) {
        if failed_message.is_some() {
            break;
        }

        if raw_line.starts_with("---*/") {
            if parsing_includes_list {
                metadata.harness_files.append(&mut include_list);
            }
            return Ok(metadata);
        }

        let line = trim_whitespace(raw_line);

        if parsing_includes_list {
            if line.starts_with('-') {
                include_list.push(second_word(line, &mut failed_message));
                continue;
            }
            if include_list.is_empty() {
                failed_message = Some("Supposed to parse a list but found no entries".to_string());
                break;
            }
            metadata.harness_files.append(&mut include_list);
            parsing_includes_list = false;
        }

        if parsing_negative {
            if line.starts_with("phase:") {
                let phase = second_word(line, &mut failed_message);
                has_phase = true;
                match phase {
                    "early" | "parse" => metadata.phase = NegativePhase::ParseOrEarly,
                    "resolution" => metadata.phase = NegativePhase::Resolution,
                    "runtime" => metadata.phase = NegativePhase::Runtime,
                    _ => {
                        failed_message = Some(format!("Unknown negative phase: {phase}"));
                        break;
                    }
                }
            } else if line.starts_with("type:") {
                metadata.negative_type = second_word(line, &mut failed_message);
            } else {
                if !has_phase {
                    failed_message = Some("Failed to find phase in negative attributes".to_string());
                    break;
                }
                if metadata.negative_type.is_empty() {
                    failed_message = Some("Failed to find type in negative attributes".to_string());
                    break;
                }
                parsing_negative = false;
            }
        }

        if parsing_flags_block {
            if line.starts_with('-') {
                let flag = second_word(line, &mut failed_message);
                apply_flag(&mut metadata, flag);
                continue;
            }
            parsing_flags_block = false;
        }

        if line.starts_with("flags:") {
            let flags = parse_list(line, &mut failed_message);
            if !flags.is_empty() {
                for flag in flags {
                    apply_flag(&mut metadata, flag);
                }
            } else if !line.contains('[') {
                parsing_flags_block = true;
            }
        } else if line.starts_with("includes:") {
            let files = parse_list(line, &mut failed_message);
            if files.is_empty() {
                parsing_includes_list = true;
            } else {
                metadata.harness_files.extend(files);
            }
        } else if line.starts_with("negative:") {
            metadata.is_negative = true;
            parsing_negative = true;
        }
    }

    Err(failed_message.unwrap_or_else(|| "Never reached end of comment '---*/'".to_string()))
}

/// A JSON value, serialized as AK's JsonValue serializes it.
enum JsonValue {
    Null,
    Bool(bool),
    Integer(i64),
    String(String),
    Object(JsonObject),
}

impl From<bool> for JsonValue {
    fn from(value: bool) -> Self {
        Self::Bool(value)
    }
}

impl From<i64> for JsonValue {
    fn from(value: i64) -> Self {
        Self::Integer(value)
    }
}

impl From<&str> for JsonValue {
    fn from(value: &str) -> Self {
        Self::String(value.to_string())
    }
}

impl From<String> for JsonValue {
    fn from(value: String) -> Self {
        Self::String(value)
    }
}

impl From<JsonObject> for JsonValue {
    fn from(value: JsonObject) -> Self {
        Self::Object(value)
    }
}

impl JsonValue {
    fn serialize_into(&self, output: &mut String) {
        match self {
            Self::Null => output.push_str("null"),
            Self::Bool(value) => output.push_str(if *value { "true" } else { "false" }),
            Self::Integer(value) => output.push_str(&value.to_string()),
            Self::String(value) => append_escaped_for_json(output, value),
            Self::Object(object) => object.serialize_into(output),
        }
    }
}

fn append_escaped_for_json(output: &mut String, text: &str) {
    output.push('"');
    for character in text.chars() {
        match character {
            '\u{8}' => output.push_str("\\b"),
            '\n' => output.push_str("\\n"),
            '\t' => output.push_str("\\t"),
            '"' => output.push_str("\\\""),
            '\\' => output.push_str("\\\\"),
            '\0'..='\u{1f}' => output.push_str(&format!("\\u{:04x}", u32::from(character))),
            _ => output.push(character),
        }
    }
    output.push('"');
}

/// A JSON object that keeps its keys in insertion order, like AK's JsonObject.
#[derive(Default)]
struct JsonObject {
    members: Vec<(&'static str, JsonValue)>,
}

impl JsonObject {
    fn set(&mut self, key: &'static str, value: impl Into<JsonValue>) {
        let value = value.into();
        if let Some(member) = self.members.iter_mut().find(|(name, _)| *name == key) {
            member.1 = value;
        } else {
            self.members.push((key, value));
        }
    }

    fn get(&self, key: &str) -> Option<&JsonValue> {
        self.members
            .iter()
            .find(|(name, _)| *name == key)
            .map(|(_, value)| value)
    }

    fn has(&self, key: &str) -> bool {
        self.get(key).is_some()
    }

    fn remove(&mut self, key: &str) {
        self.members.retain(|(name, _)| *name != key);
    }

    fn serialize_into(&self, output: &mut String) {
        output.push('{');
        for (index, (key, value)) in self.members.iter().enumerate() {
            if index != 0 {
                output.push(',');
            }
            append_escaped_for_json(output, key);
            output.push(':');
            value.serialize_into(output);
        }
        output.push('}');
    }

    fn serialized(&self) -> String {
        let mut output = String::new();
        self.serialize_into(&mut output);
        output
    }
}

/// Writes `text` to `fd` without taking ownership of it.
fn write_to_fd(fd: RawFd, text: &str) {
    // SAFETY: The caller owns the descriptor; ManuallyDrop keeps it open.
    let mut file = ManuallyDrop::new(unsafe { File::from_raw_fd(fd) });
    let _ = file.write_all(text.as_bytes());
    let _ = file.flush();
}

fn write_result(saved_stdout: RawFd, result: &JsonObject) {
    write_to_fd(saved_stdout, &format!("RESULT {}\0\n", result.serialized()));
}

/// The test that is running, which a panic reports as failed.
type CurrentTest = Arc<Mutex<String>>;

fn describe_panic(info: &std::panic::PanicHookInfo<'_>) -> String {
    let message = info
        .payload()
        .downcast_ref::<&str>()
        .copied()
        .or_else(|| info.payload().downcast_ref::<String>().map(String::as_str))
        .unwrap_or("non-string panic payload");
    match info.location() {
        Some(location) => format!("{location}: {message}"),
        None => message.to_string(),
    }
}

fn install_assertion_failure_hook(current_test: CurrentTest, saved_stdout: RawFd) {
    std::panic::set_hook(Box::new(move |info| {
        let mut result = JsonObject::default();
        let test = current_test.lock().unwrap_or_else(PoisonError::into_inner).clone();
        result.set("test", test);
        result.set("assert_fail", true);
        result.set("result", "assert_fail");
        result.set("output", describe_panic(info));
        write_result(saved_stdout, &result);
        let _ = std::io::stderr().flush();
        // SAFETY: Exits without unwinding or running destructors, from a state that may be broken.
        unsafe { libc::_exit(EXIT_ASSERTION_FAILED) }
    }));
}

struct HarnessFiles {
    directory: String,
    contents_by_name: HashMap<String, Vec<u8>>,
}

impl HarnessFiles {
    fn read(&mut self, harness_file: &str) -> Result<&[u8], TestError> {
        if !self.contents_by_name.contains_key(harness_file) {
            let path = format!("{}{harness_file}", self.directory);
            let contents = std::fs::read(&path).map_err(|_| TestError {
                phase: NegativePhase::Harness,
                error_type: "filesystem".to_string(),
                details: format!("Could not read file: {harness_file}"),
                harness_file: harness_file.to_string(),
            })?;
            self.contents_by_name.insert(harness_file.to_string(), contents);
        }
        Ok(&self.contents_by_name[harness_file])
    }
}

fn first_parser_error(errors: &[ParserError]) -> String {
    errors.first().map(ToString::to_string).unwrap_or_default()
}

/// The code units of a source that must be valid. Like AK's Utf16String::from_utf8() in the C++ runner, this stops the
/// process otherwise.
fn decoded_source<'source>(source: Option<&'source [u16]>, what: &str) -> &'source [u16] {
    source.unwrap_or_else(|| panic!("{what} is not valid UTF-8"))
}

fn parse_only_check(source: &[u16], program_type: ProgramType) -> Result<(), TestError> {
    if parse(source, program_type, 1).has_errors() {
        return Err(TestError::syntax_error(
            NegativePhase::ParseOrEarly,
            "Parse error".to_string(),
            "",
        ));
    }
    Ok(())
}

/// The C++ ScriptOrModuleProgram.
#[derive(Clone, Copy)]
enum ScriptOrModuleProgram {
    Script(Gc<Script>),
    Module(Gc<SourceTextModule>),
}

fn parse_program(
    vm: &Vm,
    realm: Gc<Realm>,
    source: &[u16],
    filepath: &str,
    program_type: ProgramType,
) -> Result<ScriptOrModuleProgram, TestError> {
    let syntax_error = |errors: Vec<ParserError>| {
        TestError::syntax_error(NegativePhase::ParseOrEarly, first_parser_error(&errors), "")
    };
    if program_type == ProgramType::Script {
        return Script::parse_with_filename(vm, source, realm, filepath)
            .map(ScriptOrModuleProgram::Script)
            .map_err(syntax_error);
    }
    let source_code = SourceCode::create(Utf16String::from_utf8(filepath), Utf16String::from_utf16(source));
    SourceTextModule::parse(vm, source_code, realm, filepath)
        .map(ScriptOrModuleProgram::Module)
        .map_err(syntax_error)
}

fn value_to_utf8_string_without_side_effects(value: Value) -> String {
    Utf16View::of_string(&value.to_utf16_string_without_side_effects()).to_utf8()
}

/// The runtime error a test threw: its name, from the error or else from its constructor, and its message.
fn describe_thrown_value(vm: &Vm, error_value: Value) -> TestError {
    let mut error = TestError {
        phase: NegativePhase::Runtime,
        error_type: String::new(),
        details: String::new(),
        harness_file: String::new(),
    };
    if error_value.is_object() {
        let object = error_value.as_object();
        let name = object.get_without_side_effects(vm, &vm.names.name);
        if !name.is_undefined() && !name.is_accessor() {
            error.error_type = value_to_utf8_string_without_side_effects(name);
        } else {
            let constructor_value = object.get_without_side_effects(vm, &vm.names.constructor);
            if constructor_value.is_object() {
                let name = constructor_value
                    .as_object()
                    .get_without_side_effects(vm, &vm.names.name);
                if !name.is_undefined() {
                    error.error_type = value_to_utf8_string_without_side_effects(name);
                }
            }
        }

        let message = object.get_without_side_effects(vm, &vm.names.message);
        if !message.is_undefined() && !message.is_accessor() {
            error.details = value_to_utf8_string_without_side_effects(message);
        }
    }
    if error.error_type.is_empty() {
        error.error_type = value_to_utf8_string_without_side_effects(error_value);
    }
    error
}

fn run_program(vm: &Vm, program: ScriptOrModuleProgram) -> Result<(), TestError> {
    let result = match program {
        ScriptOrModuleProgram::Script(script) => vm.run_script(script, None),
        ScriptOrModuleProgram::Module(module) => vm.run_module(module),
    };
    result
        .map(|_| ())
        .map_err(|throw| describe_thrown_value(vm, throw.value()))
}

/// A VM whose SharedArrayBuffers agents can map, as broadcasting one hands its shared memory to them.
fn create_vm() -> Box<Vm> {
    Vm::create_with(VmOptions {
        shared_memory_shared_array_buffers: true,
        ..VmOptions::default()
    })
}

fn run_test(
    source: Option<&[u16]>,
    filepath: &str,
    metadata: &TestMetadata<'_>,
    parse_only: bool,
    harness_files: &mut HarnessFiles,
) -> Result<(), TestError> {
    if parse_only {
        return parse_only_check(decoded_source(source, "The test"), metadata.program_type);
    }

    let vm = create_vm();
    vm.set_dynamic_imports_allowed(true);
    let root_execution_context =
        initialize_realm_with_global_object(&vm, &|realm| Test262GlobalObject::allocate(&vm, realm).upcast());
    let realm = root_execution_context.realm();
    let program = parse_program(
        &vm,
        realm,
        decoded_source(source, "The test"),
        filepath,
        metadata.program_type,
    )?;

    let mut harness_source = Vec::new();
    for harness_file in &metadata.harness_files {
        harness_source.extend_from_slice(harness_files.read(harness_file)?);
        harness_source.push(b'\n');
    }

    if !harness_source.is_empty() {
        let harness_source = utf16_from_wtf8(&harness_source);
        let harness_program = Script::parse_with_filename(
            &vm,
            decoded_source(harness_source.as_deref(), "The harness"),
            realm,
            "<harness>",
        )
        .map_err(|errors| TestError::syntax_error(NegativePhase::Harness, first_parser_error(&errors), "<harness>"))?;
        if let Err(error) = run_program(&vm, ScriptOrModuleProgram::Script(harness_program)) {
            return Err(TestError {
                phase: NegativePhase::Harness,
                error_type: error.error_type,
                details: error.details,
                harness_file: "<harness>".to_string(),
            });
        }
    }

    run_program(&vm, program)
}

fn error_to_json(error: &TestError) -> JsonObject {
    let mut object = JsonObject::default();
    object.set("phase", error.phase.name());
    object.set("type", error.error_type.as_str());
    object.set("details", error.details.as_str());
    object
}

/// Whether an error is the InternalError of a feature that the runtime does not implement yet.
fn is_unimplemented_feature_error(error_type: &str, details: &str) -> bool {
    error_type == "InternalError" && details.starts_with("TODO(")
}

#[cfg(unix)]
fn is_unimplemented_feature_failure(failure: &AgentFailure) -> bool {
    match failure {
        AgentFailure::UncaughtException { name, message } => is_unimplemented_feature_error(name, message),
        AgentFailure::Panic(_) | AgentFailure::Exited(_) => false,
    }
}

fn verify_test(
    result: &Result<(), TestError>,
    metadata: &TestMetadata<'_>,
    output: &mut JsonObject,
    parse_only: bool,
) -> bool {
    if let Err(error) = result {
        if error.phase == NegativePhase::Harness {
            output.set("harness_error", true);
            output.set(
                "harness_file",
                string_from_utf8_with_replacement_character(error.harness_file.as_bytes()),
            );
            output.set("result", "harness_error");
        } else if error.phase == NegativePhase::Runtime
            && (is_unimplemented_feature_error(&error.error_type, &error.details)
                || (error.error_type == "Test262Error" && error.details.ends_with(" but got a InternalError")))
        {
            output.set("todo_error", true);
            output.set("result", "todo_error");
        }
    }

    if metadata.is_async
        && let Some(JsonValue::String(messages)) = output.get("output")
        && messages.contains("AsyncTestFailure:InternalError: TODO(")
    {
        output.set("todo_error", true);
        output.set("result", "todo_error");
    }

    let mut expected_error = JsonValue::Null;
    let mut got_error = JsonValue::Null;
    let passed = 'verdict: {
        if !metadata.is_negative {
            match result {
                Ok(()) => break 'verdict true,
                Err(error) => {
                    got_error = error_to_json(error).into();
                    break 'verdict false;
                }
            }
        }

        let mut expected_error_object = JsonObject::default();
        expected_error_object.set("phase", metadata.phase.name());
        expected_error_object.set("type", metadata.negative_type);
        expected_error = expected_error_object.into();

        let error = match result {
            Ok(()) => {
                // A parse-only run never reaches the phase a non-parse error is expected in.
                break 'verdict parse_only && metadata.phase != NegativePhase::ParseOrEarly;
            }
            Err(error) => error,
        };

        got_error = error_to_json(error).into();

        // The phase of a module's SyntaxError is hard to track through linking and evaluation, so any SyntaxError
        // counts. Every test whose module must not evaluate starts with $DONOTEVALUATE().
        if metadata.program_type == ProgramType::Module && metadata.negative_type == "SyntaxError" {
            break 'verdict error.error_type == metadata.negative_type;
        }
        error.phase == metadata.phase && error.error_type == metadata.negative_type
    };

    let mut error_object = JsonObject::default();
    error_object.set("expected", expected_error);
    error_object.set("got", got_error);
    output.set("error", error_object);
    passed
}

struct Options {
    harness_directory: String,
    parse_only: bool,
    timeout_in_seconds: i64,
    disable_core_dumping: bool,
    run_as_agent: bool,
}

fn parse_options(arguments: &[String]) -> Result<Options, String> {
    let mut options = Options {
        harness_directory: String::new(),
        parse_only: false,
        timeout_in_seconds: 10,
        disable_core_dumping: false,
        run_as_agent: false,
    };
    let mut arguments = arguments.iter().skip(1);
    while let Some(argument) = arguments.next() {
        let (name, inline_value) = match argument.split_once('=') {
            Some((name, value)) if name.starts_with("--") => (name, Some(value.to_string())),
            _ => (argument.as_str(), None),
        };
        let mut value = |name: &str| {
            inline_value
                .clone()
                .or_else(|| arguments.next().cloned())
                .ok_or(format!("{name} needs a value"))
        };
        match name {
            "-l" | "--harness-location" => options.harness_directory = value(name)?,
            "-p" | "--parse-only" => options.parse_only = true,
            "-t" | "--timeout" => {
                let text = value(name)?;
                options.timeout_in_seconds = text.parse().map_err(|_| format!("Invalid value for {name}: {text}"))?;
            }
            // The C++ runner enables its debug logging with this; the Rust runtime has none, so it is accepted and
            // does nothing.
            "-d" | "--debug" => {}
            "--disable-core-dump" => options.disable_core_dumping = true,
            "--agent" => options.run_as_agent = true,
            _ => return Err(format!("Unknown option {argument}")),
        }
    }
    Ok(options)
}

/// Like the C++ runner, this leaves core dumps alone on macOS.
fn disable_core_dumps() -> Result<(), std::io::Error> {
    if cfg!(target_os = "macos") {
        return Ok(());
    }
    let mut limits = libc::rlimit {
        rlim_cur: 0,
        rlim_max: 0,
    };
    // SAFETY: The struct is valid for both calls.
    unsafe {
        if libc::getrlimit(libc::RLIMIT_CORE, &raw mut limits) != 0 {
            return Err(std::io::Error::last_os_error());
        }
        limits.rlim_cur = 0;
        if libc::setrlimit(libc::RLIMIT_CORE, &raw const limits) != 0 {
            return Err(std::io::Error::last_os_error());
        }
    }
    Ok(())
}

/// The harness directory for a test path, which is `harness/` next to the `test/` directory the test is in.
fn harness_directory_for_test(test_path: &str) -> Option<String> {
    test_path
        .find("test/")
        .map(|index| format!("{}harness/", &test_path[..index]))
}

fn report_os_error(operation: &str) {
    eprintln!("{operation}: {}", std::io::Error::last_os_error());
}

/// The stream tests print into: standard output is redirected into a non-blocking pipe, and results go to a duplicate
/// of the original standard output.
struct CapturedStandardOutput {
    saved_stdout: RawFd,
    pipe_read_end: RawFd,
}

impl CapturedStandardOutput {
    fn redirect() -> Result<Self, &'static str> {
        // SAFETY: These calls only create and rearrange this process's descriptors.
        unsafe {
            // NB: The descriptors close on exec, so that agents do not hold the pipes of the runner's driver open.
            let saved_stdout = libc::fcntl(libc::STDOUT_FILENO, libc::F_DUPFD_CLOEXEC, 0);
            if saved_stdout < 0 {
                return Err("dup");
            }
            let mut pipe_fds = [0; 2];
            if libc::pipe(pipe_fds.as_mut_ptr()) < 0 {
                return Err("pipe");
            }
            for fd in pipe_fds {
                let flags = libc::fcntl(fd, libc::F_GETFL);
                libc::fcntl(fd, libc::F_SETFL, flags | libc::O_NONBLOCK);
                libc::fcntl(fd, libc::F_SETFD, libc::FD_CLOEXEC);
            }
            if libc::dup2(pipe_fds[1], libc::STDOUT_FILENO) < 0 {
                return Err("dup2");
            }
            if libc::close(pipe_fds[1]) < 0 {
                return Err("close");
            }
            Ok(Self {
                saved_stdout,
                pipe_read_end: pipe_fds[0],
            })
        }
    }

    /// The first bytes printed since the last call, if anything was printed. The rest is discarded.
    fn collect_output(&self) -> Option<Vec<u8>> {
        crate::standard_output::flush();
        let mut buffer = [0u8; COLLECTED_OUTPUT_LIMIT];
        let read = |buffer: &mut [u8; COLLECTED_OUTPUT_LIMIT]| {
            // SAFETY: The buffer is valid for its length.
            unsafe { libc::read(self.pipe_read_end, buffer.as_mut_ptr().cast(), buffer.len()) }
        };
        let byte_count = read(&mut buffer);
        let byte_count = usize::try_from(byte_count).ok().filter(|count| *count > 0)?;
        let output = buffer[..byte_count].to_vec();
        while read(&mut buffer) > 0 {}
        Some(output)
    }

    /// Puts standard output back. Results have all been written by now, so failures are ignored, like in the C++
    /// runner.
    fn restore(self) {
        // SAFETY: Both descriptors belong to this struct and are not used afterwards.
        unsafe {
            if libc::dup2(self.saved_stdout, libc::STDOUT_FILENO) < 0 {
                report_os_error("dup2");
                return;
            }
            if libc::close(self.saved_stdout) < 0 {
                report_os_error("fclose");
                return;
            }
            if libc::close(self.pipe_read_end) < 0 {
                report_os_error("close");
            }
        }
    }
}

fn set_alarm(seconds: u32) {
    // SAFETY: alarm has no preconditions.
    unsafe { libc::alarm(seconds) };
}

fn run(options: &Options) -> c_int {
    if options.disable_core_dumping
        && let Err(error) = disable_core_dumps()
    {
        eprintln!("Failed to disable core dumps: {error}");
        return EXIT_WRONG_ARGUMENTS;
    }

    let mut harness_directory = options.harness_directory.clone();
    let mut detect_harness_directory = harness_directory.is_empty();
    if !detect_harness_directory && !harness_directory.ends_with('/') {
        harness_directory.push('/');
    }

    let Ok(timeout_in_seconds) = u32::try_from(options.timeout_in_seconds) else {
        eprintln!("timeout must be at least 1");
        return EXIT_WRONG_ARGUMENTS;
    };
    if timeout_in_seconds == 0 {
        eprintln!("timeout must be at least 1");
        return EXIT_WRONG_ARGUMENTS;
    }

    let captured_output = match CapturedStandardOutput::redirect() {
        Ok(captured_output) => captured_output,
        Err(operation) => {
            report_os_error(operation);
            return EXIT_STDOUT_SETUP_FAILED;
        }
    };
    let saved_stdout = captured_output.saved_stdout;
    let current_test = CurrentTest::default();
    install_assertion_failure_hook(current_test.clone(), saved_stdout);

    #[cfg(unix)]
    if let Ok(program) = std::env::current_exe() {
        agents::enable_agents(program, vec!["--agent".into()]);
    }

    let mut harness_files = HarnessFiles {
        directory: harness_directory,
        contents_by_name: HashMap::new(),
    };
    let mut count = 0usize;
    let standard_input = std::io::stdin();
    for line in standard_input.lock().split(b'\n') {
        let Ok(line) = line else {
            return EXIT_SETUP_INPUT_FAILURE;
        };
        if line.is_empty() {
            continue;
        }
        let path = String::from_utf8_lossy(&line).into_owned();
        current_test
            .lock()
            .unwrap_or_else(PoisonError::into_inner)
            .clone_from(&path);

        if detect_harness_directory {
            let Some(directory) = harness_directory_for_test(&path) else {
                eprintln!("Attempted to find harness directory from test file '{path}', but did not find 'test/'");
                return EXIT_READ_FILE_FAILURE;
            };
            harness_files.directory = directory;
            detect_harness_directory = false;
        }

        let Ok(contents) = std::fs::read(&path) else {
            eprintln!("Could not open file: {path}");
            return EXIT_READ_FILE_FAILURE;
        };
        count += 1;

        let source = TestSource::decode(&contents);

        let mut result_object = JsonObject::default();
        result_object.set("test", path.as_str());
        run_test_file(
            &source,
            &path,
            options.parse_only,
            timeout_in_seconds,
            &mut harness_files,
            &captured_output,
            &mut result_object,
        );
        write_result(saved_stdout, &result_object);
    }

    current_test.lock().unwrap_or_else(PoisonError::into_inner).clear();
    write_to_fd(saved_stdout, &format!("DONE {count}\n"));
    captured_output.restore();
    0
}

/// --agent: the runner as one of the agents that a test starts.
#[cfg(unix)]
mod agent_mode {
    use core::cell::RefCell;
    use core::ffi::c_int;

    use super::{
        EXIT_ASSERTION_FAILED, EXIT_SETUP_INPUT_FAILURE, NegativePhase, ScriptOrModuleProgram, TestError, create_vm,
        decoded_source, describe_panic, describe_thrown_value, first_parser_error, run_program,
    };
    use crate::contrib::test262::agents;
    use crate::contrib::test262::global_object::Test262GlobalObject;
    use crate::interpreter::vm::Vm;
    use crate::layout::cell::Gc;
    use crate::layout::realm::Realm;
    use crate::runtime::promise::Promise;
    use crate::script::Script;
    use crate::utf16::utf16_from_wtf8;
    use crate::utilities::initialize_realm_with_global_object;

    thread_local! {
        /// The promises that are rejected without a handler, by address, with what they were rejected with.
        static UNHANDLED_REJECTIONS: RefCell<Vec<(usize, TestError)>> = const { RefCell::new(Vec::new()) };
    }

    fn remember_unhandled_rejection(vm: &Vm, promise: Gc<Promise>) {
        let error = describe_thrown_value(vm, promise.result());
        UNHANDLED_REJECTIONS.with_borrow_mut(|rejections| rejections.push((promise.as_ptr().addr(), error)));
    }

    fn forget_handled_rejection(_vm: &Vm, promise: Gc<Promise>) {
        let address = promise.as_ptr().addr();
        UNHANDLED_REJECTIONS.with_borrow_mut(|rejections| rejections.retain(|(rejected, _)| *rejected != address));
    }

    fn run_agent_script(vm: &Vm, realm: Gc<Realm>, source: &[u8]) -> Result<(), TestError> {
        let source = utf16_from_wtf8(source);
        let script = Script::parse_with_filename(vm, decoded_source(source.as_deref(), "The agent"), realm, "<agent>")
            .map_err(|errors| TestError::syntax_error(NegativePhase::ParseOrEarly, first_parser_error(&errors), ""))?;
        run_program(vm, ScriptOrModuleProgram::Script(script))?;
        match UNHANDLED_REJECTIONS.take().into_iter().next() {
            Some((_, error)) => Err(error),
            None => Ok(()),
        }
    }

    /// Runs the script of one agent of a test, and tells the test if it failed.
    pub(super) fn run_agent() -> c_int {
        let source = match agents::connect_to_test() {
            Ok(source) => source,
            Err(error) => {
                eprintln!("test262-runner-rust: an agent could not connect to its test: {error}");
                return EXIT_SETUP_INPUT_FAILURE;
            }
        };
        std::panic::set_hook(Box::new(|info| {
            agents::report_panic(&describe_panic(info));
            // SAFETY: Exits without unwinding or running destructors, from a state that may be broken.
            unsafe { libc::_exit(EXIT_ASSERTION_FAILED) }
        }));

        let vm = create_vm();
        vm.set_on_promise_unhandled_rejection(Some(remember_unhandled_rejection));
        vm.set_on_promise_rejection_handled(Some(forget_handled_rejection));
        let root_execution_context =
            initialize_realm_with_global_object(&vm, &|realm| Test262GlobalObject::allocate(&vm, realm).upcast());
        agents::acknowledge_start();

        if let Err(error) = run_agent_script(&vm, root_execution_context.realm(), &source) {
            agents::report_uncaught_exception(&error.error_type, &error.details);
        }
        // SAFETY: Ends the agent without tearing down its VM, which nothing needs any more, while the thread that
        //         listens to the test still waits.
        unsafe { libc::_exit(0) }
    }
}

/// A test file's text, decoded once for reading its metadata and once for parsing it as C++ does.
struct TestSource {
    text: String,
    is_valid_utf8: bool,
    code_units: Option<Vec<u16>>,
    code_units_with_strict: Option<Vec<u16>>,
}

impl TestSource {
    fn decode(contents: &[u8]) -> Self {
        let code_units = utf16_from_wtf8(contents);
        let code_units_with_strict = code_units.as_ref().map(|code_units| {
            let mut with_strict: Vec<u16> = USE_STRICT_PREFIX.encode_utf16().collect();
            with_strict.extend_from_slice(code_units);
            with_strict
        });
        Self {
            text: String::from_utf8_lossy(contents).into_owned(),
            is_valid_utf8: core::str::from_utf8(contents).is_ok(),
            code_units,
            code_units_with_strict,
        }
    }
}

fn run_test_file(
    source: &TestSource,
    path: &str,
    parse_only: bool,
    timeout_in_seconds: u32,
    harness_files: &mut HarnessFiles,
    captured_output: &CapturedStandardOutput,
    result_object: &mut JsonObject,
) {
    let metadata = match extract_metadata(&source.text) {
        Ok(metadata) => metadata,
        Err(message) => {
            // The C++ runner cannot even format a message that holds an invalid byte sequence.
            assert!(
                source.is_valid_utf8 || !message.contains('\u{FFFD}'),
                "The metadata of the test is not valid UTF-8"
            );
            result_object.set("result", "metadata_error");
            result_object.set("metadata_error", true);
            result_object.set("metadata_output", message);
            return;
        }
    };
    if metadata.skip_test {
        result_object.set("result", "skipped");
        return;
    }

    let mut run_with_strict_mode = |strict_mode: bool, result_object: &mut JsonObject| -> bool {
        result_object.set("strict_mode", strict_mode);

        let start = Instant::now();
        set_alarm(timeout_in_seconds);
        let code_units = if strict_mode {
            &source.code_units_with_strict
        } else {
            &source.code_units
        };
        let result = run_test(code_units.as_deref(), path, &metadata, parse_only, harness_files);
        #[cfg(unix)]
        let an_agent_reached_an_unimplemented_feature =
            agents::terminate_agents().iter().any(is_unimplemented_feature_failure);
        set_alarm(0);
        let elapsed_milliseconds = i64::try_from(start.elapsed().as_millis()).unwrap_or(i64::MAX);

        let (output_key, duration_key) = if strict_mode {
            ("strict_output", "strict_duration_ms")
        } else {
            ("output", "duration_ms")
        };
        result_object.set(duration_key, elapsed_milliseconds);

        let first_output = captured_output.collect_output();
        if let Some(output) = &first_output {
            result_object.set(output_key, string_from_utf8_with_replacement_character(output));
        }

        let mut passed = verify_test(&result, &metadata, result_object, parse_only);
        if metadata.is_async && !parse_only {
            let output = first_output.as_deref().unwrap_or_default();
            let output_contains = |needle: &[u8]| output.windows(needle.len()).any(|window| window == needle);
            if !output_contains(b"Test262:AsyncTestComplete") || output_contains(b"Test262:AsyncTestFailure") {
                result_object.set("async_fail", true);
                if first_output.is_none() {
                    result_object.set(output_key, JsonValue::Null);
                }
                passed = false;
            }
        }
        // A test can fail before it asks for the report that would have brought it the error of an agent.
        #[cfg(unix)]
        if !passed && an_agent_reached_an_unimplemented_feature {
            result_object.set("todo_error", true);
            result_object.set("result", "todo_error");
        }
        passed
    };

    let mut passed = true;
    if metadata.strict_mode != StrictMode::OnlyStrict {
        passed = run_with_strict_mode(false, result_object);
    }
    if passed && metadata.strict_mode != StrictMode::NoStrict {
        passed = run_with_strict_mode(true, result_object);
    }

    if passed {
        result_object.remove("strict_mode");
    }
    if !result_object.has("result") {
        result_object.set("result", if passed { "passed" } else { "failed" });
    }
}

/// The entry point of test262-runner-rust, called from its C++ main.
///
/// # Safety
///
/// `argv` must hold `argc` NUL-terminated strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn libjs_runtime_rust_test262_runner_main(argc: c_int, argv: *const *const c_char) -> c_int {
    let arguments: Vec<String> = (0..usize::try_from(argc).unwrap_or(0))
        // SAFETY: The caller passes argc valid strings.
        .map(|index| {
            unsafe { CStr::from_ptr(*argv.add(index)) }
                .to_string_lossy()
                .into_owned()
        })
        .collect();
    match parse_options(&arguments) {
        #[cfg(unix)]
        Ok(options) if options.run_as_agent => agent_mode::run_agent(),
        Ok(options) => run(&options),
        Err(error) => {
            eprintln!("test262-runner-rust: {error}");
            1
        }
    }
}
