/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Queries and transformations over Rust-owned CSS selectors.

use std::sync::Arc;

use super::css_tokenizer::{ParserTokenKind, tokenize_for_parser};
use super::selector::{Combinator, PseudoClassType, PseudoElementType, RustSelector};
use super::selector_arena::{
    PseudoElementValue as ArenaPseudoElementValue, Selector as ArenaSelector, SimpleSelector as ArenaSimpleSelector,
    SimpleView,
};
use super::selector_parser::StyleNestingParent;
use super::selector_tree::{
    CompiledSelector, CompoundSelector, PseudoClassSelector, PseudoElementValue, SelectorList, SimpleSelector,
};

fn pseudo_class(pseudo_class: PseudoClassType, arguments: SelectorList) -> SimpleSelector {
    SimpleSelector::PseudoClass(Box::new(PseudoClassSelector {
        pseudo_class,
        an_plus_b_pattern: Default::default(),
        argument_selector_list: arguments,
        languages: Box::new([]),
        direction: None,
        identifier: None,
        identifier_identity: crate::css::style::index::StyleAtomID::NONE,
        levels: Box::new([]),
        is_forgiving: false,
    }))
}

fn scope_selector() -> Arc<CompiledSelector> {
    CompiledSelector::new(Box::new([CompoundSelector {
        combinator: Combinator::None,
        is_implicit_universal_anchor: false,
        simple_selectors: Box::new([pseudo_class(PseudoClassType::Scope, Box::new([]))]),
    }]))
}

/// `:where(:scope)`, which matches the scoping root and adds no specificity.
pub(crate) fn scope_root_anchor() -> SimpleSelector {
    pseudo_class(PseudoClassType::Where, Box::new([scope_selector()]))
}

pub(crate) fn scope_root_selector_list() -> SelectorList {
    Box::new([CompiledSelector::new(Box::new([CompoundSelector {
        combinator: Combinator::None,
        is_implicit_universal_anchor: false,
        simple_selectors: Box::new([scope_root_anchor()]),
    }]))])
}

fn invalid_selector_names_nesting(source: &[u16]) -> bool {
    tokenize_for_parser(source)
        .iter()
        .any(|token| matches!(token.kind, ParserTokenKind::Delim(value) if value == u32::from(b'&')))
}

pub(crate) fn contains_nesting(selector: &CompiledSelector) -> bool {
    selector.compound_selectors().iter().any(|compound| {
        compound.simple_selectors.iter().any(|simple| match simple {
            SimpleSelector::Nesting => true,
            SimpleSelector::PseudoClass(pseudo_class) => pseudo_class
                .argument_selector_list
                .iter()
                .any(|selector| contains_nesting(selector)),
            SimpleSelector::Invalid(source) => invalid_selector_names_nesting(source.as_ref()),
            _ => false,
        })
    })
}

pub(crate) fn contains_pseudo_class(selector: &CompiledSelector, expected: PseudoClassType) -> bool {
    selector.compound_selectors().iter().any(|compound| {
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
        .compound_selectors()
        .iter()
        .any(|compound| compound.simple_selectors.iter().any(predicate))
}

// https://drafts.csswg.org/css-conditional-4/#support-definition
// A CSS processor is considered to support a CSS selector if it accepts that all aspects of that selector,
// recursively, (rather than considering any of its syntax to be unknown or invalid) and that selector doesn’t
// contain unknown -webkit- pseudo-elements.
pub(crate) fn contains_unsupported_selector(selector: &CompiledSelector) -> bool {
    fn visit(selector: &CompiledSelector, allow_pseudo_elements: bool) -> bool {
        any_simple(selector, &|simple| match simple {
            SimpleSelector::Invalid(..) => true,
            SimpleSelector::PseudoElement(pseudo_element) => {
                !allow_pseudo_elements
                    || pseudo_element.pseudo_element == PseudoElementType::UnknownWebKit
                    || matches!(&pseudo_element.value, PseudoElementValue::CompoundSelector(selector)
                        if visit(selector, false))
            }
            SimpleSelector::PseudoClass(pseudo_class) => pseudo_class
                .argument_selector_list
                .iter()
                .any(|selector| visit(selector, false)),
            _ => false,
        })
    }
    visit(selector, true)
}

fn contains_named_namespace(selector: ArenaSelector<'_>) -> bool {
    selector.compounds().any(|compound| {
        compound.simple_selectors.iter().any(|simple| match simple {
            ArenaSimpleSelector::Universal(name) | ArenaSimpleSelector::TagName(name) => {
                name.namespace_type == super::selector::NamespaceType::Named
            }
            ArenaSimpleSelector::Attribute(attribute) => {
                attribute.qualified_name.namespace_type == super::selector::NamespaceType::Named
            }
            ArenaSimpleSelector::PseudoClass(pseudo_class) => {
                pseudo_class.argument_selector_list.iter().any(contains_named_namespace)
            }
            ArenaSimpleSelector::PseudoElement(pseudo_element) => match pseudo_element.value {
                ArenaPseudoElementValue::CompoundSelector(selector) => contains_named_namespace(selector),
                _ => false,
            },
            _ => false,
        })
    })
}

pub(crate) fn invalid_for_has(selector: &CompiledSelector) -> bool {
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
        return Some(CompiledSelector::new(selector.to_compounds()));
    }
    let mut compounds = selector.to_compounds();
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

pub(crate) fn relative_to(selector: &CompiledSelector, parent: SimpleSelector) -> Arc<CompiledSelector> {
    let mut compounds = Vec::with_capacity(selector.compound_selectors().len() + 1);
    compounds.push(CompoundSelector {
        combinator: Combinator::None,
        is_implicit_universal_anchor: false,
        simple_selectors: Box::new([parent]),
    });
    compounds.extend(
        selector
            .compound_selectors()
            .iter()
            .map(|compound| compound.to_compound()),
    );
    if let Some(second) = compounds.get_mut(1)
        && second.combinator == Combinator::None
    {
        second.combinator = Combinator::Descendant;
    }
    CompiledSelector::new(compounds.into_boxed_slice())
}

/// What a query reads of a selector, from one walk of the selector's arena.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiSelectorQueryFacts {
    /// The pseudo-classes the selector contains, with those of the selectors they take, one bit for the value of each.
    pub pseudo_classes: [u64; 2],
    pub contains_nesting: bool,
    /// Whether every element matches the selector, so that a query over a subtree can collect its elements without
    /// matching any of them.
    pub matches_every_element: bool,
}

fn collect_query_facts(selector: ArenaSelector<'_>, facts: &mut FfiSelectorQueryFacts) {
    for compound in selector.compounds() {
        for view in compound.simple_selectors.views() {
            if let SimpleView::Id(_) | SimpleView::Class(_) | SimpleView::TagName(_) = view {
                continue;
            }
            match view.decode() {
                ArenaSimpleSelector::PseudoClass(pseudo_class) => {
                    let value = pseudo_class.pseudo_class as usize;
                    facts.pseudo_classes[value / u64::BITS as usize] |= 1 << (value % u64::BITS as usize);
                    for argument in pseudo_class.argument_selector_list.iter() {
                        collect_query_facts(argument, facts);
                    }
                }
                ArenaSimpleSelector::Nesting(_) => facts.contains_nesting = true,
                ArenaSimpleSelector::Invalid(source) => {
                    facts.contains_nesting |= invalid_selector_names_nesting(source);
                }
                _ => {}
            }
        }
    }
}

fn query_facts(selector: ArenaSelector<'_>) -> FfiSelectorQueryFacts {
    let mut facts = FfiSelectorQueryFacts::default();
    collect_query_facts(selector, &mut facts);
    let mut compounds = selector.compounds();
    if let (Some(compound), None) = (compounds.next(), compounds.next())
        && compound.simple_selectors.len() == 1
        && let Some(ArenaSimpleSelector::Universal(name)) = compound.simple_selectors.first()
    {
        facts.matches_every_element = matches!(
            name.namespace_type,
            super::selector::NamespaceType::Any | super::selector::NamespaceType::Default
        );
    }
    facts
}

/// # Safety
/// `selector` must point to a live `RustSelector`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_query_facts(selector: *const RustSelector) -> FfiSelectorQueryFacts {
    unsafe {
        assert!(!selector.is_null());
        query_facts((*selector).compiled())
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
                        .compound_selectors()
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
                    .compound_selectors()
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
                .compound_selectors()
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
                let mut compounds = selector.to_compounds();
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
    use super::super::selector_parser::{SelectorType, parse_selector_list_from_component_values};
    use super::contains_named_namespace;

    #[test]
    fn query_facts_agree_with_the_parse_tree() {
        use super::{contains_nesting, contains_pseudo_class, query_facts};
        use crate::css::selector::{NamespaceType, PseudoClassType, SelectorListRef, pseudo_class_from_ffi};
        use crate::css::selector_tree::SimpleSelector;

        let sources = [
            "div",
            "*",
            "*|*",
            "|*",
            "* *",
            "*.a",
            "[data-id=\"item\"]",
            ":hover",
            "a:hover > b:focus ~ c:first-child",
            ":not(:is(a:checked, b:where(:disabled)))",
            ":has(> a:empty, + b:nth-child(2n of :required))",
            ":is(:unknown-forgiven, :root)",
            "a::before:hover",
            "::slotted(a:hover)",
            ":host(:focus-within) a",
            ":lang(en):dir(ltr):state(open)",
            "&",
            "a &",
            ":is(a, :not(& b))",
            ":has(> &)",
            ":is(&!, a)",
            ":is(!, a)",
        ];
        for source in sources {
            let units: Vec<_> = source.encode_utf16().collect();
            let values = consume_a_list_of_component_values(tokenize_for_parser(units.as_slice())).unwrap();
            let parsed = parse_selector_list_from_component_values(&values, &[], SelectorType::Standalone)
                .unwrap_or_else(|_| panic!("{source} parses"));
            let list = SelectorListRef::pack(&parsed.selectors);
            for (tree, packed) in parsed.selectors.iter().zip(list.owned()) {
                let facts = query_facts(packed.view());
                for value in 0..=PseudoClassType::Where as u8 {
                    let expected = contains_pseudo_class(tree, pseudo_class_from_ffi(value));
                    let contained = facts.pseudo_classes[usize::from(value) / 64] & (1 << (value % 64)) != 0;
                    assert_eq!(contained, expected, "{source} with pseudo-class {value}");
                }
                assert_eq!(facts.contains_nesting, contains_nesting(tree), "{source} nesting");
                let every_element = match tree.compound_selectors().as_slice() {
                    [compound] => matches!(
                        compound.simple_selectors,
                        [SimpleSelector::Universal(name)]
                            if matches!(name.namespace_type, NamespaceType::Any | NamespaceType::Default)
                    ),
                    _ => false,
                };
                assert_eq!(facts.matches_every_element, every_element, "{source} every element");
            }
        }
    }

    #[test]
    fn transformed_lists_own_their_results_after_inputs_are_dropped() {
        use super::{absolutize_selector_list, adapt_scope_end_selector_list};
        use crate::css::selector_parser::StyleNestingParent;
        use crate::css::selector_serialization::serialize_selector_without_namespaces;

        // NB: The parsed trees keep their names live, which the results packed from them lease.
        let trees = std::cell::RefCell::new(Vec::new());
        let parse = |source: &str, kind: SelectorType| -> super::SelectorList {
            let units: Vec<_> = source.encode_utf16().collect();
            let values = consume_a_list_of_component_values(tokenize_for_parser(units.as_slice())).unwrap();
            let parsed = parse_selector_list_from_component_values(&values, &[], kind).unwrap();
            let selectors = parsed.selectors.clone();
            trees.borrow_mut().push(parsed);
            selectors
        };
        fn text(selectors: super::SelectorList) -> Vec<Vec<u16>> {
            selectors
                .into_vec()
                .into_iter()
                .map(|selector| {
                    let list = crate::css::selector::SelectorListRef::pack(&[selector]);
                    let selector = super::RustSelector {
                        selector: list.owned().next().unwrap(),
                    };
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

    fn contains_named_namespace_with_namespace_context(source: &str) -> bool {
        let units: Vec<_> = source.encode_utf16().collect();
        let values = consume_a_list_of_component_values(tokenize_for_parser(units.as_slice())).unwrap();
        let parsed = parse_selector_list_from_component_values(
            &values,
            &[TokenizerInput::Latin1(b"foo")],
            SelectorType::Standalone,
        )
        .unwrap();
        // NB: The parsed tree keeps its names live, which the list packed from it leases.
        let list = crate::css::selector::SelectorListRef::pack(&parsed.selectors);
        list.owned().any(|selector| contains_named_namespace(selector.view()))
    }

    #[test]
    fn named_namespace_inside_pseudo_element_compound_selector_is_seen() {
        assert!(contains_named_namespace_with_namespace_context("::slotted(foo|div)"));
    }

    #[test]
    fn named_namespace_inside_pseudo_class_argument_is_seen() {
        assert!(contains_named_namespace_with_namespace_context(":is(foo|div)"));
    }

    #[test]
    fn pseudo_element_compound_selector_without_namespace_is_clean() {
        assert!(!contains_named_namespace_with_namespace_context("::slotted(div.a)"));
    }
}
