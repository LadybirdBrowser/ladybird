/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Queries and transformations over Rust-owned CSS selectors.

use std::sync::Arc;

use super::css_tokenizer::{ParserTokenKind, tokenize_for_parser};
use super::selector::{
    Combinator, CompiledSelector, CompoundSelector, PseudoClassSelector, PseudoClassType, PseudoElementType,
    PseudoElementValue, RustSelector, SelectorList, SimpleSelector, pseudo_class_from_ffi,
};
use super::selector_parser::StyleNestingParent;

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

fn scope_selector() -> Arc<CompiledSelector> {
    CompiledSelector::new(Box::new([CompoundSelector {
        combinator: Combinator::None,
        is_implicit_universal_anchor: false,
        simple_selectors: Box::new([pseudo_class(PseudoClassType::Scope, Box::new([]))]),
    }]))
}

pub(crate) fn scope_root_selector_list() -> SelectorList {
    Box::new([CompiledSelector::new(Box::new([CompoundSelector {
        combinator: Combinator::None,
        is_implicit_universal_anchor: false,
        simple_selectors: Box::new([pseudo_class(PseudoClassType::Where, Box::new([scope_selector()]))]),
    }]))])
}

pub(crate) fn contains_nesting<Identity>(selector: &CompiledSelector<Identity>) -> bool {
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

fn any_simple<Identity>(
    selector: &CompiledSelector<Identity>,
    predicate: &impl Fn(&SimpleSelector<Identity>) -> bool,
) -> bool {
    selector
        .compound_selectors
        .iter()
        .any(|compound| compound.simple_selectors.iter().any(predicate))
}

pub(crate) fn contains_unknown_webkit<Identity>(selector: &CompiledSelector<Identity>) -> bool {
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

fn contains_named_namespace<Identity>(selector: &CompiledSelector<Identity>) -> bool {
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

fn absolutize(selector: &CompiledSelector, replacement: &SimpleSelector) -> Option<Arc<CompiledSelector>> {
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

pub(crate) fn relative_to<Identity: Clone>(
    selector: &CompiledSelector<Identity>,
    parent: SimpleSelector<Identity>,
) -> Arc<CompiledSelector<Identity>> {
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
        assert!(!selector.is_null());
        supports_simple_dom_matching((*selector).compiled())
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
        assert!(!selector.is_null());
        matches_every_element((*selector).compiled())
    }
}

/// # Safety
/// `selector` must point to a live `RustSelector`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_contains_nesting(selector: *const RustSelector) -> bool {
    unsafe {
        assert!(!selector.is_null());
        contains_nesting((*selector).compiled())
    }
}

/// # Safety
/// `selector` must point to a live `RustSelector`, and `value` must identify a pseudo-class.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_contains_pseudo_class(selector: *const RustSelector, value: u8) -> bool {
    unsafe {
        assert!(!selector.is_null());
        contains_pseudo_class((*selector).compiled(), pseudo_class_from_ffi(value))
    }
}

/// # Safety
/// `selector` must point to a live `RustSelector`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_contains_named_namespace(selector: *const RustSelector) -> bool {
    unsafe {
        assert!(!selector.is_null());
        contains_named_namespace((*selector).compiled())
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
    }
}

/// # Safety
/// `selector` must point to a live `RustSelector`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_first_combinator(selector: *const RustSelector) -> Combinator {
    unsafe {
        assert!(!selector.is_null());
        (*selector)
            .compiled()
            .compound_selectors
            .first()
            .map_or(Combinator::None, |compound| compound.combinator)
    }
}

pub(crate) fn absolutize_selector_list(
    selectors: &SelectorList,
    parent: StyleNestingParent,
    parents: &SelectorList,
) -> Option<SelectorList> {
    if !selectors.iter().any(|selector| {
        contains_nesting(selector)
            || (parent == StyleNestingParent::Scope
                && !matches!(
                    selector
                        .compound_selectors
                        .first()
                        .map_or(Combinator::None, |compound| compound.combinator),
                    Combinator::None | Combinator::Descendant
                ))
    }) {
        return None;
    }
    let replacement = if parent == StyleNestingParent::Style {
        pseudo_class(PseudoClassType::Is, parents.clone())
    } else {
        pseudo_class(PseudoClassType::Where, Box::new([scope_selector()]))
    };
    Some(
        selectors
            .iter()
            .filter_map(|selector| {
                let first = selector
                    .compound_selectors
                    .first()
                    .map_or(Combinator::None, |compound| compound.combinator);
                if contains_nesting(selector) {
                    absolutize(selector, &replacement)
                } else if parent == StyleNestingParent::Scope
                    && !matches!(first, Combinator::None | Combinator::Descendant)
                {
                    Some(relative_to(selector, replacement.clone()))
                } else {
                    Some(selector.clone())
                }
            })
            .collect(),
    )
}

pub(crate) fn adapt_scope_end_selector_list(selectors: &SelectorList) -> SelectorList {
    selectors
        .iter()
        .map(|selector| {
            let first = selector
                .compound_selectors
                .first()
                .map_or(Combinator::None, |compound| compound.combinator);
            if !matches!(first, Combinator::None | Combinator::Descendant)
                || (!contains_nesting(selector) && !contains_pseudo_class(selector, PseudoClassType::Scope))
            {
                relative_to(
                    selector,
                    pseudo_class(PseudoClassType::Where, Box::new([scope_selector()])),
                )
            } else if first == Combinator::Descendant {
                let mut compounds = selector.compound_selectors.clone();
                compounds[0].combinator = Combinator::None;
                CompiledSelector::new(compounds)
            } else {
                selector.clone()
            }
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::super::css_tokenizer::{TokenizerInput, tokenize_for_parser};
    use super::super::parser::component_value::consume_a_list_of_component_values;
    use super::super::selector::parsed::SelectorList;
    use super::super::selector_parser::{SelectorType, parse_selector_list_from_component_values};
    use super::contains_named_namespace;

    #[test]
    fn transformed_lists_own_their_results_after_inputs_are_dropped() {
        use super::{absolutize_selector_list, adapt_scope_end_selector_list};
        use crate::css::selector_parser::StyleNestingParent;
        use crate::css::selector_serialization::serialize_selector_without_namespaces;

        fn parse(source: &str, kind: SelectorType) -> super::SelectorList {
            let units: Vec<_> = source.encode_utf16().collect();
            let values = consume_a_list_of_component_values(tokenize_for_parser(units.as_slice())).unwrap();
            let parsed = parse_selector_list_from_component_values(&values, &[], kind).unwrap();
            unsafe { parsed.bind() }.selectors
        }
        fn text(selectors: super::SelectorList) -> Vec<Vec<u16>> {
            selectors
                .into_vec()
                .into_iter()
                .map(|selector| {
                    let selector = super::RustSelector { selector };
                    serialize_selector_without_namespaces(&selector)
                })
                .collect()
        }
        let utf16 = |source: &str| source.encode_utf16().collect::<Vec<_>>();
        let empty: super::SelectorList = Box::new([]);
        let parents = parse(".親, #id", SelectorType::Standalone);
        let selectors = parse(".plain, & > .子", SelectorType::Standalone);
        let result = absolutize_selector_list(&selectors, StyleNestingParent::Style, &parents).unwrap();
        drop((parents, selectors));
        assert_eq!(text(result), [".plain", ":is(.親, #id) > .子"].map(utf16));

        let selectors = parse("> .子, :scope .lim, .plain", SelectorType::Relative);
        let result = adapt_scope_end_selector_list(&selectors);
        drop(selectors);
        assert_eq!(
            text(result),
            [":where(:scope) > .子", ":scope .lim", ":where(:scope) .plain"].map(utf16)
        );
        assert!(
            absolutize_selector_list(
                &parse(".plain", SelectorType::Standalone),
                StyleNestingParent::None,
                &empty
            )
            .is_none()
        );

        let selectors = parse("&", SelectorType::Standalone);
        let result = absolutize_selector_list(&selectors, StyleNestingParent::Style, &empty).unwrap();
        drop(selectors);
        assert_eq!(text(result), [utf16(":is()")]);
        let selectors = parse("> .子, & .plain", SelectorType::Relative);
        let result = absolutize_selector_list(&selectors, StyleNestingParent::Scope, &empty).unwrap();
        drop(selectors);
        assert_eq!(
            text(result),
            [":where(:scope) > .子", ":where(:scope) .plain"].map(utf16)
        );
        let parents = parse(":has(.親)", SelectorType::Standalone);
        let selectors = parse(":has(&)", SelectorType::Standalone);
        let result = absolutize_selector_list(&selectors, StyleNestingParent::Style, &parents).unwrap();
        drop((parents, selectors));
        assert!(result.is_empty());
    }

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
