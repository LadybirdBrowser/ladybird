/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Runs the spec testsuite's scripts (the .json files wast2json makes, with their modules next to them) against two
//! stores, one where every call into a function the JIT compiled runs the compiled code, and one where the
//! interpreter runs everything, and compares results, traps, memories and globals after every call.

use super::{CompiledModule, Outcome, TestBinding, function_index};
use libwasm_engine::runtime::{ExternValue, InstanceAddress, Store, Trap, Value};
use libwasm_engine::types::{FuncType, GlobalType, Limits, MemoryType, RefType, TableType, ValType};
use libwasm_jit::Options;
use serde_json::Value as Json;
use std::collections::{BTreeSet, HashMap};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::rc::Rc;
use std::sync::Arc;

#[derive(Default)]
pub struct Totals {
    pub scripts: usize,
    pub modules: usize,
    pub functions: usize,
    pub compiled: usize,
    pub runnable: usize,
    pub exercised: usize,
    pub cases: usize,
    pub failures: usize,
    /// Calls that reached a function the JIT did not compile, after which the module is not compared any more.
    pub not_compiled: usize,
}

type Exports = HashMap<String, ExternValue>;

struct LoadedModule {
    name: Option<String>,
    interpreted: InstanceAddress,
    jit: InstanceAddress,
    /// The module compiled by the tests, unless the runtime compiles it.
    compiled: Option<(CompiledModule, TestBinding)>,
    diverged: bool,
}

pub fn outcome(result: Result<Vec<Value>, Trap>) -> Outcome {
    match result {
        Ok(values) => Outcome::Values(values),
        Err(trap) => Outcome::Trap(trap.code()),
    }
}

pub fn same_value(a: &Value, b: &Value) -> bool {
    match (a, b) {
        (Value::F32(x), Value::F32(y)) => x == y || (f32::from_bits(*x).is_nan() && f32::from_bits(*y).is_nan()),
        (Value::F64(x), Value::F64(y)) => x == y || (f64::from_bits(*x).is_nan() && f64::from_bits(*y).is_nan()),
        // NB: References are addresses, which differ between the stores, but i31 references are their values.
        (Value::Ref(x), Value::Ref(y)) if x & 1 != 0 || y & 1 != 0 => x == y,
        (Value::Ref(x), Value::Ref(y)) => (*x == 0) == (*y == 0),
        _ => a == b,
    }
}

pub fn same_outcome(a: &Outcome, b: &Outcome) -> bool {
    match (a, b) {
        (Outcome::Values(a), Outcome::Values(b)) => {
            a.len() == b.len() && a.iter().zip(b).all(|(a, b)| same_value(a, b))
        }
        (Outcome::Trap(a), Outcome::Trap(b)) => a == b,
        _ => false,
    }
}

/// How the memories and globals of two instances of a module differ, if they do.
pub fn state_difference(
    interpreted_store: &Store,
    interpreted: InstanceAddress,
    jit_store: &Store,
    jit: InstanceAddress,
) -> Option<String> {
    let (a, b) = (interpreted_store.instance(interpreted), jit_store.instance(jit));
    for (x, y) in a.memories.iter().zip(&b.memories) {
        let (x, y) = (interpreted_store.memory(*x).bytes(), jit_store.memory(*y).bytes());
        if x != y {
            let position = x
                .iter()
                .zip(y)
                .position(|(p, q)| p != q)
                .unwrap_or(x.len().min(y.len()));
            return Some(format!("memories differ at {position}"));
        }
    }
    for (position, (x, y)) in a.tables.iter().zip(&b.tables).enumerate() {
        let (x, y) = (&interpreted_store.table(*x).elements, &jit_store.table(*y).elements);
        // NB: Elements are addresses too, so only whether they are null can be compared.
        if x.len() != y.len() || x.iter().zip(y).any(|(p, q)| (*p == 0) != (*q == 0)) {
            return Some(format!("table {position} differs"));
        }
    }
    for (position, (x, y)) in a.globals.iter().zip(&b.globals).enumerate() {
        let (x, y) = (interpreted_store.global(*x).get(), jit_store.global(*y).get());
        // NB: References are addresses, which differ between the stores.
        if !matches!(x, Value::Ref(_)) && !same_value(&x, &y) {
            return Some(format!("global {position}: interpreter {x:?}, JIT {y:?}"));
        }
    }
    None
}

/// An argument of a scripted call, if the JIT handles its type.
fn argument(json: &Json) -> Option<Value> {
    if json.get("type")?.as_str()? == "v128" {
        return vector_argument(json);
    }
    let value = json.get("value")?.as_str()?;
    Some(match json.get("type")?.as_str()? {
        "i32" => Value::I32(value.parse::<u32>().ok()? as i32),
        "i64" => Value::I64(value.parse::<u64>().ok()? as i64),
        "f32" => Value::F32(value.parse::<u32>().ok()?),
        "f64" => Value::F64(value.parse::<u64>().ok()?),
        _ => return None,
    })
}

/// A v128 argument of a scripted call: its lanes, as `lane_type` says.
fn vector_argument(json: &Json) -> Option<Value> {
    let lanes = json.get("value")?.as_array()?;
    let bits = match json.get("lane_type")?.as_str()? {
        "i8" => 8,
        "i16" => 16,
        "i32" | "f32" => 32,
        "i64" | "f64" => 64,
        _ => return None,
    };
    let mut vector = 0u128;
    for (index, lane) in lanes.iter().enumerate() {
        // NB: Some scripts write lanes as signed numbers.
        let lane = lane.as_str()?;
        let lane = lane
            .parse::<u64>()
            .ok()
            .or_else(|| lane.parse::<i64>().ok().map(|lane| lane as u64))?;
        let mask = if bits == 64 { u64::MAX } else { (1 << bits) - 1 };
        vector |= u128::from(lane & mask) << (index * bits);
    }
    Some(Value::V128(vector))
}

/// The spectest module's instance in `store`.
fn spectest(store: &mut Store) -> HashMap<String, ExternValue> {
    let mut exports = HashMap::new();
    let print = |params: Vec<ValType>| FuncType {
        params,
        results: Vec::new(),
    };
    let nothing: libwasm_engine::runtime::HostCallback = Rc::new(|_: &mut Store, _: &[Value]| Ok(Vec::new()));
    for (name, params) in [
        ("print", vec![]),
        ("print_i32", vec![ValType::I32]),
        ("print_i64", vec![ValType::I64]),
        ("print_f32", vec![ValType::F32]),
        ("print_f64", vec![ValType::F64]),
        ("print_i32_f32", vec![ValType::I32, ValType::F32]),
        ("print_f64_f64", vec![ValType::F64, ValType::F64]),
    ] {
        let function = store.allocate_host_function(print(params), nothing.clone());
        exports.insert(name.to_string(), ExternValue::Function(function));
    }
    let global = |value, initial| (GlobalType { value, mutable: false }, initial);
    for (name, (ty, initial)) in [
        ("global_i32", global(ValType::I32, Value::I32(666))),
        ("global_i64", global(ValType::I64, Value::I64(666))),
        ("global_f32", global(ValType::F32, Value::F32(666.6f32.to_bits()))),
        ("global_f64", global(ValType::F64, Value::F64(666.6f64.to_bits()))),
    ] {
        let address = store.allocate_global(ty, initial);
        exports.insert(name.to_string(), ExternValue::Global(address));
    }
    let limits = |min, max| Limits {
        min,
        max: Some(max),
        address_type: libwasm_engine::types::AddressType::I32,
        shared: false,
    };
    if let Some(table) = store.allocate_table(
        TableType {
            element: RefType::FUNCREF,
            limits: limits(10, 20),
        },
        0,
    ) {
        exports.insert("table".to_string(), ExternValue::Table(table));
    }
    if let Some(memory) = store.allocate_memory(MemoryType { limits: limits(1, 2) }) {
        exports.insert("memory".to_string(), ExternValue::Memory(memory));
    }
    exports
}

struct Script<'a> {
    directory: PathBuf,
    interpreted: Store,
    jit: Store,
    /// The exports of the registered modules, in both stores.
    registered: HashMap<String, (Exports, Exports)>,
    modules: Vec<LoadedModule>,
    current: Option<usize>,
    totals: &'a mut Totals,
    verbose: bool,
    name: String,
    options: Options,
    runtime: bool,
    /// Where `--dump` writes what the interpreter did in every action.
    dump: Option<&'a mut dyn std::io::Write>,
}

/// A value as `--dump` writes it: the same on every architecture, so NaNs of scalar types and the addresses of
/// references are left out.
fn dump_value(value: &Value) -> String {
    match value {
        Value::F32(bits) if f32::from_bits(*bits).is_nan() => "f32:nan".to_string(),
        Value::F64(bits) if f64::from_bits(*bits).is_nan() => "f64:nan".to_string(),
        Value::Ref(0) => "ref:null".to_string(),
        Value::Ref(bits) if bits & 1 != 0 => format!("ref:i31:{bits:x}"),
        Value::Ref(_) => "ref:nonnull".to_string(),
        other => format!("{other:?}"),
    }
}

/// The memories, tables and globals of an instance, as `--dump` writes them: a hash of the memories' bytes, which
/// table elements are null, and the globals.
fn dump_state(store: &Store, instance: InstanceAddress) -> String {
    let instance = store.instance(instance);
    let mut text = String::new();
    for memory in &instance.memories {
        let bytes = store.memory(*memory).bytes();
        let hash = bytes.iter().fold(0xcbf2_9ce4_8422_2325u64, |hash, byte| {
            (hash ^ u64::from(*byte)).wrapping_mul(0x0100_0000_01b3)
        });
        text.push_str(&format!(" m{}:{hash:x}", bytes.len()));
    }
    for table in &instance.tables {
        let elements = &store.table(*table).elements;
        let hash = elements.iter().fold(0xcbf2_9ce4_8422_2325u64, |hash, element| {
            (hash ^ u64::from(*element == 0)).wrapping_mul(0x0100_0000_01b3)
        });
        text.push_str(&format!(" t{}:{hash:x}", elements.len()));
    }
    for global in &instance.globals {
        text.push_str(&format!(" g:{}", dump_value(&store.global(*global).get())));
    }
    text
}

impl Script<'_> {
    fn exports(store: &Store, instance: InstanceAddress) -> HashMap<String, ExternValue> {
        store.instance(instance).exports.clone()
    }

    fn load(&mut self, name: Option<String>, filename: &str) {
        self.current = None;
        let Ok(bytes) = std::fs::read(self.directory.join(filename)) else {
            return;
        };
        let Ok(module) = libwasm_engine::compile(&bytes) else {
            return;
        };
        let module = Arc::new(module);
        let mut interpreted_imports = Vec::new();
        let mut jit_imports = Vec::new();
        for import in &module.imports {
            let Some((interpreted, jit)) = self.registered.get(&import.module) else {
                return;
            };
            let (Some(interpreted), Some(jit)) = (interpreted.get(&import.name), jit.get(&import.name)) else {
                return;
            };
            interpreted_imports.push(*interpreted);
            jit_imports.push(*jit);
        }
        let Ok(interpreted) = self.interpreted.instantiate(module.clone(), &interpreted_imports) else {
            return;
        };
        let Ok(jit) = self.jit.instantiate(module.clone(), &jit_imports) else {
            return;
        };
        let defined = module.code.len();
        self.totals.modules += 1;
        self.totals.functions += defined;
        let compiled = if self.runtime {
            None
        } else {
            let compiled = CompiledModule::compile(module.clone(), self.options);
            let binding = TestBinding::new(&compiled, &self.jit, jit);
            let first = module.imported_function_count as usize;
            self.totals.compiled += compiled.entries[first..].iter().filter(|entry| **entry != 0).count();
            self.totals.runnable += compiled.runnable[first..].iter().filter(|runnable| **runnable).count();
            Some((compiled, binding))
        };
        self.modules.push(LoadedModule {
            name,
            interpreted,
            jit,
            compiled,
            diverged: false,
        });
        self.current = Some(self.modules.len() - 1);
    }

    fn module_index(&self, name: Option<&str>) -> Option<usize> {
        match name {
            Some(name) => self
                .modules
                .iter()
                .rposition(|module| module.name.as_deref() == Some(name)),
            None => self.current,
        }
    }

    /// Runs an action in both stores and compares what happened.
    fn act(&mut self, action: &Json, line: u64, exercised: &mut BTreeSet<(usize, u32)>) {
        let Some(index) = self.module_index(action.get("module").and_then(|module| module.as_str())) else {
            return;
        };
        let Some(field) = action.get("field").and_then(|field| field.as_str()) else {
            return;
        };
        let loaded = &self.modules[index];
        if loaded.diverged {
            return;
        }
        let interpreted_exports = Self::exports(&self.interpreted, loaded.interpreted);
        let jit_exports = Self::exports(&self.jit, loaded.jit);
        match action.get("type").and_then(|kind| kind.as_str()) {
            Some("invoke") => {
                let (Some(ExternValue::Function(interpreted)), Some(ExternValue::Function(jit))) =
                    (interpreted_exports.get(field), jit_exports.get(field))
                else {
                    return;
                };
                let Some(arguments) = action
                    .get("args")
                    .and_then(|arguments| arguments.as_array())
                    .and_then(|arguments| arguments.iter().map(argument).collect::<Option<Vec<_>>>())
                else {
                    return;
                };
                let expected = outcome(self.interpreted.invoke(*interpreted, &arguments));
                if let Some(dump) = self.dump.as_mut() {
                    let result = match &expected {
                        Outcome::Values(values) => values.iter().map(dump_value).collect::<Vec<_>>().join(" "),
                        Outcome::Trap(code) => format!("trap {code}"),
                    };
                    // NB: How deep the stack gets before it is exhausted differs, and so does what the code did on the way.
                    let state = if expected == Outcome::Trap(libwasm_engine::abi::TRAP_STACK_EXHAUSTED) {
                        String::new()
                    } else {
                        dump_state(&self.interpreted, self.modules[index].interpreted)
                    };
                    let _ = writeln!(dump, "{}:{line} {field:?} => {result} |{state}", self.name);
                }
                let loaded = &mut self.modules[index];
                let function = function_index(&self.jit, *jit, loaded.jit);
                let actual = match (function, &mut loaded.compiled) {
                    (Some(function), None) => {
                        exercised.insert((index, function));
                        self.totals.cases += 1;
                        Some(outcome(self.jit.invoke(*jit, &arguments)))
                    }
                    (Some(function), Some((compiled, binding))) if compiled.runnable[function as usize] => {
                        exercised.insert((index, function));
                        self.totals.cases += 1;
                        Some(binding.call(&mut self.jit, compiled, function, &arguments))
                    }
                    _ => None,
                };
                let Some(actual) = actual else {
                    // Not compiled: the interpreter runs it in the other store too, so that both stay the same.
                    let _ = self.jit.invoke(*jit, &arguments);
                    return;
                };
                if actual == Outcome::Trap(super::TRAP_NOT_COMPILED) {
                    // The stores differ from here on.
                    self.totals.not_compiled += 1;
                    self.modules[index].diverged = true;
                    return;
                }
                if !same_outcome(&expected, &actual) {
                    self.fail(format!(
                        "line {line}: {field}{arguments:?}: interpreter {expected:?}, JIT {actual:?}"
                    ));
                    return;
                }
                // NB: How deep the stack gets before it is exhausted differs, and so does what the code did on the way.
                if expected != Outcome::Trap(libwasm_engine::abi::TRAP_STACK_EXHAUSTED) {
                    self.compare_state(index, line, field);
                }
            }
            Some("get") => {
                let (Some(ExternValue::Global(a)), Some(ExternValue::Global(b))) =
                    (interpreted_exports.get(field), jit_exports.get(field))
                else {
                    return;
                };
                let (a, b) = (self.interpreted.global(*a).get(), self.jit.global(*b).get());
                if !same_value(&a, &b) {
                    self.fail(format!("line {line}: global {field}: interpreter {a:?}, JIT {b:?}"));
                }
            }
            _ => {}
        }
    }

    /// Compares the memories and globals of a module's instances in both stores.
    fn compare_state(&mut self, index: usize, line: u64, field: &str) {
        let loaded = &self.modules[index];
        if let Some(difference) = state_difference(&self.interpreted, loaded.interpreted, &self.jit, loaded.jit) {
            self.fail(format!("line {line}: {field}: {difference}"));
        }
    }

    fn fail(&mut self, message: String) {
        self.totals.failures += 1;
        println!("FAIL {}: {message}", self.name);
    }

    fn run(&mut self, script: &Json) {
        let mut exercised = BTreeSet::new();
        let Some(commands) = script.get("commands").and_then(|commands| commands.as_array()) else {
            return;
        };
        for command in commands {
            let line = command.get("line").and_then(|line| line.as_u64()).unwrap_or(0);
            match command.get("type").and_then(|kind| kind.as_str()) {
                Some("module") => {
                    let name = command.get("name").and_then(|name| name.as_str()).map(String::from);
                    if let Some(filename) = command.get("filename").and_then(|filename| filename.as_str()) {
                        self.load(name, filename);
                    }
                }
                Some("register") => {
                    let Some(alias) = command.get("as").and_then(|alias| alias.as_str()) else {
                        continue;
                    };
                    let name = command.get("name").and_then(|name| name.as_str());
                    if let Some(index) = self.module_index(name) {
                        let loaded = &self.modules[index];
                        let exports = (
                            Self::exports(&self.interpreted, loaded.interpreted),
                            Self::exports(&self.jit, loaded.jit),
                        );
                        self.registered.insert(alias.to_string(), exports);
                    }
                }
                Some("action" | "assert_return" | "assert_trap" | "assert_exhaustion") => {
                    if let Some(action) = command.get("action") {
                        self.act(action, line, &mut exercised);
                    }
                }
                _ => {}
            }
        }
        self.totals.exercised += exercised.len();
        if self.verbose {
            println!("{}: {} functions exercised", self.name, exercised.len());
        }
    }
}

pub fn scripts(paths: &[String]) -> Vec<PathBuf> {
    let mut result = Vec::new();
    for path in paths {
        let path = PathBuf::from(path);
        if path.is_dir() {
            let mut entries = std::fs::read_dir(&path)
                .unwrap()
                .filter_map(|entry| entry.ok().map(|entry| entry.path()))
                .filter(|path| path.extension().is_some_and(|extension| extension == "json"))
                .collect::<Vec<_>>();
            entries.sort();
            result.extend(entries);
        } else {
            result.push(path);
        }
    }
    result
}

/// Runs every script of `paths` (scripts or directories of them).
/// Runs the scripts at `paths`. With `runtime`, the JIT's store runs every call through the runtime, which compiles
/// what `LIBWASM_JIT` says, instead of calling compiled code directly.
pub fn run(paths: &[String], verbose: bool, options: Options, runtime: bool) -> Totals {
    run_with_dump(paths, verbose, options, runtime, None)
}

/// `run()`, writing what the interpreter did in every action to `dump`: a file that should be the same on every
/// architecture.
pub fn run_with_dump(
    paths: &[String],
    verbose: bool,
    options: Options,
    runtime: bool,
    mut dump: Option<&mut dyn std::io::Write>,
) -> Totals {
    let mut totals = Totals::default();
    for path in scripts(paths) {
        let Ok(text) = std::fs::read_to_string(&path) else {
            continue;
        };
        let Ok(script) = serde_json::from_str::<Json>(&text) else {
            continue;
        };
        totals.scripts += 1;
        let mut interpreted = Store::new();
        interpreted.disable_jit();
        let mut jit = Store::new();
        let spectest = (spectest(&mut interpreted), spectest(&mut jit));
        let mut registered = HashMap::new();
        registered.insert("spectest".to_string(), spectest);
        let mut runner = Script {
            directory: path.parent().unwrap_or(Path::new(".")).to_path_buf(),
            interpreted,
            jit,
            registered,
            modules: Vec::new(),
            current: None,
            totals: &mut totals,
            verbose,
            name: path.file_name().unwrap().to_string_lossy().into_owned(),
            options,
            runtime,
            dump: dump.as_mut().map(|dump| &mut **dump as &mut dyn std::io::Write),
        };
        runner.run(&script);
    }
    totals
}

/// Writes `text`, a script in the spec testsuite's text format, to `directory` as `NAME.json` and the binary modules
/// next to it, and returns the JSON file's path, or None where there is no wasm-tools to convert it.
pub fn write_wast(directory: &Path, name: &str, text: &str) -> Option<PathBuf> {
    std::fs::create_dir_all(directory).unwrap();
    let wast = directory.join(format!("{name}.wast"));
    std::fs::write(&wast, text).unwrap();
    let json = directory.join(format!("{name}.json"));
    let status = std::process::Command::new(std::env::var("WASM_TOOLS").unwrap_or_else(|_| "wasm-tools".into()))
        .arg("json-from-wast")
        .arg(&wast)
        .arg("-o")
        .arg(&json)
        .arg("--wasm-dir")
        .arg(directory)
        .status()
        .ok()?;
    assert!(
        status.success(),
        "wasm-tools json-from-wast failed on {}",
        wast.display()
    );
    Some(json)
}
