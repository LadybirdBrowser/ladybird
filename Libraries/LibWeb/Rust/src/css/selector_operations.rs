/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Queries and transformations over Rust-owned CSS selectors.

use std::rc::Rc;

use super::css_tokenizer::{ParserTokenKind, tokenize_for_parser};
use super::selector::{
    Combinator, CompiledSelector, CompoundSelector, PseudoClassSelector, PseudoClassType, PseudoElementType,
    PseudoElementValue, RustSelector, SelectorList, SimpleSelector, pseudo_class_from_ffi,
};

fn pseudo_class(pseudo_class: PseudoClassType, arguments: SelectorList) -> SimpleSelector {
    SimpleSelector::PseudoClass(PseudoClassSelector {
        pseudo_class,
        an_plus_b_pattern: Default::default(),
        argument_selector_list: arguments,
        languages: Box::new([]),
        direction: None,
        identifier: None,
        identifier_identity: None,
        identifier_lowercase_identity: None,
        levels: Box::new([]),
        is_forgiving: false,
    })
}

fn scope_selector() -> Rc<CompiledSelector> {
    CompiledSelector::new(Box::new([CompoundSelector {
        combinator: Combinator::None,
        is_implicit_universal_anchor: false,
        simple_selectors: Box::new([pseudo_class(PseudoClassType::Scope, Box::new([]))]),
    }]))
}

fn contains_nesting(selector: &CompiledSelector) -> bool {
    selector.compound_selectors.iter().any(|compound| {
        compound.simple_selectors.iter().any(|simple| match simple {
            SimpleSelector::Nesting => true,
            SimpleSelector::PseudoClass(pseudo_class) => pseudo_class
                .argument_selector_list
                .iter()
                .any(|selector| contains_nesting(selector)),
            SimpleSelector::Invalid(source) => tokenize_for_parser(source.as_ref())
                .iter()
                .any(|token| matches!(token.kind, ParserTokenKind::Delim(value) if value == u32::from(b'&'))),
            _ => false,
        })
    })
}

fn contains_pseudo_class(selector: &CompiledSelector, expected: PseudoClassType) -> bool {
    selector.compound_selectors.iter().any(|compound| {
        compound.simple_selectors.iter().any(|simple| {
            let SimpleSelector::PseudoClass(pseudo_class) = simple else {
                return false;
            };
            pseudo_class.pseudo_class == expected
                || pseudo_class
                    .argument_selector_list
                    .iter()
                    .any(|selector| contains_pseudo_class(selector, expected))
        })
    })
}

fn any_simple(selector: &CompiledSelector, predicate: &impl Fn(&SimpleSelector) -> bool) -> bool {
    selector
        .compound_selectors
        .iter()
        .any(|compound| compound.simple_selectors.iter().any(predicate))
}

pub(crate) fn contains_unknown_webkit(selector: &CompiledSelector) -> bool {
    any_simple(selector, &|simple| match simple {
        SimpleSelector::PseudoElement(pseudo_element) => {
            pseudo_element.pseudo_element == PseudoElementType::UnknownWebKit
        }
        SimpleSelector::PseudoClass(pseudo_class) => pseudo_class
            .argument_selector_list
            .iter()
            .any(|selector| contains_unknown_webkit(selector)),
        _ => false,
    })
}

fn contains_named_namespace(selector: &CompiledSelector) -> bool {
    any_simple(selector, &|simple| match simple {
        SimpleSelector::Universal(name) | SimpleSelector::TagName(name) => {
            name.namespace_type == super::selector::NamespaceType::Named
        }
        SimpleSelector::Attribute(attribute) => {
            attribute.qualified_name.namespace_type == super::selector::NamespaceType::Named
        }
        SimpleSelector::PseudoClass(pseudo_class) => pseudo_class
            .argument_selector_list
            .iter()
            .any(|selector| contains_named_namespace(selector)),
        SimpleSelector::PseudoElement(pseudo_element) => match &pseudo_element.value {
            PseudoElementValue::CompoundSelector(selector) => contains_named_namespace(selector),
            _ => false,
        },
        _ => false,
    })
}

fn invalid_for_has(selector: &CompiledSelector) -> bool {
    any_simple(selector, &|simple| match simple {
        SimpleSelector::PseudoElement(_) => true,
        SimpleSelector::PseudoClass(pseudo_class) => {
            pseudo_class.pseudo_class == PseudoClassType::Has
                || pseudo_class
                    .argument_selector_list
                    .iter()
                    .any(|selector| invalid_for_has(selector))
        }
        _ => false,
    })
}

fn absolutize(selector: &CompiledSelector, replacement: &SimpleSelector) -> Option<Rc<CompiledSelector>> {
    if !contains_nesting(selector) {
        return Some(CompiledSelector::new(selector.compound_selectors.clone()));
    }
    let mut compounds = selector.compound_selectors.clone();
    for compound in &mut compounds {
        let mut simples = Vec::with_capacity(compound.simple_selectors.len());
        for simple in &compound.simple_selectors {
            match simple {
                SimpleSelector::Nesting => simples.push(replacement.clone()),
                SimpleSelector::PseudoClass(pseudo_class) => {
                    let mut pseudo_class = pseudo_class.clone();
                    let mut arguments = Vec::with_capacity(pseudo_class.argument_selector_list.len());
                    for argument in &pseudo_class.argument_selector_list {
                        match absolutize(argument, replacement) {
                            Some(argument) => arguments.push(argument),
                            None if pseudo_class.is_forgiving => {}
                            None => return None,
                        }
                    }
                    pseudo_class.argument_selector_list = arguments.into_boxed_slice();
                    if pseudo_class.pseudo_class == PseudoClassType::Has
                        && pseudo_class
                            .argument_selector_list
                            .iter()
                            .any(|selector| invalid_for_has(selector))
                    {
                        return None;
                    }
                    simples.push(SimpleSelector::PseudoClass(pseudo_class));
                }
                _ => simples.push(simple.clone()),
            }
        }
        compound.simple_selectors = simples.into_boxed_slice();
    }
    Some(CompiledSelector::new(compounds))
}

fn relative_to(selector: &CompiledSelector, parent: SimpleSelector) -> Rc<CompiledSelector> {
    let mut compounds = Vec::with_capacity(selector.compound_selectors.len() + 1);
    compounds.push(CompoundSelector {
        combinator: Combinator::None,
        is_implicit_universal_anchor: false,
        simple_selectors: Box::new([parent]),
    });
    compounds.extend(selector.compound_selectors.iter().cloned());
    if let Some(second) = compounds.get_mut(1)
        && second.combinator == Combinator::None
    {
        second.combinator = Combinator::Descendant;
    }
    CompiledSelector::new(compounds.into_boxed_slice())
}

fn supports_simple_dom_matching(selector: &CompiledSelector) -> bool {
    let [compound] = selector.compound_selectors.as_ref() else {
        return false;
    };
    !compound.simple_selectors.is_empty()
        && compound.simple_selectors.iter().all(|simple| match simple {
            SimpleSelector::Universal(name) | SimpleSelector::TagName(name) => matches!(
                name.namespace_type,
                super::selector::NamespaceType::Any | super::selector::NamespaceType::Default
            ),
            SimpleSelector::Id(_) | SimpleSelector::Class(_) => true,
            _ => false,
        })
}

/// Returns whether a selector can be matched by `rust_selector_matches_simple_dom`.
///
/// # Safety
/// `selector` must point to a live `RustSelector`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_supports_simple_dom_matching(selector: *const RustSelector) -> bool {
    unsafe {
        crate::abort_on_panic(|| {
            assert!(!selector.is_null());
            supports_simple_dom_matching((*selector).compiled())
        })
    }
}

/// Whether a selector is a lone universal selector, which every element matches. A query API has
/// no default namespace, so `*` and `*|*` name the same elements there.
fn matches_every_element(selector: &CompiledSelector) -> bool {
    let [compound] = selector.compound_selectors.as_ref() else {
        return false;
    };
    let [SimpleSelector::Universal(name)] = compound.simple_selectors.as_ref() else {
        return false;
    };
    matches!(
        name.namespace_type,
        super::selector::NamespaceType::Any | super::selector::NamespaceType::Default
    )
}

/// Returns whether every element matches the selector, so that a query over a subtree can collect
/// its elements without matching any of them.
///
/// # Safety
/// `selector` must point to a live `RustSelector`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_matches_every_element(selector: *const RustSelector) -> bool {
    unsafe {
        crate::abort_on_panic(|| {
            assert!(!selector.is_null());
            matches_every_element((*selector).compiled())
        })
    }
}

unsafe fn replacement(
    use_parent_selectors: bool,
    parent_selectors: *const *const RustSelector,
    count: usize,
) -> SimpleSelector {
    if !use_parent_selectors {
        return pseudo_class(PseudoClassType::Where, Box::new([scope_selector()]));
    }
    let handles = if count == 0 {
        &[]
    } else {
        assert!(!parent_selectors.is_null());
        unsafe { std::slice::from_raw_parts(parent_selectors, count) }
    };
    pseudo_class(
        PseudoClassType::Is,
        handles
            .iter()
            .map(|handle| {
                assert!(!handle.is_null());
                unsafe { (**handle).selector.clone() }
            })
            .collect::<Vec<_>>()
            .into_boxed_slice(),
    )
}

/// # Safety
/// `selector` must point to a live `RustSelector`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_contains_nesting(selector: *const RustSelector) -> bool {
    unsafe {
        crate::abort_on_panic(|| {
            assert!(!selector.is_null());
            contains_nesting((*selector).compiled())
        })
    }
}

/// # Safety
/// `selector` must point to a live `RustSelector`, and `value` must identify a pseudo-class.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_contains_pseudo_class(selector: *const RustSelector, value: u8) -> bool {
    unsafe {
        crate::abort_on_panic(|| {
            assert!(!selector.is_null());
            contains_pseudo_class((*selector).compiled(), pseudo_class_from_ffi(value))
        })
    }
}

/// # Safety
/// `selector` must point to a live `RustSelector`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_contains_named_namespace(selector: *const RustSelector) -> bool {
    unsafe {
        crate::abort_on_panic(|| {
            assert!(!selector.is_null());
            contains_named_namespace((*selector).compiled())
        })
    }
}

/// Matches the tag, ID, and class subset directly against interned DOM name identities.
///
/// Returns zero or one for a supported selector and `u8::MAX` for any selector which needs the
/// full style-engine matcher.
///
/// # Safety
/// `selector` must point to a live `RustSelector`. The class pointers must each address
/// `class_count` identities, or be null when the count is zero.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_matches_simple_dom(
    selector: *const RustSelector,
    tag_name: usize,
    lowercase_tag_name: usize,
    id: usize,
    lowercase_id: usize,
    classes: *const usize,
    lowercase_classes: *const usize,
    class_count: usize,
    fold_tag_name: bool,
    fold_id_and_classes: bool,
) -> u8 {
    unsafe {
        crate::abort_on_panic(|| {
            assert!(!selector.is_null());
            let compounds = &(*selector).compiled().compound_selectors;
            if !supports_simple_dom_matching((*selector).compiled()) {
                return u8::MAX;
            }
            let compound = &compounds[0];
            let classes = if class_count == 0 {
                &[][..]
            } else {
                if classes.is_null() {
                    return u8::MAX;
                }
                std::slice::from_raw_parts(classes, class_count)
            };
            let lowercase_classes = if !fold_id_and_classes || class_count == 0 {
                &[][..]
            } else {
                if lowercase_classes.is_null() {
                    return u8::MAX;
                }
                std::slice::from_raw_parts(lowercase_classes, class_count)
            };
            for simple in &compound.simple_selectors {
                let matches = match simple {
                    SimpleSelector::Universal(name)
                        if matches!(
                            name.namespace_type,
                            super::selector::NamespaceType::Any | super::selector::NamespaceType::Default
                        ) =>
                    {
                        true
                    }
                    SimpleSelector::TagName(name)
                        if matches!(
                            name.namespace_type,
                            super::selector::NamespaceType::Any | super::selector::NamespaceType::Default
                        ) =>
                    {
                        if fold_tag_name {
                            name.interned_lowercase_name_identity() == Some(lowercase_tag_name)
                        } else {
                            name.interned_name_identity() == Some(tag_name)
                        }
                    }
                    SimpleSelector::Id(name) => {
                        if fold_id_and_classes {
                            name.interned_lowercase_name_identity() == Some(lowercase_id)
                        } else {
                            name.interned_name_identity() == Some(id)
                        }
                    }
                    SimpleSelector::Class(name) => {
                        let (expected, candidates) = if fold_id_and_classes {
                            (name.interned_lowercase_name_identity(), lowercase_classes)
                        } else {
                            (name.interned_name_identity(), classes)
                        };
                        expected.is_some_and(|expected| candidates.contains(&expected))
                    }
                    _ => unreachable!(),
                };
                if !matches {
                    return 0;
                }
            }
            1
        })
    }
}

/// # Safety
/// `selector` must point to a live `RustSelector`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_first_combinator(selector: *const RustSelector) -> Combinator {
    unsafe {
        crate::abort_on_panic(|| {
            assert!(!selector.is_null());
            (*selector)
                .compiled()
                .compound_selectors
                .first()
                .map_or(Combinator::None, |compound| compound.combinator)
        })
    }
}

/// # Safety
/// `selector` must point to a live `RustSelector`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_with_first_combinator_none(selector: *const RustSelector) -> *mut RustSelector {
    unsafe {
        crate::abort_on_panic(|| {
            assert!(!selector.is_null());
            let mut compounds = (*selector).compiled().compound_selectors.clone();
            if let Some(first) = compounds.first_mut() {
                first.combinator = Combinator::None;
            }
            Box::into_raw(Box::new(RustSelector {
                selector: CompiledSelector::new(compounds),
            }))
        })
    }
}

/// # Safety
/// `selector` must point to a live `RustSelector`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_relative_to_nesting(selector: *const RustSelector) -> *mut RustSelector {
    unsafe {
        crate::abort_on_panic(|| {
            assert!(!selector.is_null());
            Box::into_raw(Box::new(RustSelector {
                selector: relative_to((*selector).compiled(), SimpleSelector::Nesting),
            }))
        })
    }
}

/// # Safety
/// `selector` must point to a live `RustSelector`. When `use_parent_selectors` is true, `parents`
/// must address `count` live selector pointers, or be null when `count` is zero.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_relative_to_scope(
    selector: *const RustSelector,
    use_parent_selectors: bool,
    parents: *const *const RustSelector,
    count: usize,
) -> *mut RustSelector {
    unsafe {
        crate::abort_on_panic(|| {
            assert!(!selector.is_null());
            let parent = replacement(use_parent_selectors, parents, count);
            Box::into_raw(Box::new(RustSelector {
                selector: relative_to((*selector).compiled(), parent),
            }))
        })
    }
}

/// # Safety
/// `selector` must point to a live `RustSelector`. When `use_parent_selectors` is true, `parents`
/// must address `count` live selector pointers, or be null when `count` is zero.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_absolutize(
    selector: *const RustSelector,
    use_parent_selectors: bool,
    parents: *const *const RustSelector,
    count: usize,
) -> *mut RustSelector {
    unsafe {
        crate::abort_on_panic(|| {
            assert!(!selector.is_null());
            let replacement = replacement(use_parent_selectors, parents, count);
            absolutize((*selector).compiled(), &replacement)
                .map(|selector| Box::into_raw(Box::new(RustSelector { selector })))
                .unwrap_or(std::ptr::null_mut())
        })
    }
}

#[cfg(test)]
mod tests {
    use super::super::css_tokenizer::{TokenizerInput, tokenize_for_parser};
    use super::super::parser::component_value::consume_a_list_of_component_values;
    use super::super::selector::SelectorList;
    use super::super::selector_parser::{SelectorType, parse_selector_list_from_component_values};
    use super::contains_named_namespace;

    fn parse_with_namespace_context(source: &str) -> SelectorList {
        let values = consume_a_list_of_component_values(tokenize_for_parser(source.as_bytes())).unwrap();
        parse_selector_list_from_component_values(&values, &[TokenizerInput::Ascii(b"foo")], SelectorType::Standalone)
            .unwrap()
            .selectors()
            .clone()
    }

    #[test]
    fn named_namespace_inside_pseudo_element_compound_selector_is_seen() {
        let selectors = parse_with_namespace_context("::slotted(foo|div)");
        assert!(selectors.iter().all(|selector| contains_named_namespace(selector)));
    }

    #[test]
    fn named_namespace_inside_pseudo_class_argument_is_seen() {
        let selectors = parse_with_namespace_context(":is(foo|div)");
        assert!(selectors.iter().all(|selector| contains_named_namespace(selector)));
    }

    #[test]
    fn pseudo_element_compound_selector_without_namespace_is_clean() {
        let selectors = parse_with_namespace_context("::slotted(div.a)");
        assert!(!selectors.iter().any(|selector| contains_named_namespace(selector)));
    }
}
