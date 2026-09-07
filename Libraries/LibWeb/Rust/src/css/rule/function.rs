/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::NativeRuleType;
use super::read::{NativeRuleView, RuleRef};
use crate::css::container_conditions::ContainerConditionsData;
use crate::css::descriptor_block::DescriptorBlockData;
use crate::css::ffi_support::FfiUtf16View;
use crate::css::function_signature::FunctionSignature;
use std::ffi::c_void;
use std::ops::ControlFlow;
use std::sync::Arc;

struct FunctionDeclarationInput {
    declarations: Arc<DescriptorBlockData>,
    containers: Vec<Arc<ContainerConditionsData>>,
}

// Immutable compilation output. Media changes invalidate the definition cache; container
// conditions remain element-dependent. No live rule, sheet, or descriptor owner is retained.
pub struct CompiledFunction {
    identity: u64,
    signature: Arc<FunctionSignature>,
    inputs: Vec<FunctionDeclarationInput>,
}

impl CompiledFunction {
    fn compile(view: &NativeRuleView<'_>) -> Self {
        fn walk(
            rule: RuleRef<'_>,
            outer_containers: &[Arc<ContainerConditionsData>],
            containers: &mut Vec<Arc<ContainerConditionsData>>,
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
                    });
                    return;
                }
                NativeRuleType::Container => containers.push(rule.container().unwrap().clone()),
                NativeRuleType::Media | NativeRuleType::Supports if rule.cached_condition_holds() => {}
                _ => return,
            }
            let _ = rule.visit_children(&mut |child| {
                walk(child, outer_containers, containers, inputs);
                ControlFlow::Continue(())
            });
            if rule.rule_type() == NativeRuleType::Container {
                containers.pop();
            }
        }
        let mut result = Self {
            identity: view.rule.identity(),
            signature: view.rule.function_signature().unwrap(),
            inputs: Vec::new(),
        };
        let _ = view.rule.visit_children(&mut |child| {
            walk(child, view.containers, &mut Vec::new(), &mut result.inputs);
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

/// # Safety
/// Callbacks must accept the context and borrow their arguments only for each call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_compiled_function_visit_declarations(
    function: &CompiledFunction,
    context: *mut c_void,
    matches_container: unsafe extern "C" fn(*mut c_void, *const *const ContainerConditionsData, usize) -> bool,
    visit: unsafe extern "C" fn(*mut c_void, FfiUtf16View, *const c_void),
) {
    for input in &function.inputs {
        if !input.containers.is_empty() {
            let pointers: Vec<_> = input.containers.iter().map(Arc::as_ptr).collect();
            if !unsafe { matches_container(context, pointers.as_ptr(), pointers.len()) } {
                continue;
            }
        }
        for descriptor in &input.declarations.descriptors {
            let name = descriptor.name.units();
            unsafe {
                visit(
                    context,
                    FfiUtf16View {
                        ascii: std::ptr::null(),
                        utf16: name.as_ptr(),
                        length: name.len(),
                    },
                    Arc::as_ptr(&descriptor.value).cast(),
                );
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::container_conditions::rust_container_conditions_name;
    use crate::css::parser::syntax_parser::parse_shared_stylesheet;
    use crate::css::parser::value_parser::ParseContext;
    use crate::css::rule::RULE_OWNER_ALLOCATIONS;
    use crate::css::serialize::serialize_style_value_to_utf16;
    use crate::css::style_value::StyleValueData;

    #[test]
    fn declarations_preserve_conditions_and_value_lifetimes_without_rule_owners() {
        let owners = RULE_OWNER_ALLOCATIONS.get();
        let units: Vec<_> = "@function --値() { --基: 1px; @media not all { --隠: 2px; }
            @supports (unknown: value) { --無: 3px; } @supports (display: block) { --有: 4px; }
            @container 外 (width > 100px) { --外: 5px; @container 内 (width > 10px) { --内: 6px; } }
            result: var(--基); }"
            .encode_utf16()
            .collect();
        let context: ParseContext = unsafe { std::mem::zeroed() };
        let parsed =
            unsafe { parse_shared_stylesheet(crate::css::css_tokenizer::TokenizerInput::Utf16(&units), &context) };
        let rules = crate::css::rule::NativeRuleList::from_parsed(parsed);
        let mut compiled = None;
        let _ = rules.visit_rules(&mut |rule| {
            compiled = Some(CompiledFunction::compile(&NativeRuleView { rule, containers: &[] }));
            ControlFlow::Continue(())
        });
        let compiled = compiled.unwrap();
        drop(rules);
        #[derive(Default)]
        struct Visited {
            names: Vec<Vec<u16>>,
            values: Vec<Arc<StyleValueData>>,
            containers: Vec<Vec<u16>>,
        }
        unsafe extern "C" fn matches(
            context: *mut c_void,
            conditions: *const *const ContainerConditionsData,
            count: usize,
        ) -> bool {
            let visited = unsafe { &mut *context.cast::<Visited>() };
            let mut matches = true;
            for condition in unsafe { std::slice::from_raw_parts(conditions, count) } {
                let name = unsafe { rust_container_conditions_name(*condition, 0) };
                let name = unsafe { std::slice::from_raw_parts(name.utf16, name.length) };
                matches &= name == "内".encode_utf16().collect::<Vec<_>>();
                visited.containers.push(name.to_vec());
            }
            matches
        }
        unsafe extern "C" fn visit(context: *mut c_void, name: FfiUtf16View, value: *const c_void) {
            let visited = unsafe { &mut *context.cast::<Visited>() };
            visited
                .names
                .push(unsafe { std::slice::from_raw_parts(name.utf16, name.length) }.to_vec());
            let value = value.cast::<StyleValueData>();
            unsafe {
                Arc::increment_strong_count(value);
            }
            visited.values.push(unsafe { Arc::from_raw(value) });
        }
        let mut visited = Visited::default();
        unsafe {
            rust_compiled_function_visit_declarations(&compiled, (&raw mut visited).cast(), matches, visit);
        }
        drop(compiled);
        let strings = |values: &[&str]| {
            values
                .iter()
                .map(|value| value.encode_utf16().collect::<Vec<_>>())
                .collect::<Vec<_>>()
        };
        assert_eq!(visited.names, strings(&["--基", "--有", "result"]));
        assert_eq!(visited.containers, strings(&["外", "内", "外"]));
        assert_eq!(
            visited
                .values
                .iter()
                .map(|value| serialize_style_value_to_utf16(value).unwrap())
                .collect::<Vec<_>>(),
            strings(&["1px", "4px", "var(--基)"])
        );
        assert_eq!(RULE_OWNER_ALLOCATIONS.get(), owners);
    }
}
