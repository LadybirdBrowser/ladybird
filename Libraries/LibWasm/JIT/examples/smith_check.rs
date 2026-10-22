/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Generates random modules in the subset the JIT compiles with `wasm-tools smith`, calls every exported function
//! the JIT compiled with random arguments, and compares results, traps, memories and globals with the interpreter.
//!
//! ```text
//! smith_check [--count N] [--seed S] [--keep DIRECTORY] [--wasm-tools PATH] [--stress-registers] [--no-licm]
//!     [--simd] [--features gc,threads,memory64,multi-memory]
//! ```
//!
//! `--features` turns on proposals besides the default ones, in every module. Modules that wait on shared memory are
//! skipped, since nothing would wake them. With GC, modules get less fuel, since the stores never free objects.

#[path = "../tests/harness/mod.rs"]
mod harness;

use harness::{CompiledModule, Outcome, TestBinding, function_index};
use libwasm_engine::runtime::{ExternValue, Store, Trap, Value};
use libwasm_engine::types::ValType;
use libwasm_jit::Options;
use std::io::Write;
use std::process::{Command, Stdio};
use std::sync::Arc;

// NB: NaN payloads may differ between implementations, and reinterpreting them makes the difference visible, so
//     the modules canonicalize them.
const SMITH_FLAGS: &[&str] = &[
    "--ensure-termination",
    "--canonicalize-nans",
    "true",
    "--fuel",
    "1000000",
    "--max-imports",
    "0",
    "--max-tables",
    "4",
    "--max-memories",
    "1",
    "--max-memory32-bytes",
    "262144",
    "--max-memory64-bytes",
    "262144",
    "--bulk-memory-enabled",
    "true",
    "--reference-types-enabled",
    "true",
    "--exceptions-enabled",
    "false",
    "--gc-enabled",
    "false",
    "--tail-call-enabled",
    "true",
    "--threads-enabled",
    "false",
    "--memory64-enabled",
    "false",
    "--custom-page-sizes-enabled",
    "false",
    "--wide-arithmetic-enabled",
    "false",
    "--multi-value-enabled",
    "true",
    "--saturating-float-to-int-enabled",
    "true",
    "--sign-extension-ops-enabled",
    "true",
    "--export-everything",
    "true",
    "--min-funcs",
    "4",
    "--max-funcs",
    "40",
];

/// xorshift64*.
struct Random(u64);

impl Random {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 >> 12;
        self.0 ^= self.0 << 25;
        self.0 ^= self.0 >> 27;
        self.0.wrapping_mul(0x2545_f491_4f6c_dd1d)
    }

    fn below(&mut self, bound: u64) -> u64 {
        self.next() % bound
    }

    fn bits(&mut self, width: u32) -> u64 {
        const SPECIAL: [u64; 8] = [0, 1, 2, 3, 7, 0xff, 0x7fff_ffff, 0x8000_0000];
        let bits = match self.below(4) {
            0 => SPECIAL[self.below(SPECIAL.len() as u64) as usize],
            1 => self.below(64),
            2 => self.next().wrapping_neg() & 0xffff,
            _ => self.next(),
        };
        if width == 32 { bits & 0xffff_ffff } else { bits }
    }

    fn argument(&mut self, ty: ValType) -> Option<Value> {
        Some(match ty {
            ValType::I32 => Value::I32(self.bits(32) as u32 as i32),
            ValType::I64 => Value::I64(self.bits(64) as i64),
            ValType::F32 => {
                const VALUES: [f32; 8] = [0.0, -0.0, 1.0, -1.5, 2147483648.0, f32::INFINITY, f32::NAN, 1e30];
                Value::F32(match self.below(3) {
                    0 => VALUES[self.below(8) as usize].to_bits(),
                    1 => (self.below(2000) as f32 - 1000.0).to_bits(),
                    _ => self.next() as u32,
                })
            }
            ValType::F64 => {
                const VALUES: [f64; 8] = [0.0, -0.0, 1.0, -1.5, 9.3e18, f64::NEG_INFINITY, f64::NAN, 4294967296.0];
                Value::F64(match self.below(3) {
                    0 => VALUES[self.below(8) as usize].to_bits(),
                    1 => (self.below(2000) as f64 - 1000.0).to_bits(),
                    _ => self.next(),
                })
            }
            ValType::V128 => Value::V128(u128::from(self.bits(64)) | (u128::from(self.bits(64)) << 64)),
            ValType::Ref(reference) if reference.nullable => Value::Ref(0),
            ValType::Ref(_) => return None,
        })
    }
}

fn outcome(result: Result<Vec<Value>, Trap>) -> Outcome {
    match result {
        Ok(values) => Outcome::Values(values),
        Err(trap) => Outcome::Trap(trap.code()),
    }
}

#[derive(Default)]
struct Totals {
    modules: usize,
    functions: usize,
    compiled: usize,
    runnable: usize,
    cases: usize,
    failures: usize,
    not_compiled: usize,
    /// Why functions were not compiled, and how often.
    unsupported: std::collections::BTreeMap<&'static str, usize>,
}

fn main() {
    harness::run_main(checked_main);
}

fn checked_main() {
    let mut count = 100;
    let mut seed = 1u64;
    let mut keep = None;
    let mut stress_registers = false;
    let mut no_licm = false;
    let mut simd = false;
    let mut features: Vec<String> = Vec::new();
    let mut wasm_tools = std::env::var("HOME")
        .map(|home| format!("{home}/.cargo/bin/wasm-tools"))
        .unwrap_or_default();
    let mut arguments = std::env::args().skip(1);
    while let Some(argument) = arguments.next() {
        match argument.as_str() {
            "--count" => {
                count = arguments
                    .next()
                    .and_then(|value| value.parse().ok())
                    .expect("--count N");
            }
            "--seed" => seed = arguments.next().and_then(|value| value.parse().ok()).expect("--seed S"),
            "--keep" => keep = arguments.next(),
            "--wasm-tools" => wasm_tools = arguments.next().expect("--wasm-tools PATH"),
            "--stress-registers" => stress_registers = true,
            "--no-licm" => no_licm = true,
            "--simd" => simd = true,
            "--features" => {
                features = arguments
                    .next()
                    .expect("--features LIST")
                    .split(',')
                    .map(str::to_string)
                    .collect();
            }
            _ => panic!("unknown argument {argument}"),
        }
    }
    let simd_flag = |iteration: usize| {
        if simd || iteration.is_multiple_of(2) {
            "true"
        } else {
            "false"
        }
    };
    let mut flags: Vec<&str> = SMITH_FLAGS.to_vec();
    let mut set_flag = |name: &str, value: &'static str| {
        let position = flags.iter().position(|flag| *flag == name).expect("a known flag");
        flags[position + 1] = value;
    };
    for feature in &features {
        match feature.as_str() {
            "gc" => {
                set_flag("--gc-enabled", "true");
                // NB: The stores of these tests never free GC objects, so allocating loops run out of memory with
                //     the usual fuel.
                set_flag("--fuel", "20000");
            }
            "threads" => set_flag("--threads-enabled", "true"),
            "memory64" => set_flag("--memory64-enabled", "true"),
            "multi-memory" => set_flag("--max-memories", "4"),
            _ => panic!("unknown feature {feature}"),
        }
    }
    let mut random = Random(seed.wrapping_mul(0x9e37_79b9_7f4a_7c15) | 1);
    let mut totals = Totals::default();
    for iteration in 0..count {
        let input = (0..1000 + random.below(30000))
            .map(|_| random.next() as u8)
            .collect::<Vec<_>>();
        let mut child = Command::new(&wasm_tools)
            .arg("smith")
            .args(&flags)
            // NB: Every other module without SIMD, unless --simd says otherwise.
            .args(["--simd-enabled", simd_flag(iteration)])
            .args(["--relaxed-simd-enabled", simd_flag(iteration)])
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .expect("wasm-tools runs");
        child.stdin.take().unwrap().write_all(&input).unwrap();
        let output = child.wait_with_output().unwrap();
        if !output.status.success() {
            continue;
        }
        let bytes = output.stdout;
        let Ok(module) = libwasm_engine::compile(&bytes) else {
            continue;
        };
        if waits(&module) {
            continue;
        }
        let module = Arc::new(module);
        let mut interpreted = Store::new();
        let mut jit = Store::new();
        let (Ok(interpreted_instance), Ok(jit_instance)) = (
            interpreted.instantiate(module.clone(), &[]),
            jit.instantiate(module.clone(), &[]),
        ) else {
            continue;
        };
        if let Some(directory) = &keep {
            // NB: Where a crash leaves it.
            std::fs::write(format!("{directory}/current.wasm"), &bytes).unwrap();
        }
        let compiled = CompiledModule::compile(
            module.clone(),
            Options {
                verify_ir: true,
                stress_registers,
                no_licm,
                ..Options::default()
            },
        );
        let mut binding = TestBinding::new(&compiled, &jit, jit_instance);
        totals.modules += 1;
        totals.functions += module.code.len();
        let first = module.imported_function_count as usize;
        totals.compiled += compiled.entries[first..].iter().filter(|entry| **entry != 0).count();
        totals.runnable += compiled.runnable[first..].iter().filter(|runnable| **runnable).count();
        for failure in compiled.failures.iter().flatten() {
            let what = failure.reason();
            *totals.unsupported.entry(what).or_default() += 1;
        }
        let mut exports = interpreted
            .instance(interpreted_instance)
            .exports
            .iter()
            .map(|(name, export)| (name.clone(), *export))
            .collect::<Vec<_>>();
        exports.sort_by(|a, b| a.0.cmp(&b.0));
        let mut failed = false;
        let mut stop = false;
        for (name, export) in exports {
            let ExternValue::Function(interpreted_function) = export else {
                continue;
            };
            let Some(ExternValue::Function(jit_function)) = jit.instance(jit_instance).export(&name) else {
                continue;
            };
            let Some(index) = function_index(&jit, jit_function, jit_instance) else {
                continue;
            };
            let ty = interpreted.function(interpreted_function).ty.clone();
            for _ in 0..4 {
                let Some(arguments) = ty
                    .params
                    .iter()
                    .map(|ty| random.argument(*ty))
                    .collect::<Option<Vec<_>>>()
                else {
                    break;
                };
                let expected = outcome(interpreted.invoke(interpreted_function, &arguments));
                if !compiled.runnable[index as usize] {
                    let _ = jit.invoke(jit_function, &arguments);
                    continue;
                }
                totals.cases += 1;
                let actual = binding.call(&mut jit, &compiled, index, &arguments);
                if actual == Outcome::Trap(harness::TRAP_NOT_COMPILED) {
                    totals.not_compiled += 1;
                    stop = true;
                    break;
                }
                let exhausted = Outcome::Trap(libwasm_engine::abi::TRAP_STACK_EXHAUSTED);
                if expected == exhausted || actual == exhausted {
                    // NB: How deep the stack gets before it is exhausted differs between the tiers, and so does what
                    //     the code did on the way there, like how much of a fuel global it used. The stores differ
                    //     from here on.
                    stop = true;
                    break;
                }
                let mut problem = None;
                if !harness::script::same_outcome(&expected, &actual) {
                    problem = Some(format!("{name}{arguments:?}: interpreter {expected:?}, JIT {actual:?}"));
                } else if let Some(difference) =
                    harness::script::state_difference(&interpreted, interpreted_instance, &jit, jit_instance)
                {
                    problem = Some(format!("{name}{arguments:?}: {difference}"));
                }
                if let Some(problem) = problem {
                    totals.failures += 1;
                    failed = true;
                    println!("FAIL module {iteration}: {problem}");
                    break;
                }
            }
            if failed || stop {
                break;
            }
        }
        if failed && let Some(directory) = &keep {
            let path = format!("{directory}/smith-{seed}-{iteration}.wasm");
            std::fs::write(&path, &bytes).unwrap();
            println!("  kept {path}");
        }
    }
    println!(
        "{} modules, {} functions: {} compiled, {} runnable; {} cases, {} failures, {} reached uncompiled code",
        totals.modules,
        totals.functions,
        totals.compiled,
        totals.runnable,
        totals.cases,
        totals.failures,
        totals.not_compiled
    );
    for (what, count) in &totals.unsupported {
        println!("{count:>8} not compiled: {what}");
    }
    if totals.failures > 0 {
        std::process::exit(1);
    }
}

/// Whether a function of the module waits on shared memory (`memory.atomic.wait32` and `wait64`).
fn waits(module: &libwasm_engine::module::Module) -> bool {
    use libwasm_jit::decode::{Decoder, Instruction};
    module.code.iter().any(|body| {
        let mut decoder = Decoder::new(&module.bytes, body.code.start as usize, body.code.end as usize);
        while !decoder.is_at_end() {
            if let Instruction::Atomic(1 | 2) = decoder.decode() {
                return true;
            }
        }
        false
    })
}
