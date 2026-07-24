/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Flap compiler library.
//!
//! Flap compiles typed interpreter handler definitions through semantic
//! analysis, SSA construction and optimization, machine lowering, register
//! allocation, and target-specific assembly emission. The compiler operates
//! entirely on in-memory inputs and outputs; the `flapc` binary is only a file
//! system and command-line adapter.

pub(crate) mod bytecode;
pub(crate) mod frontend;
pub(crate) mod hir;
pub(crate) mod identity;
pub(crate) mod intrinsic;
pub(crate) mod low_ir;
pub(crate) mod ssa;
pub(crate) mod target;
pub(crate) mod types;

use bytecode::HandlerLayout as BytecodeHandlerLayout;
use frontend::diagnostic::{Diagnostic, SourceLocation};
use frontend::layout::{LayoutConstants, LayoutDatabase, LayoutError};
use identity::HandlerId;
use std::collections::HashMap;
use std::error::Error;
use std::fmt;
use target::emitter::DISPATCH_TABLE_SIZE;
use types::BlockTemperature;

pub use frontend::diagnostic::SourceSpan;
pub use low_ir::Program as LowProgram;
pub use ssa::report::OptimizationReport;
pub use target::ir::{
    AllocatedProgram, MachineProgram, Program as TargetProgram,
};

/// A machine architecture supported by Flap's textual assembly backends.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub enum Architecture {
    X86_64,
    Aarch64,
}

/// The object-file conventions expected by the generated assembly.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ObjectFormat {
    MachO,
    Elf,
    Coff,
}

/// The target selected for a compilation.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Target {
    pub architecture: Architecture,
    pub object_format: ObjectFormat,
}

/// Options which affect machine lowering or assembly emission.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CompileOptions {
    pub target: Target,
    pub has_jscvt: bool,
    pub enable_assertions: bool,
}

/// Controls optional diagnostics collected while optimizing a program.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct OptimizationReportOptions {
    pub include_remarks: bool,
    pub dump_changed_ir: bool,
}

/// In-memory source inputs for one Flap compilation.
pub struct CompilationUnit<'a> {
    pub source_name: &'a str,
    pub source: &'a str,
    pub constants: Option<&'a str>,
    pub bytecode_def: Option<&'a str>,
}

#[derive(Clone)]
struct HandlerTemplate {
    id: HandlerId,
    function: ssa::Function,
    temperature: BlockTemperature,
}

impl HandlerTemplate {
    fn name(&self) -> &str {
        &self.function.name
    }
}

/// Target-independent compiler state which can be reused across compilations.
pub struct PreparedProgram {
    constants: LayoutConstants,
    bytecode: BytecodeMetadata,
    handlers: Vec<HandlerTemplate>,
}

/// Parsed bytecode definitions shared by all handlers in a program.
struct BytecodeMetadata {
    handler_layouts: Vec<BytecodeHandlerLayout>,
    dispatch_handlers: Vec<Option<HandlerId>>,
}

/// Textual assembly produced by a Flap backend.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Assembly {
    text: String,
}

impl Assembly {
    pub fn as_str(&self) -> &str {
        &self.text
    }

}

/// A diagnostic produced by one of the compiler stages.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CompileStage {
    Layout,
    Parse,
    Semantic,
    Ssa,
    LowIr,
    Selection,
    Allocation,
    Verification,
    Finalization,
    Emission,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CompileError {
    pub stage: CompileStage,
    pub handler: Option<String>,
    pub span: Option<SourceSpan>,
    pub message: String,
}

impl CompileError {
    pub(crate) fn new(
        stage: CompileStage,
        handler: Option<&str>,
        message: impl Into<String>,
    ) -> Self {
        Self {
            stage,
            handler: handler.map(str::to_string),
            span: None,
            message: message.into(),
        }
    }

    fn from_diagnostic(stage: CompileStage, diagnostic: Diagnostic) -> Self {
        Self {
            stage,
            handler: None,
            span: Some(diagnostic.span),
            message: diagnostic.to_string(),
        }
    }

    fn from_layout_error(error: LayoutError) -> Self {
        let location = SourceLocation {
            offset: 0,
            line: error.line,
            column: 1,
        };
        Self {
            stage: CompileStage::Layout,
            handler: None,
            span: Some(SourceSpan {
                start: location,
                end: location,
            }),
            message: error.to_string(),
        }
    }
}

impl fmt::Display for CompileError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(formatter, "{:?}", self.stage)?;
        if let Some(handler) = &self.handler {
            write!(formatter, " error in handler '{handler}'")?;
        } else {
            formatter.write_str(" error")?;
        }
        write!(formatter, ": {}", self.message)
    }
}

impl Error for CompileError {}

/// A reusable Flap compiler configured for one target.
pub struct Compiler {
    options: CompileOptions,
}

impl Compiler {
    pub fn new(options: CompileOptions) -> Self {
        Self { options }
    }

    pub fn compile(&self, unit: CompilationUnit<'_>) -> Result<Assembly, CompileError> {
        let prepared = self.prepare(unit)?;
        self.compile_prepared(&prepared)
    }

    /// Parse, type-check, inline, and optimize all reusable handler templates.
    pub fn prepare(&self, unit: CompilationUnit<'_>) -> Result<PreparedProgram, CompileError> {
        self.prepare_internal(unit, None).map(|(prepared, _)| prepared)
    }

    /// Prepare handler templates and retain a structured report of the SSA optimization pipeline.
    pub fn prepare_with_optimization_report(
        &self,
        unit: CompilationUnit<'_>,
        options: OptimizationReportOptions,
    ) -> Result<(PreparedProgram, OptimizationReport), CompileError> {
        let (prepared, report) = self.prepare_internal(unit, Some(options))?;
        Ok((prepared, report.expect("optimization reporting was requested")))
    }

    fn prepare_internal(
        &self,
        unit: CompilationUnit<'_>,
        report_options: Option<OptimizationReportOptions>,
    ) -> Result<(PreparedProgram, Option<OptimizationReport>), CompileError> {
        let layouts = unit
            .constants
            .map(LayoutDatabase::parse)
            .transpose()
            .map_err(CompileError::from_layout_error)?;

        let ast = frontend::parser::parse(unit.source_name, unit.source)
            .map_err(|diagnostic| CompileError::from_diagnostic(CompileStage::Parse, diagnostic))?;
        let typed_program = if let Some(layouts) = &layouts {
            hir::check_with_layouts(unit.source_name, &ast, layouts.fields())
        } else {
            hir::check(unit.source_name, &ast)
        }
        .map_err(|diagnostic| CompileError::from_diagnostic(CompileStage::Semantic, diagnostic))?;

        let constants = layouts
            .map(LayoutDatabase::into_constants)
            .unwrap_or_default();

        let ssa_inline_functions = typed_program
            .inline_functions
            .iter()
            .enumerate()
            .map(|(index, typed_inline_function)| {
                assert_eq!(typed_inline_function.id.index(), index);
                ssa::lowering::lower_inline_function(typed_inline_function)
            })
            .collect::<Result<Vec<_>, _>>()?;

        let mut handlers = typed_program
            .handlers
            .iter()
            .enumerate()
            .map(|(index, typed_handler)| {
                let mut function = ssa::lowering::lower_handler(typed_handler)?;
                ssa::inline::inline_calls(&mut function, &ssa_inline_functions)?;
                ssa::optimize::resolve_constants(&mut function, &constants);
                Ok(HandlerTemplate {
                    id: HandlerId::new(index),
                    function,
                    temperature: typed_handler.temperature,
                })
            })
            .collect::<Result<Vec<_>, CompileError>>()?;

        let report = if let Some(options) = report_options {
            let reports = handlers
                .iter_mut()
                .map(|handler| {
                    ssa::optimize::optimize_function_with_report(
                        &mut handler.function,
                        ssa::pass::PassManagerOptions {
                            collect_remarks: options.include_remarks,
                            dump_changed_ir: options.dump_changed_ir,
                        },
                    )
                })
                .collect();
            Some(OptimizationReport::new("aot-prepare", reports))
        } else {
            for handler in &mut handlers {
                ssa::optimize::optimize_function(&mut handler.function);
            }
            None
        };

        for handler in &mut handlers {
            ssa::optimize::resolve_layout_constants(
                &mut handler.function,
                &constants,
            )?;
        }

        let handler_ids = handlers
            .iter()
            .map(|handler| (handler.name(), handler.id))
            .collect::<HashMap<_, _>>();
        let (op_layouts, dispatch_handlers) = if let Some(bytecode_def) = unit.bytecode_def {
            let ops = bytecode_def::parse_bytecode_def(bytecode_def);
            // The interpreter dispatches on a single opcode byte, so a table beyond 256 entries
            // would have unreachable handlers.
            if ops.len() > DISPATCH_TABLE_SIZE {
                return Err(CompileError::new(
                    CompileStage::Parse,
                    None,
                    format!(
                        "bytecode definition has {} operations, which exceeds the {DISPATCH_TABLE_SIZE}-entry dispatch table",
                        ops.len()
                    ),
                ));
            }
            (
                Some(bytecode_def::compute_layouts(&ops)),
                ops.iter()
                    .map(|op| handler_ids.get(op.name.as_str()).copied())
                    .collect(),
            )
        } else {
            (None, Vec::new())
        };
        let handler_layouts = handlers
            .iter()
            .map(|handler| {
                BytecodeHandlerLayout::new(
                    &handler.function.parameter_names,
                    op_layouts
                        .as_ref()
                        .and_then(|layouts| layouts.get(handler.name())),
                )
            })
            .collect();

        Ok((
            PreparedProgram {
                constants,
                bytecode: BytecodeMetadata {
                    handler_layouts,
                    dispatch_handlers,
                },
                handlers,
            },
            report,
        ))
    }

    /// Lower every prepared handler template to target-independent LowIR.
    pub fn lower(&self, prepared: &PreparedProgram) -> Result<LowProgram, CompileError> {
        let mut program = LowProgram::new();
        program.runtime =
            low_ir::RuntimeConstants::from_layout(&prepared.constants);
        program.dispatch_handlers = prepared.bytecode.dispatch_handlers.clone();

        for template in &prepared.handlers {
            let mut handler = low_ir::lowering::lower_handler_with_constants(
                &template.function,
                template.temperature == BlockTemperature::Cold,
                &prepared.constants,
                template.id,
            )?;
            low_ir::lowering::materialize_bytecode_layout(
                &mut handler,
                &prepared.bytecode.handler_layouts
                    [template.id.index()],
            )?;
            program.handlers.push(handler);
        }

        Ok(program)
    }

    /// Select legal target instructions for a lowered program.
    pub fn select(&self, program: LowProgram) -> Result<TargetProgram, CompileError> {
        target::selection::select_program(program, self.architecture())
    }

    /// Assign physical registers to a selected target program.
    pub fn allocate(
        &self,
        program: TargetProgram,
    ) -> Result<AllocatedProgram, CompileError> {
        if program.architecture != self.architecture() {
            return Err(CompileError::new(
                CompileStage::Allocation,
                None,
                "selected program architecture does not match compiler target",
            ));
        }
        target::allocator::allocate_program(program)
    }

    /// Finalize allocated CFGs into ordered physical machine instructions.
    pub fn finalize(&self, allocated: AllocatedProgram) -> Result<MachineProgram, CompileError> {
        if allocated.architecture != self.architecture() {
            return Err(CompileError::new(
                CompileStage::Finalization,
                None,
                "allocated program architecture does not match compiler target",
            ));
        }
        target::finalize::finalize_program(allocated, &self.options)
    }

    /// Print textual assembly from a finalized machine program.
    pub fn emit_program(&self, machine: &MachineProgram) -> Result<Assembly, CompileError> {
        if machine.target != self.options.target {
            return Err(CompileError::new(
                CompileStage::Emission,
                None,
                "machine program target does not match compiler target",
            ));
        }
        if let Some(name) = machine.missing_required_runtime_constant() {
            return Err(CompileError::new(
                CompileStage::Emission,
                None,
                format!("required runtime constant '{name}' is missing"),
            ));
        }
        let text = match self.options.target.architecture {
            Architecture::X86_64 => target::x86_64::generate(machine, &self.options),
            Architecture::Aarch64 => target::aarch64::generate(machine, &self.options),
        };
        Ok(Assembly { text })
    }

    /// Compile a previously prepared set of handler templates.
    pub fn compile_prepared(&self, prepared: &PreparedProgram) -> Result<Assembly, CompileError> {
        let program = self.lower(prepared)?;
        let selected = self.select(program)?;
        let allocated = self.allocate(selected)?;
        let machine = self.finalize(allocated)?;
        self.emit_program(&machine)
    }

    fn architecture(&self) -> Architecture {
        self.options.target.architecture
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const REQUIRED_LAYOUT: &str = r#"
const EXECUTION_CONTEXT_EXECUTABLE = 80
const EXECUTION_CONTEXT_PROGRAM_COUNTER = 56
const SIZEOF_EXECUTION_CONTEXT = 120
const VM_RUNNING_EXECUTION_CONTEXT = 15288
const EXECUTABLE_BYTECODE_DATA = 104
const INT32_TAG = 0x7FFA
const BOOLEAN_TAG = 0x7FF9
const INT32_TAG_SHIFTED = 0x7FFA000000000000
const NAN_BASE_TAG = 0x7FF8
const CANON_NAN_BITS = 0x7FF8000000000000
"#;

    fn compiler(architecture: Architecture) -> Compiler {
        Compiler::new(CompileOptions {
            target: Target {
                architecture,
                object_format: ObjectFormat::Elf,
            },
            has_jscvt: false,
            enable_assertions: false,
        })
    }

    fn unit(bytecode_def: &'static str) -> CompilationUnit<'static> {
        CompilationUnit {
            source_name: "test.flap",
            source: "handler Nop() { dispatch_next; }",
            constants: Some(REQUIRED_LAYOUT),
            bytecode_def: Some(bytecode_def),
        }
    }

    #[test]
    fn compiles_from_in_memory_sources_for_each_target() {
        for architecture in [Architecture::X86_64, Architecture::Aarch64] {
            let assembly = compiler(architecture)
            .compile(unit("op Nop\nendop\n"))
            .expect("in-memory compilation should succeed");

            assert!(assembly.as_str().contains("js_interpreter"));
            assert!(assembly.as_str().contains("handler_Nop"));
        }
    }

    #[test]
    fn exposes_reusable_compiler_stages() {
        let compiler = compiler(Architecture::X86_64);

        let prepared = compiler.prepare(unit("op Nop\nendop\n")).expect("preparation should succeed");
        let program = compiler.lower(&prepared).expect("machine lowering should succeed");
        let selected = compiler.select(program).expect("selection should succeed");
        assert_eq!(selected.architecture(), Architecture::X86_64);
        let allocated = compiler.allocate(selected).expect("allocation should succeed");
        let machine = compiler.finalize(allocated).expect("finalization should succeed");
        assert_eq!(machine.architecture(), Architecture::X86_64);
        let staged = compiler.emit_program(&machine).expect("emission should succeed");
        assert_eq!(staged, compiler.emit_program(&machine).expect("re-emission should succeed"));
        let one_shot = compiler.compile(unit("op Nop\nendop\n")).expect("one-shot compilation should succeed");
        assert_eq!(staged, one_shot);
    }

    #[test]
    fn resolves_bytecode_dispatch_to_handler_identities() {
        let compiler = compiler(Architecture::X86_64);
        let prepared = compiler
            .prepare(unit("op Nop\nendop\nop Missing\nendop\n"))
            .unwrap();
        let handler_id = prepared.handlers[0].id;

        assert_eq!(
            prepared.bytecode.dispatch_handlers,
            [Some(handler_id), None]
        );
        let lowered = compiler.lower(&prepared).unwrap();
        assert_eq!(lowered.handlers[0].id, handler_id);
        assert_eq!(lowered.dispatch_handlers, [Some(handler_id), None]);
    }

    #[test]
    fn reports_handler_optimization_pipelines() {
        let compiler = compiler(Architecture::X86_64);
        let (_, report) = compiler
            .prepare_with_optimization_report(
                unit("op Nop\nendop\n"),
                OptimizationReportOptions {
                    include_remarks: true,
                    dump_changed_ir: true,
                },
            )
            .expect("reported preparation should succeed");

        assert_eq!(report.pipeline, "aot-prepare");
        assert_eq!(report.functions.len(), 1);
        assert_eq!(report.functions[0].function, "Nop");
        let report = report.to_string();
        assert!(report.contains("pass sparse-conditional-constant-propagation"));
        assert!(report.contains("fixed-point block-parameter-cleanup: iterations=1/2 converged"));
    }

    #[test]
    fn attributes_user_input_errors_to_compiler_stages() {
        let compiler = compiler(Architecture::X86_64);
        let parse_error = compiler
            .prepare(CompilationUnit {
                source_name: "bad.flap",
                source: "handler Broken(",
                constants: None,
                bytecode_def: None,
            })
            .err()
            .expect("invalid source should fail");
        assert_eq!(parse_error.stage, CompileStage::Parse);
        assert!(parse_error.span.is_some());
        assert_eq!(parse_error.handler, None);

        let layout_error = compiler
            .prepare(CompilationUnit {
                source_name: "test.flap",
                source: "handler Nop() { dispatch_next; }",
                constants: Some("field Object.shape Shape MISSING nullable\n"),
                bytecode_def: None,
            })
            .err()
            .expect("invalid layout should fail");
        assert_eq!(layout_error.stage, CompileStage::Layout);
        assert_eq!(layout_error.span.unwrap().start.line, 1);
        assert!(layout_error.message.contains("undefined layout value"));

        let emission_error = compiler
            .compile(CompilationUnit {
                source_name: "test.flap",
                source: "handler Nop() { dispatch_next; }",
                constants: None,
                bytecode_def: Some("op Nop\nendop\n"),
            })
            .expect_err("missing runtime metadata should fail");
        assert_eq!(emission_error.stage, CompileStage::Emission);
        assert_eq!(emission_error.handler, None);
        assert!(
            emission_error
                .message
                .contains("required runtime constant 'INT32_TAG_SHIFTED'")
        );
    }

    #[test]
    fn formats_compiler_stage_and_handler_context() {
        let error = CompileError::new(
            CompileStage::Selection,
            Some("Jump"),
            "cold block produces fallthrough control flow",
        );

        assert_eq!(
            error.to_string(),
            "Selection error in handler 'Jump': cold block produces fallthrough control flow"
        );
    }
}
