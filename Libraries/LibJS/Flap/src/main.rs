/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use flapc::{
    Architecture, CompilationUnit, CompileOptions, Compiler, ObjectFormat, OptimizationReportOptions, SourceInput,
    Target,
};
use std::fs;

const USAGE: &str = "Usage: flapc --arch x86_64 --input <file.flap> --output <file.S> [--constants <file>] [--optimization-report <file>] [--dump-changed-ir]";

struct CommandLine {
    options: CompileOptions,
    input_path: String,
    output_path: String,
    constants_path: Option<String>,
    optimization_report_path: Option<String>,
    dump_changed_ir: bool,
}

fn main() {
    if let Err(error) = run() {
        eprintln!("{error}");
        std::process::exit(1);
    }
}

fn run() -> Result<(), String> {
    let command_line = parse_command_line()?;
    let source = read_source(&command_line.input_path)?;
    let constants = command_line.constants_path.as_deref().map(read_source).transpose()?;

    let compiler = Compiler::new(command_line.options);
    let unit = CompilationUnit {
        source: SourceInput {
            name: &command_line.input_path,
            contents: &source,
        },
        constants: command_line
            .constants_path
            .as_deref()
            .zip(constants.as_deref())
            .map(|(name, contents)| SourceInput { name, contents }),
    };
    let assembly = if let Some(report_path) = &command_line.optimization_report_path {
        let (prepared, report) = compiler
            .prepare_with_optimization_report(
                unit,
                OptimizationReportOptions {
                    include_remarks: true,
                    dump_changed_ir: command_line.dump_changed_ir,
                },
            )
            .map_err(|error| error.to_string())?;
        let assembly = compiler
            .compile_prepared(&prepared)
            .map_err(|error| error.to_string())?;
        fs::write(report_path, report.to_string())
            .map_err(|error| format!("Failed to write {report_path}: {error}"))?;
        assembly
    } else {
        compiler.compile(unit).map_err(|error| error.to_string())?
    };

    fs::write(&command_line.output_path, assembly.as_str())
        .map_err(|error| format!("Failed to write {}: {error}", command_line.output_path))
}

fn parse_command_line() -> Result<CommandLine, String> {
    let mut args = std::env::args().skip(1);
    let mut architecture = Architecture::X86_64;
    let mut object_format = ObjectFormat::MachO;
    let mut input_path = None;
    let mut output_path = None;
    let mut constants_path = None;
    let mut has_jscvt = false;
    let mut enable_assertions = false;
    let mut optimization_report_path = None;
    let mut dump_changed_ir = false;

    while let Some(argument) = args.next() {
        match argument.as_str() {
            "--has-jscvt" => has_jscvt = true,
            "--enable-assertions" => enable_assertions = true,
            "--dump-changed-ir" => dump_changed_ir = true,
            "--arch" => {
                architecture = match required_value(&mut args, "--arch")?.as_str() {
                    "x86_64" => Architecture::X86_64,
                    "aarch64" => Architecture::Aarch64,
                    other => return Err(format!("Unknown architecture: {other}")),
                };
            }
            "--object-format" => {
                object_format = match required_value(&mut args, "--object-format")?.as_str() {
                    "macho" => ObjectFormat::MachO,
                    "elf" => ObjectFormat::Elf,
                    "coff" => ObjectFormat::Coff,
                    other => return Err(format!("Unknown object format: {other}")),
                };
            }
            "--input" => input_path = Some(required_value(&mut args, "--input")?),
            "--output" => output_path = Some(required_value(&mut args, "--output")?),
            "--constants" => constants_path = Some(required_value(&mut args, "--constants")?),
            "--optimization-report" => {
                optimization_report_path = Some(required_value(&mut args, "--optimization-report")?);
            }
            _ => return Err(format!("Unknown argument: {argument}")),
        }
    }

    if dump_changed_ir && optimization_report_path.is_none() {
        return Err("--dump-changed-ir requires --optimization-report".to_string());
    }

    Ok(CommandLine {
        options: CompileOptions {
            target: Target {
                architecture,
                object_format,
            },
            has_jscvt,
            enable_assertions,
        },
        input_path: input_path.ok_or(USAGE)?,
        output_path: output_path.ok_or(USAGE)?,
        constants_path,
        optimization_report_path,
        dump_changed_ir,
    })
}

fn required_value(args: &mut impl Iterator<Item = String>, option: &str) -> Result<String, String> {
    args.next().ok_or_else(|| format!("{option} requires a value"))
}

fn read_source(path: &str) -> Result<String, String> {
    fs::read_to_string(path).map_err(|error| format!("Failed to read {path}: {error}"))
}
