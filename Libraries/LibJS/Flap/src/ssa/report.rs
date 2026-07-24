/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Structured SSA optimization reports and their text formatting.

use std::fmt;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum OptimizationRemarkKind {
    Applied,
    Missed,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct OptimizationRemark {
    pub(crate) kind: OptimizationRemarkKind,
    pub(crate) transformation: String,
    pub(crate) message: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct PassRunReport {
    pub(crate) name: String,
    pub(crate) changed: bool,
    pub(crate) attempted_transformations: u64,
    pub(crate) successful_transformations: u64,
    pub(crate) remarks: Vec<OptimizationRemark>,
    pub(crate) ir_before: Option<String>,
    pub(crate) ir_after: Option<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct FunctionOptimizationReport {
    pub(crate) function: String,
    changed: bool,
    body: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct OptimizationReport {
    pub(crate) pipeline: String,
    pub(crate) functions: Vec<FunctionOptimizationReport>,
}

impl OptimizationReport {
    pub(crate) fn new(pipeline: impl Into<String>, functions: Vec<FunctionOptimizationReport>) -> Self {
        Self {
            pipeline: pipeline.into(),
            functions,
        }
    }

    pub(crate) fn changed_functions(&self) -> impl Iterator<Item = &FunctionOptimizationReport> {
        self.functions.iter().filter(|function| function.changed())
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
            body: String::new(),
        }
    }

    pub(crate) fn changed(&self) -> bool {
        self.changed
    }

    pub(crate) fn push_pass(&mut self, pass: &PassRunReport) {
        self.changed |= pass.changed;
        write_pass_report(&mut self.body, pass, "  ")
            .expect("writing an optimization report to a string cannot fail");
    }

    pub(crate) fn push_fixed_point(
        &mut self,
        name: &str,
        maximum_iterations: usize,
        converged: bool,
        iterations: &[Vec<PassRunReport>],
    ) {
        use std::fmt::Write;
        writeln!(
            self.body,
            "  fixed-point {name}: iterations={}/{} {}",
            iterations.len(),
            maximum_iterations,
            if converged { "converged" } else { "limit-reached" }
        )
        .expect("writing an optimization report to a string cannot fail");
        for (index, iteration) in iterations.iter().enumerate() {
            writeln!(self.body, "    iteration {}:", index + 1)
                .expect("writing an optimization report to a string cannot fail");
            for pass in iteration {
                self.push_pass_with_indent(pass, "      ");
            }
        }
    }

    fn push_pass_with_indent(&mut self, pass: &PassRunReport, indent: &str) {
        self.changed |= pass.changed;
        write_pass_report(&mut self.body, pass, indent)
            .expect("writing an optimization report to a string cannot fail");
    }
}

impl fmt::Display for FunctionOptimizationReport {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        writeln!(
            formatter,
            "handler {}: {}",
            self.function,
            if self.changed() { "changed" } else { "unchanged" }
        )?;
        formatter.write_str(&self.body)
    }
}

fn write_pass_report(formatter: &mut impl fmt::Write, pass: &PassRunReport, indent: &str) -> fmt::Result {
    writeln!(
        formatter,
        "{indent}pass {}: {} attempted={} applied={}",
        pass.name,
        if pass.changed { "changed" } else { "unchanged" },
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
