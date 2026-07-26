/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Structured SSA optimization reports and their text formatting.

use std::fmt;
use std::time::Duration;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OptimizationRemarkKind {
    Applied,
    Missed,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OptimizationRemark {
    pub kind: OptimizationRemarkKind,
    pub transformation: String,
    pub message: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PassRunReport {
    pub name: String,
    pub elapsed: Duration,
    pub changed: bool,
    pub attempted_transformations: u64,
    pub successful_transformations: u64,
    pub remarks: Vec<OptimizationRemark>,
    pub ir_before: Option<String>,
    pub ir_after: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FixedPointOptimizationReport {
    pub name: String,
    pub maximum_iterations: usize,
    pub converged: bool,
    pub iterations: Vec<Vec<PassRunReport>>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum OptimizationRecord {
    Pass(PassRunReport),
    FixedPoint(FixedPointOptimizationReport),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FunctionOptimizationReport {
    pub function: String,
    pub changed: bool,
    pub records: Vec<OptimizationRecord>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OptimizationReport {
    pub pipeline: String,
    pub functions: Vec<FunctionOptimizationReport>,
}

impl OptimizationReport {
    pub(crate) fn new(pipeline: impl Into<String>, functions: Vec<FunctionOptimizationReport>) -> Self {
        Self {
            pipeline: pipeline.into(),
            functions,
        }
    }

    pub fn changed_functions(&self) -> impl Iterator<Item = &FunctionOptimizationReport> {
        self.functions.iter().filter(|function| function.changed)
    }
}

impl fmt::Display for OptimizationReport {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        let changed = self.changed_functions().count();
        writeln!(
            formatter,
            "pipeline {}: handlers={} changed={}",
            self.pipeline,
            self.functions.len(),
            changed
        )?;
        for function in &self.functions {
            writeln!(formatter)?;
            write!(formatter, "{function}")?;
        }
        Ok(())
    }
}

impl FunctionOptimizationReport {
    pub(crate) fn new(function: impl Into<String>) -> Self {
        Self {
            function: function.into(),
            changed: false,
            records: Vec::new(),
        }
    }

    pub(crate) fn push_pass(&mut self, pass: &PassRunReport) {
        self.changed |= pass.changed;
        self.records.push(OptimizationRecord::Pass(pass.clone()));
    }

    pub(crate) fn push_fixed_point(
        &mut self,
        name: &str,
        maximum_iterations: usize,
        converged: bool,
        iterations: &[Vec<PassRunReport>],
    ) {
        self.changed |= iterations.iter().flatten().any(|pass| pass.changed);
        self.records
            .push(OptimizationRecord::FixedPoint(FixedPointOptimizationReport {
                name: name.to_string(),
                maximum_iterations,
                converged,
                iterations: iterations.to_vec(),
            }));
    }
}

impl fmt::Display for FunctionOptimizationReport {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        writeln!(
            formatter,
            "handler {}: {}",
            self.function,
            if self.changed { "changed" } else { "unchanged" }
        )?;
        for record in &self.records {
            match record {
                OptimizationRecord::Pass(pass) => write_pass_report(formatter, pass, "  ")?,
                OptimizationRecord::FixedPoint(fixed_point) => {
                    writeln!(
                        formatter,
                        "  fixed-point {}: iterations={}/{} {}",
                        fixed_point.name,
                        fixed_point.iterations.len(),
                        fixed_point.maximum_iterations,
                        if fixed_point.converged {
                            "converged"
                        } else {
                            "limit-reached"
                        }
                    )?;
                    for (index, iteration) in fixed_point.iterations.iter().enumerate() {
                        writeln!(formatter, "    iteration {}:", index + 1)?;
                        for pass in iteration {
                            write_pass_report(formatter, pass, "      ")?;
                        }
                    }
                }
            }
        }
        Ok(())
    }
}

fn write_pass_report(formatter: &mut impl fmt::Write, pass: &PassRunReport, indent: &str) -> fmt::Result {
    writeln!(
        formatter,
        "{indent}pass {}: {} time-us={} attempted={} applied={}",
        pass.name,
        if pass.changed { "changed" } else { "unchanged" },
        pass.elapsed.as_micros(),
        pass.attempted_transformations,
        pass.successful_transformations
    )?;
    for remark in &pass.remarks {
        writeln!(
            formatter,
            "{indent}  {} {}: {}",
            match remark.kind {
                OptimizationRemarkKind::Applied => "applied",
                OptimizationRemarkKind::Missed => "missed",
            },
            remark.transformation,
            remark.message
        )?;
    }
    if let (Some(before), Some(after)) = (&pass.ir_before, &pass.ir_after) {
        writeln!(formatter, "{indent}  IR before:")?;
        write_indented(formatter, before, &format!("{indent}    "))?;
        writeln!(formatter, "{indent}  IR after:")?;
        write_indented(formatter, after, &format!("{indent}    "))?;
    }
    Ok(())
}

fn write_indented(formatter: &mut impl fmt::Write, text: &str, indent: &str) -> fmt::Result {
    for line in text.lines() {
        writeln!(formatter, "{indent}{line}")?;
    }
    Ok(())
}
