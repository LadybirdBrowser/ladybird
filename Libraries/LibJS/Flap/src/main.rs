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

const USAGE: &str = "Usage: flapc --layout-spec <file> --emit-layout-probe <file.cpp>\n       flapc --arch x86_64 --input <file.flap> --output <file.S> [--constants <file> | --layout-spec <file> --layout-object <file.o> --objdump <objdump>] [--optimization-report <file>] [--dump-changed-ir]";

struct CommandLine {
    options: CompileOptions,
    input_path: Option<String>,
    output_path: Option<String>,
    emit_layout_probe_path: Option<String>,
    constants_path: Option<String>,
    layout_spec_path: Option<String>,
    layout_object_path: Option<String>,
    objdump_path: Option<String>,
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
    if let Some(output_path) = &command_line.emit_layout_probe_path {
        return emit_layout_probe(&command_line, output_path);
    }

    let input_path = command_line.input_path.as_deref().ok_or(USAGE)?;
    let output_path = command_line.output_path.as_deref().ok_or(USAGE)?;
    let source = read_source(input_path)?;
    let constants = read_layout(&command_line)?;

    let compiler = Compiler::new(command_line.options);
    let unit = CompilationUnit {
        source: SourceInput {
            name: input_path,
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

    fs::write(output_path, assembly.as_str()).map_err(|error| format!("Failed to write {output_path}: {error}"))
}

fn parse_command_line() -> Result<CommandLine, String> {
    let mut args = std::env::args().skip(1);
    let mut architecture = Architecture::X86_64;
    let mut object_format = ObjectFormat::MachO;
    let mut input_path = None;
    let mut output_path = None;
    let mut emit_layout_probe_path = None;
    let mut constants_path = None;
    let mut layout_spec_path = None;
    let mut layout_object_path = None;
    let mut objdump_path = None;
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
            "--emit-layout-probe" => emit_layout_probe_path = Some(required_value(&mut args, "--emit-layout-probe")?),
            "--constants" => constants_path = Some(required_value(&mut args, "--constants")?),
            "--layout-spec" => layout_spec_path = Some(required_value(&mut args, "--layout-spec")?),
            "--layout-object" => layout_object_path = Some(required_value(&mut args, "--layout-object")?),
            "--objdump" => objdump_path = Some(required_value(&mut args, "--objdump")?),
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
    if emit_layout_probe_path.is_some() {
        if layout_spec_path.is_none() {
            return Err("--emit-layout-probe requires --layout-spec".to_string());
        }
        if input_path.is_some()
            || output_path.is_some()
            || constants_path.is_some()
            || layout_object_path.is_some()
            || objdump_path.is_some()
            || optimization_report_path.is_some()
            || dump_changed_ir
        {
            return Err("--emit-layout-probe can only be used with --layout-spec".to_string());
        }
    } else if layout_spec_path.is_some() != layout_object_path.is_some()
        || layout_spec_path.is_some() != objdump_path.is_some()
    {
        return Err("--layout-spec requires --layout-object and --objdump".to_string());
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
        input_path,
        output_path,
        emit_layout_probe_path,
        constants_path,
        layout_spec_path,
        layout_object_path,
        objdump_path,
        optimization_report_path,
        dump_changed_ir,
    })
}

fn emit_layout_probe(command_line: &CommandLine, output_path: &str) -> Result<(), String> {
    let spec_path = command_line
        .layout_spec_path
        .as_deref()
        .expect("--emit-layout-probe requires --layout-spec");
    let spec = read_source(spec_path)?;
    let probe = flapc::generate_layout_probe(&spec).map_err(|error| format!("{spec_path}: {error}"))?;
    if let Some(parent) = Path::new(output_path).parent() {
        fs::create_dir_all(parent).map_err(|error| format!("Failed to create {}: {error}", parent.display()))?;
    }
    fs::write(output_path, probe).map_err(|error| format!("Failed to write {output_path}: {error}"))
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
    let layout_object_path = command_line
        .layout_object_path
        .as_deref()
        .expect("--layout-spec requires --layout-object");
    let objdump_path = command_line
        .objdump_path
        .as_deref()
        .expect("--layout-spec requires --objdump");
    let output_path = Path::new(
        command_line
            .output_path
            .as_deref()
            .expect("normal compilation requires --output"),
    );
    let output_directory = output_path.parent().unwrap_or_else(|| Path::new("."));
    let layout = flapc::read_layout_from_object(&spec, Path::new(objdump_path), Path::new(layout_object_path))
        .map_err(|error| format!("{spec_path}: {error}"))?;
    let layout_path = output_directory.join("layout.conf");
    fs::write(&layout_path, &layout).map_err(|error| format!("Failed to write {}: {error}", layout_path.display()))?;
    Ok(Some((layout_path.to_string_lossy().into_owned(), layout)))
}

fn required_value(args: &mut impl Iterator<Item = String>, option: &str) -> Result<String, String> {
    args.next().ok_or_else(|| format!("{option} requires a value"))
}

fn read_source(path: &str) -> Result<String, String> {
    fs::read_to_string(path).map_err(|error| format!("Failed to read {path}: {error}"))
}
