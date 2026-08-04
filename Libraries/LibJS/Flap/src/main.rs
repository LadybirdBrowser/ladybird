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
use std::path::Path;

const USAGE: &str = "Usage: flapc --arch x86_64 --input <file.flap> --output <file.S> [--constants <file> | --layout-spec <file> --clang-args <file>] [--optimization-report <file>] [--dump-changed-ir]";

struct CommandLine {
    options: CompileOptions,
    input_path: String,
    output_path: String,
    constants_path: Option<String>,
    layout_spec_path: Option<String>,
    clang_args_path: Option<String>,
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
    let constants = read_layout(&command_line)?;

    let compiler = Compiler::new(command_line.options);
    let unit = CompilationUnit {
        source: SourceInput {
            name: &command_line.input_path,
            contents: &source,
        },
        constants: constants.as_ref().map(|(name, contents)| SourceInput {
            name: name.as_str(),
            contents: contents.as_str(),
        }),
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
    let mut layout_spec_path = None;
    let mut clang_args_path = None;
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
            "--layout-spec" => layout_spec_path = Some(required_value(&mut args, "--layout-spec")?),
            "--clang-args" => clang_args_path = Some(required_value(&mut args, "--clang-args")?),
            "--optimization-report" => {
                optimization_report_path = Some(required_value(&mut args, "--optimization-report")?);
            }
            _ => return Err(format!("Unknown argument: {argument}")),
        }
    }

    if dump_changed_ir && optimization_report_path.is_none() {
        return Err("--dump-changed-ir requires --optimization-report".to_string());
    }
    if layout_spec_path.is_some() && constants_path.is_some() {
        return Err("--layout-spec and --constants are mutually exclusive".to_string());
    }
    if layout_spec_path.is_some() != clang_args_path.is_some() {
        return Err("--layout-spec requires --clang-args".to_string());
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
        layout_spec_path,
        clang_args_path,
        optimization_report_path,
        dump_changed_ir,
    })
}

fn read_layout(command_line: &CommandLine) -> Result<Option<(String, String)>, String> {
    let Some(spec_path) = command_line.layout_spec_path.as_deref() else {
        return command_line
            .constants_path
            .as_deref()
            .map(|path| read_source(path).map(|contents| (path.to_string(), contents)))
            .transpose();
    };
    let spec = read_source(spec_path)?;
    let clang_args_path = command_line
        .clang_args_path
        .as_deref()
        .expect("--layout-spec requires --clang-args");
    let clang_arguments = read_source(clang_args_path)?
        .lines()
        .map(str::trim)
        .filter(|line| !line.is_empty())
        .map(str::to_string)
        .collect::<Vec<_>>();
    let output_path = Path::new(&command_line.output_path);
    let output_directory = output_path.parent().unwrap_or_else(|| Path::new("."));
    let probe_path = output_directory.join("layout-probe.cpp");
    let (layout, dependencies) = flapc::read_layout_from_headers(&spec, &probe_path, &clang_arguments)
        .map_err(|error| format!("{spec_path}: {error}"))?;
    let layout_path = output_directory.join("layout.conf");
    fs::write(&layout_path, &layout).map_err(|error| format!("Failed to write {}: {error}", layout_path.display()))?;
    write_depfile(
        &format!("{}.d", command_line.output_path),
        &command_line.output_path,
        spec_path,
        &dependencies,
    )?;
    Ok(Some((layout_path.to_string_lossy().into_owned(), layout)))
}

fn write_depfile(path: &str, output_path: &str, spec_path: &str, dependencies: &[String]) -> Result<(), String> {
    fn escape(value: &str) -> String {
        value.replace('\\', "\\\\").replace(' ', "\\ ").replace('#', "\\#")
    }

    let mut contents = format!("{}:", escape(output_path));
    for dependency in std::iter::once(spec_path).chain(dependencies.iter().map(String::as_str)) {
        contents.push_str(" \\\n  ");
        contents.push_str(&escape(dependency));
    }
    contents.push('\n');
    fs::write(path, contents).map_err(|error| format!("Failed to write {path}: {error}"))
}

fn required_value(args: &mut impl Iterator<Item = String>, option: &str) -> Result<String, String> {
    args.next().ok_or_else(|| format!("{option} requires a value"))
}

fn read_source(path: &str) -> Result<String, String> {
    fs::read_to_string(path).map_err(|error| format!("Failed to read {path}: {error}"))
}
