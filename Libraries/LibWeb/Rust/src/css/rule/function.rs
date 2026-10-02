/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::NativeRuleType;
use super::read::{NativeRuleView, RuleRef};
use crate::css::container_conditions::ContainerConditionsData;
use crate::css::descriptor_block::DescriptorBlockData;
use crate::css::function_signature::FunctionSignature;
use crate::css::media_list::MediaListData;
use std::ops::ControlFlow;
use std::sync::Arc;

/// One block of a function's declarations, with the conditions that gate it.
pub(crate) struct FunctionDeclarationInput {
    pub(crate) declarations: Arc<DescriptorBlockData>,
    /// The container conditions around the block, nearest first, which hold for an element or not.
    pub(crate) containers: Vec<Arc<ContainerConditionsData>>,
    /// The `@media` lists around the block, which hold for the document or not.
    pub(crate) media: Vec<Arc<MediaListData>>,
}

// Immutable compilation output. Media and container conditions stay inputs, evaluated where the
// function is called. No live rule, sheet, or descriptor owner is retained.
pub struct CompiledFunction {
    pub(crate) identity: u64,
    pub(crate) signature: Arc<FunctionSignature>,
    pub(crate) inputs: Vec<FunctionDeclarationInput>,
}

impl CompiledFunction {
    fn compile(view: &NativeRuleView<'_>) -> Self {
        fn walk(
            rule: RuleRef<'_>,
            outer_containers: &[Arc<ContainerConditionsData>],
            containers: &mut Vec<Arc<ContainerConditionsData>>,
            media: &mut Vec<Arc<MediaListData>>,
            inputs: &mut Vec<FunctionDeclarationInput>,
        ) {
            match rule.rule_type() {
                NativeRuleType::FunctionDeclarations => {
                    // Preserve the declaration walk's nearest-container semantics: enclosing
                    // conditions join the chain when the body enters a container rule.
                    let conditions = if containers.is_empty() {
                        Vec::new()
                    } else {
                        containers
                            .iter()
                            .rev()
                            .chain(outer_containers.iter().rev())
                            .cloned()
                            .collect()
                    };
                    inputs.push(FunctionDeclarationInput {
                        declarations: rule.descriptors().unwrap(),
                        containers: conditions,
                        media: media.clone(),
                    });
                    return;
                }
                NativeRuleType::Container => containers.push(rule.container().unwrap().clone()),
                NativeRuleType::Media => media.push(rule.media_data()),
                NativeRuleType::Supports if rule.cached_condition_holds() => {}
                _ => return,
            }
            let _ = rule.visit_children(&mut |child| {
                walk(child, outer_containers, containers, media, inputs);
                ControlFlow::Continue(())
            });
            match rule.rule_type() {
                NativeRuleType::Container => {
                    containers.pop();
                }
                NativeRuleType::Media => {
                    media.pop();
                }
                _ => {}
            }
        }
        let mut result = Self {
            identity: view.rule.identity(),
            signature: view.rule.function_signature().unwrap(),
            inputs: Vec::new(),
        };
        let _ = view.rule.visit_children(&mut |child| {
            walk(
                child,
                view.containers,
                &mut Vec::new(),
                &mut Vec::new(),
                &mut result.inputs,
            );
            ControlFlow::Continue(())
        });
        result
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_rule_view_compile_function(view: &NativeRuleView<'_>) -> *const CompiledFunction {
    Arc::into_raw(Arc::new(CompiledFunction::compile(view)))
}

/// # Safety
/// The function must be an owned reference returned by compilation or retention.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compiled_function_release(function: *const CompiledFunction) {
    if !function.is_null() {
        drop(unsafe { Arc::from_raw(function) });
    }
}

/// # Safety
/// The function must be live and Arc-owned.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compiled_function_retain(function: *const CompiledFunction) -> *const CompiledFunction {
    unsafe {
        Arc::increment_strong_count(function);
    }
    function
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_compiled_function_identity(function: &CompiledFunction) -> u64 {
    function.identity
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_compiled_function_signature(function: &CompiledFunction) -> *const FunctionSignature {
    Arc::as_ptr(&function.signature)
}

/// The functions a style sheet's top-level `@function` rules compile to, in order, with no rule
/// owner left alive.
#[cfg(test)]
pub(crate) fn compile_functions_for_testing(source: &str) -> Vec<CompiledFunction> {
    let units: Vec<_> = source.encode_utf16().collect();
    let context: crate::css::parser::value_parser::ParseContext = unsafe { std::mem::zeroed() };
    let parsed = unsafe {
        crate::css::parser::syntax_parser::parse_shared_stylesheet(
            crate::css::css_tokenizer::TokenizerInput::Utf16(&units),
            &raw const context,
        )
    };
    let rules = super::NativeRuleList::from_parsed(parsed);
    let mut compiled = Vec::new();
    let _ = rules.visit_rules(&mut |rule| {
        compiled.push(CompiledFunction::compile(&NativeRuleView { rule, containers: &[] }));
        ControlFlow::Continue(())
    });
    compiled
}
