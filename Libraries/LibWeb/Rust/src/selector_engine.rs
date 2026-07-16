/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};

use crate::abort_on_panic;

static NEXT_SELECTOR_ID: AtomicU64 = AtomicU64::new(1);

pub type SelectorString = Box<[u16]>;
pub type SelectorList = Box<[Arc<CompiledSelector>]>;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Combinator {
    None,
    ImmediateChild,
    Descendant,
    NextSibling,
    SubsequentSibling,
    Column,
    PseudoElement,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum NamespaceType {
    Default,
    None,
    Any,
    Named,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct QualifiedName {
    pub namespace_type: NamespaceType,
    pub namespace: SelectorString,
    pub name: SelectorString,
    pub lowercase_name: SelectorString,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AttributeMatchType {
    HasAttribute,
    ExactValue,
    ContainsWord,
    ContainsString,
    StartsWithSegment,
    StartsWithString,
    EndsWithString,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AttributeCaseType {
    Default,
    Sensitive,
    Insensitive,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AttributeSelector {
    pub match_type: AttributeMatchType,
    pub qualified_name: QualifiedName,
    pub value: SelectorString,
    pub case_type: AttributeCaseType,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AnPlusBPattern {
    pub step_size: i32,
    pub offset: i32,
}

impl AnPlusBPattern {
    pub fn matches(self, index: i32) -> bool {
        if self.step_size == 0 {
            return index == self.offset;
        }
        let delta = i64::from(index) - i64::from(self.offset);
        let step_size = i64::from(self.step_size);
        delta % step_size == 0 && delta / step_size >= 0
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Direction {
    LeftToRight,
    RightToLeft,
    Other,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum PseudoClassType {
    Active,
    AnyLink,
    Autofill,
    Buffering,
    Checked,
    Default,
    Defined,
    Dir,
    Disabled,
    Empty,
    Enabled,
    EvenLessGoodValue,
    FirstChild,
    FirstOfType,
    Focus,
    FocusVisible,
    FocusWithin,
    Fullscreen,
    Has,
    Heading,
    HighValue,
    Host,
    Hover,
    Indeterminate,
    Invalid,
    Is,
    Lang,
    LastChild,
    LastOfType,
    Link,
    LocalLink,
    LowValue,
    Modal,
    Muted,
    Not,
    NthChild,
    NthLastChild,
    NthLastOfType,
    NthOfType,
    OnlyChild,
    OnlyOfType,
    Open,
    OptimalValue,
    Optional,
    PopoverOpen,
    Paused,
    PlaceholderShown,
    Playing,
    ReadOnly,
    ReadWrite,
    Required,
    Root,
    Scope,
    Seeking,
    Stalled,
    State,
    SuboptimalValue,
    Target,
    Unchecked,
    UserInvalid,
    UserValid,
    Valid,
    Visited,
    VolumeLocked,
    Where,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PseudoClassSelector {
    pub pseudo_class: PseudoClassType,
    pub an_plus_b_pattern: AnPlusBPattern,
    pub argument_selector_list: SelectorList,
    pub languages: Box<[SelectorString]>,
    pub direction: Option<Direction>,
    pub identifier: Option<SelectorString>,
    pub levels: Box<[i64]>,
}

impl PseudoClassSelector {
    #[cfg(test)]
    fn without_arguments(pseudo_class: PseudoClassType) -> Self {
        Self {
            pseudo_class,
            an_plus_b_pattern: AnPlusBPattern::default(),
            argument_selector_list: Box::new([]),
            languages: Box::new([]),
            direction: None,
            identifier: None,
            levels: Box::new([]),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum PseudoElementType {
    After,
    Backdrop,
    Before,
    FirstLetter,
    FirstLine,
    Marker,
    Selection,
    ViewTransition,
    DetailsContent,
    FileSelectorButton,
    Placeholder,
    SliderFill,
    SliderThumb,
    SliderTrack,
    Part,
    Slotted,
    ViewTransitionGroup,
    ViewTransitionImagePair,
    ViewTransitionNew,
    ViewTransitionOld,
    UnknownWebKit,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PseudoElementValue {
    None,
    CompoundSelector(Arc<CompiledSelector>),
    Identifiers(Box<[SelectorString]>),
    TransitionName { is_universal: bool, value: SelectorString },
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PseudoElementSelector {
    pub pseudo_element: PseudoElementType,
    pub value: PseudoElementValue,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SimpleSelector {
    Universal(QualifiedName),
    TagName(QualifiedName),
    Id(SelectorString),
    Class(SelectorString),
    Attribute(AttributeSelector),
    PseudoClass(PseudoClassSelector),
    PseudoElement(PseudoElementSelector),
    Nesting,
    Invalid,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CompoundSelector {
    pub combinator: Combinator,
    pub simple_selectors: Box<[SimpleSelector]>,
}

#[derive(Debug)]
pub struct CompiledSelector {
    id: u64,
    cxx_selector: usize,
    pub compound_selectors: Box<[CompoundSelector]>,
    pub target_pseudo_element: Option<PseudoElementType>,
    pub can_use_fast_matches: bool,
    pub sibling_invalidation_distance: usize,
}

impl PartialEq for CompiledSelector {
    fn eq(&self, other: &Self) -> bool {
        self.compound_selectors == other.compound_selectors
    }
}

impl Eq for CompiledSelector {}

impl CompiledSelector {
    pub fn new(compound_selectors: Box<[CompoundSelector]>) -> Arc<Self> {
        Self::new_with_cxx_selector(compound_selectors, std::ptr::null())
    }

    fn new_with_cxx_selector(compound_selectors: Box<[CompoundSelector]>, cxx_selector: *const c_void) -> Arc<Self> {
        let id = NEXT_SELECTOR_ID.fetch_add(1, Ordering::Relaxed);
        assert_ne!(id, 0, "selector IDs must not wrap");

        let target_pseudo_element = compound_selectors
            .last()
            .and_then(|compound| compound.simple_selectors.first())
            .and_then(|simple_selector| match simple_selector {
                SimpleSelector::PseudoElement(selector)
                    if !matches!(
                        selector.pseudo_element,
                        PseudoElementType::Part | PseudoElementType::Slotted
                    ) =>
                {
                    Some(selector.pseudo_element)
                }
                _ => None,
            });

        let can_use_fast_matches = compound_selectors.iter().all(|compound| {
            matches!(
                compound.combinator,
                Combinator::None | Combinator::Descendant | Combinator::ImmediateChild
            ) && compound
                .simple_selectors
                .iter()
                .all(can_simple_selector_use_fast_matches)
        });

        let sibling_invalidation_distance = sibling_invalidation_distance(&compound_selectors);

        Arc::new(Self {
            id,
            cxx_selector: cxx_selector as usize,
            compound_selectors,
            target_pseudo_element,
            can_use_fast_matches,
            sibling_invalidation_distance,
        })
    }

    pub fn id(&self) -> u64 {
        self.id
    }

    fn cxx_selector(&self) -> *const c_void {
        self.cxx_selector as *const c_void
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MatchTarget<Element> {
    pub element: Element,
    pub pseudo_element: Option<PseudoElementType>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SelectorKind {
    Normal,
    Relative,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TagNameMatchingMode {
    Normal,
    Fast,
}

#[derive(Clone, Copy)]
struct MatchState<Element> {
    scope: Option<Element>,
    selector_kind: SelectorKind,
    anchor: Option<Element>,
}

struct PseudoElementTransition<Element> {
    target: MatchTarget<Element>,
    shadow_host: Option<Element>,
}

pub trait SelectorDom {
    type Element: Copy + Eq;

    fn matches_universal_selector(&mut self, element: Self::Element, qualified_name: &QualifiedName) -> bool;
    fn matches_tag_name_selector(
        &mut self,
        element: Self::Element,
        qualified_name: &QualifiedName,
        mode: TagNameMatchingMode,
    ) -> bool;
    fn matches_id_selector(&mut self, element: Self::Element, id: &[u16]) -> bool;
    fn matches_class_selector(&mut self, element: Self::Element, class_name: &[u16]) -> bool;
    fn matches_attribute_selector(&mut self, element: Self::Element, attribute: &AttributeSelector) -> bool;
    fn matches_pseudo_class_state(&mut self, element: Self::Element, pseudo_class: &PseudoClassSelector) -> bool;

    fn parent_element(&mut self, element: Self::Element, shadow_host: Option<Self::Element>) -> Option<Self::Element>;
    fn parent_element_in_light_tree(&mut self, element: Self::Element) -> Option<Self::Element>;
    fn previous_element_sibling(&mut self, element: Self::Element) -> Option<Self::Element>;
    fn next_element_sibling(&mut self, element: Self::Element) -> Option<Self::Element>;
    fn first_element_child(&mut self, element: Self::Element) -> Option<Self::Element>;
    fn last_element_child(&mut self, element: Self::Element) -> Option<Self::Element>;
    fn first_element_descendant(&mut self, element: Self::Element) -> Option<Self::Element>;
    fn next_element_descendant(&mut self, element: Self::Element, root: Self::Element) -> Option<Self::Element>;
    fn has_no_element_or_nonempty_text_children(&mut self, element: Self::Element) -> bool;
    fn has_same_type(&mut self, first: Self::Element, second: Self::Element) -> bool;
    fn is_document_root(&mut self, element: Self::Element) -> bool;

    fn slotted_parent(&mut self, element: Self::Element) -> Option<(Self::Element, Option<Self::Element>)>;
    fn part_parent(
        &mut self,
        element: Self::Element,
        identifiers: &[SelectorString],
        allow_same_shadow_root_scope: bool,
        shadow_host: Option<Self::Element>,
    ) -> Option<(Self::Element, Option<Self::Element>)>;

    fn note_structural_pseudo_class(&mut self, element: Self::Element, pseudo_class: PseudoClassType);
    fn note_has_pseudo_class(&mut self, element: Self::Element);
    fn note_sibling_combinator(
        &mut self,
        element: Self::Element,
        combinator: Combinator,
        sibling_invalidation_distance: usize,
    );
    fn note_has_sibling_combinator_anchor(&mut self, anchor: Self::Element);
    fn note_has_sibling_combinator_element(&mut self, element: Self::Element);
    fn note_has_scope_element(&mut self, element: Self::Element);
    fn collects_selector_involvement_metadata(&mut self) -> bool;

    fn enter_has_argument_matching(&mut self) -> bool;
    fn leave_has_argument_matching(&mut self, previous_value: bool);
    fn has_cache_get(&mut self, selector_id: u64, anchor: Self::Element) -> Option<bool>;
    fn has_cache_set(&mut self, selector_id: u64, anchor: Self::Element, result: bool);
    fn should_reject_has_argument(&mut self, selector: &CompiledSelector, anchor: Self::Element) -> bool;
}

pub fn matches_selector<Dom: SelectorDom>(
    selector: &CompiledSelector,
    target: MatchTarget<Dom::Element>,
    shadow_host: Option<Dom::Element>,
    scope: Option<Dom::Element>,
    dom: &mut Dom,
) -> bool {
    matches_selector_internal(selector, target, shadow_host, scope, SelectorKind::Normal, None, dom)
}

pub fn matches_originating_element_for_pseudo_element<Dom: SelectorDom>(
    selector: &CompiledSelector,
    pseudo_element: PseudoElementType,
    target: Dom::Element,
    shadow_host: Option<Dom::Element>,
    scope: Option<Dom::Element>,
    dom: &mut Dom,
) -> bool {
    for component_list_index in (0..selector.compound_selectors.len()).rev() {
        let compound_selector = &selector.compound_selectors[component_list_index];
        if compound_selector.combinator != Combinator::PseudoElement {
            continue;
        }
        let Some(SimpleSelector::PseudoElement(pseudo_element_selector)) = compound_selector.simple_selectors.first()
        else {
            continue;
        };
        if pseudo_element_selector.pseudo_element != pseudo_element {
            continue;
        }
        if component_list_index == 0 {
            return false;
        }
        return matches_compound_selector(
            selector,
            component_list_index - 1,
            MatchTarget {
                element: target,
                pseudo_element: None,
            },
            shadow_host,
            MatchState {
                scope,
                selector_kind: SelectorKind::Normal,
                anchor: None,
            },
            dom,
        );
    }
    false
}

fn matches_selector_internal<Dom: SelectorDom>(
    selector: &CompiledSelector,
    target: MatchTarget<Dom::Element>,
    shadow_host: Option<Dom::Element>,
    scope: Option<Dom::Element>,
    selector_kind: SelectorKind,
    anchor: Option<Dom::Element>,
    dom: &mut Dom,
) -> bool {
    if selector_kind == SelectorKind::Normal && target.pseudo_element.is_none() && selector.can_use_fast_matches {
        return fast_matches(selector, target.element, shadow_host, dom);
    }

    if selector.target_pseudo_element != target.pseudo_element {
        return false;
    }
    let Some(component_list_index) = selector.compound_selectors.len().checked_sub(1) else {
        return false;
    };
    matches_compound_selector(
        selector,
        component_list_index,
        target,
        shadow_host,
        MatchState {
            scope,
            selector_kind,
            anchor,
        },
        dom,
    )
}

fn matches_compound_selector<Dom: SelectorDom>(
    selector: &CompiledSelector,
    component_list_index: usize,
    target: MatchTarget<Dom::Element>,
    shadow_host: Option<Dom::Element>,
    state: MatchState<Dom::Element>,
    dom: &mut Dom,
) -> bool {
    let compound_selector = &selector.compound_selectors[component_list_index];
    let is_has = |simple_selector: &SimpleSelector| {
        matches!(
            simple_selector,
            SimpleSelector::PseudoClass(PseudoClassSelector {
                pseudo_class: PseudoClassType::Has,
                ..
            })
        )
    };

    for simple_selector in compound_selector
        .simple_selectors
        .iter()
        .rev()
        .filter(|selector| !is_has(selector))
    {
        if !matches_simple_selector(
            simple_selector,
            target,
            shadow_host,
            state.scope,
            state.selector_kind,
            TagNameMatchingMode::Normal,
            dom,
        ) {
            return false;
        }
    }
    for simple_selector in compound_selector
        .simple_selectors
        .iter()
        .rev()
        .filter(|selector| is_has(selector))
    {
        if !matches_simple_selector(
            simple_selector,
            target,
            shadow_host,
            state.scope,
            state.selector_kind,
            TagNameMatchingMode::Normal,
            dom,
        ) {
            return false;
        }
    }

    if state.selector_kind == SelectorKind::Relative && component_list_index == 0 {
        return Some(target.element) != state.anchor;
    }

    match compound_selector.combinator {
        Combinator::None => state.selector_kind != SelectorKind::Relative,
        Combinator::PseudoElement => {
            let Some(previous_target) =
                pseudo_element_transition_target(selector, component_list_index, target, shadow_host, state.scope, dom)
            else {
                return false;
            };
            let Some(previous_index) = component_list_index.checked_sub(1) else {
                return false;
            };
            matches_compound_selector(
                selector,
                previous_index,
                previous_target.target,
                previous_target.shadow_host,
                state,
                dom,
            )
        }
        Combinator::Descendant => {
            let Some(previous_index) = component_list_index.checked_sub(1) else {
                return false;
            };
            let mut ancestor = traverse_up(target, shadow_host, dom);
            while let Some(element) = ancestor {
                if Some(element) == state.anchor {
                    return false;
                }
                dom.note_has_scope_element(element);
                if matches_compound_selector(
                    selector,
                    previous_index,
                    MatchTarget {
                        element,
                        pseudo_element: None,
                    },
                    shadow_host,
                    state,
                    dom,
                ) {
                    return true;
                }
                ancestor = dom.parent_element(element, shadow_host);
            }
            false
        }
        Combinator::ImmediateChild => {
            let Some(previous_index) = component_list_index.checked_sub(1) else {
                return false;
            };
            let Some(parent) = traverse_up(target, shadow_host, dom) else {
                return false;
            };
            dom.note_has_scope_element(parent);
            matches_compound_selector(
                selector,
                previous_index,
                MatchTarget {
                    element: parent,
                    pseudo_element: None,
                },
                shadow_host,
                state,
                dom,
            )
        }
        Combinator::NextSibling => {
            dom.note_sibling_combinator(
                target.element,
                Combinator::NextSibling,
                selector.sibling_invalidation_distance,
            );
            let Some(previous_index) = component_list_index.checked_sub(1) else {
                return false;
            };
            let Some(sibling) = dom.previous_element_sibling(target.element) else {
                return false;
            };
            dom.note_has_scope_element(sibling);
            matches_compound_selector(
                selector,
                previous_index,
                MatchTarget {
                    element: sibling,
                    pseudo_element: None,
                },
                shadow_host,
                state,
                dom,
            )
        }
        Combinator::SubsequentSibling => {
            dom.note_sibling_combinator(
                target.element,
                Combinator::SubsequentSibling,
                selector.sibling_invalidation_distance,
            );
            let Some(previous_index) = component_list_index.checked_sub(1) else {
                return false;
            };
            let mut sibling = dom.previous_element_sibling(target.element);
            while let Some(element) = sibling {
                dom.note_has_scope_element(element);
                if matches_compound_selector(
                    selector,
                    previous_index,
                    MatchTarget {
                        element,
                        pseudo_element: None,
                    },
                    shadow_host,
                    state,
                    dom,
                ) {
                    return true;
                }
                sibling = dom.previous_element_sibling(element);
            }
            false
        }
        Combinator::Column => unimplemented!("column combinator matching"),
    }
}

fn matches_simple_selector<Dom: SelectorDom>(
    simple_selector: &SimpleSelector,
    target: MatchTarget<Dom::Element>,
    shadow_host: Option<Dom::Element>,
    scope: Option<Dom::Element>,
    selector_kind: SelectorKind,
    tag_name_matching_mode: TagNameMatchingMode,
    dom: &mut Dom,
) -> bool {
    if should_block_shadow_host_matching(simple_selector, target.element, shadow_host, scope) {
        return false;
    }

    match simple_selector {
        SimpleSelector::Universal(qualified_name) => {
            target.pseudo_element.is_none() && dom.matches_universal_selector(target.element, qualified_name)
        }
        SimpleSelector::TagName(qualified_name) => {
            target.pseudo_element.is_none()
                && dom.matches_tag_name_selector(target.element, qualified_name, tag_name_matching_mode)
        }
        SimpleSelector::Id(id) => target.pseudo_element.is_none() && dom.matches_id_selector(target.element, id),
        SimpleSelector::Class(class_name) => {
            target.pseudo_element.is_none() && dom.matches_class_selector(target.element, class_name)
        }
        SimpleSelector::Attribute(attribute) => {
            target.pseudo_element.is_none() && dom.matches_attribute_selector(target.element, attribute)
        }
        SimpleSelector::PseudoClass(pseudo_class) => {
            matches_pseudo_class(pseudo_class, target, shadow_host, scope, selector_kind, dom)
        }
        SimpleSelector::PseudoElement(pseudo_element) => match pseudo_element.pseudo_element {
            PseudoElementType::Slotted | PseudoElementType::Part => target.pseudo_element.is_none(),
            pseudo_element => target.pseudo_element == Some(pseudo_element),
        },
        SimpleSelector::Nesting => {
            target.pseudo_element.is_none()
                && scope.map_or_else(|| dom.is_document_root(target.element), |scope| scope == target.element)
        }
        SimpleSelector::Invalid => false,
    }
}

fn traverse_up<Dom: SelectorDom>(
    target: MatchTarget<Dom::Element>,
    shadow_host: Option<Dom::Element>,
    dom: &mut Dom,
) -> Option<Dom::Element> {
    if target.pseudo_element.is_some() {
        return Some(target.element);
    }
    dom.parent_element(target.element, shadow_host)
}

fn should_block_shadow_host_matching<Element: Copy + Eq>(
    simple_selector: &SimpleSelector,
    element: Element,
    shadow_host: Option<Element>,
    scope: Option<Element>,
) -> bool {
    if shadow_host != Some(element) {
        return false;
    }
    match simple_selector {
        SimpleSelector::PseudoClass(pseudo_class) => {
            !(matches!(
                pseudo_class.pseudo_class,
                PseudoClassType::Host | PseudoClassType::Has | PseudoClassType::Is | PseudoClassType::Where
            ) || pseudo_class.pseudo_class == PseudoClassType::Scope && scope == Some(element))
        }
        SimpleSelector::Nesting | SimpleSelector::PseudoElement(_) => false,
        _ => true,
    }
}

fn matches_pseudo_class<Dom: SelectorDom>(
    pseudo_class: &PseudoClassSelector,
    target: MatchTarget<Dom::Element>,
    shadow_host: Option<Dom::Element>,
    scope: Option<Dom::Element>,
    selector_kind: SelectorKind,
    dom: &mut Dom,
) -> bool {
    use PseudoClassType::*;

    match pseudo_class.pseudo_class {
        Is | Where => pseudo_class.argument_selector_list.iter().any(|selector| {
            matches_selector_internal(selector, target, shadow_host, scope, SelectorKind::Normal, None, dom)
        }),
        Not => pseudo_class.argument_selector_list.iter().all(|selector| {
            !matches_selector_internal(selector, target, shadow_host, scope, SelectorKind::Normal, None, dom)
        }),
        Has => {
            if target.pseudo_element.is_some() || selector_kind == SelectorKind::Relative {
                return false;
            }
            dom.note_has_pseudo_class(target.element);
            pseudo_class
                .argument_selector_list
                .iter()
                .any(|selector| matches_has_pseudo_class(selector, target.element, shadow_host, scope, dom))
        }
        Host => {
            if target.pseudo_element.is_some() || shadow_host != Some(target.element) {
                return false;
            }
            pseudo_class.argument_selector_list.first().is_none_or(|selector| {
                matches_selector_internal(
                    selector,
                    MatchTarget {
                        element: target.element,
                        pseudo_element: None,
                    },
                    None,
                    None,
                    SelectorKind::Normal,
                    None,
                    dom,
                )
            })
        }
        Scope => {
            target.pseudo_element.is_none()
                && scope.map_or_else(|| dom.is_document_root(target.element), |scope| scope == target.element)
        }
        FirstChild | LastChild | OnlyChild | FirstOfType | LastOfType | OnlyOfType | NthChild | NthLastChild
        | NthOfType | NthLastOfType => {
            if target.pseudo_element.is_some() {
                return false;
            }
            dom.note_structural_pseudo_class(target.element, pseudo_class.pseudo_class);
            match pseudo_class.pseudo_class {
                FirstChild => dom.previous_element_sibling(target.element).is_none(),
                LastChild => dom.next_element_sibling(target.element).is_none(),
                OnlyChild => {
                    dom.previous_element_sibling(target.element).is_none()
                        && dom.next_element_sibling(target.element).is_none()
                }
                FirstOfType => previous_sibling_with_same_type(target.element, dom).is_none(),
                LastOfType => next_sibling_with_same_type(target.element, dom).is_none(),
                OnlyOfType => {
                    previous_sibling_with_same_type(target.element, dom).is_none()
                        && next_sibling_with_same_type(target.element, dom).is_none()
                }
                NthChild | NthLastChild | NthOfType | NthLastOfType => {
                    matches_nth_pseudo_class(pseudo_class, target.element, shadow_host, dom)
                }
                _ => unreachable!(),
            }
        }
        Empty => target.pseudo_element.is_none() && dom.has_no_element_or_nonempty_text_children(target.element),
        Root => target.pseudo_element.is_none() && dom.is_document_root(target.element),
        _ => target.pseudo_element.is_none() && dom.matches_pseudo_class_state(target.element, pseudo_class),
    }
}

fn matches_nth_pseudo_class<Dom: SelectorDom>(
    pseudo_class: &PseudoClassSelector,
    target: Dom::Element,
    shadow_host: Option<Dom::Element>,
    dom: &mut Dom,
) -> bool {
    let matches_selector_list = |element: Dom::Element, dom: &mut Dom| {
        pseudo_class.argument_selector_list.is_empty()
            || pseudo_class.argument_selector_list.iter().any(|selector| {
                matches_selector_internal(
                    selector,
                    MatchTarget {
                        element,
                        pseudo_element: None,
                    },
                    shadow_host,
                    None,
                    SelectorKind::Normal,
                    None,
                    dom,
                )
            })
    };

    let mut index = 1i32;
    match pseudo_class.pseudo_class {
        PseudoClassType::NthChild => {
            if !matches_selector_list(target, dom) {
                return false;
            }
            let mut child = dom
                .parent_element_in_light_tree(target)
                .and_then(|parent| dom.first_element_child(parent));
            while let Some(element) = child {
                if element == target {
                    break;
                }
                if matches_selector_list(element, dom) {
                    index += 1;
                }
                child = dom.next_element_sibling(element);
            }
        }
        PseudoClassType::NthLastChild => {
            if !matches_selector_list(target, dom) {
                return false;
            }
            let mut child = dom
                .parent_element_in_light_tree(target)
                .and_then(|parent| dom.last_element_child(parent));
            while let Some(element) = child {
                if element == target {
                    break;
                }
                if matches_selector_list(element, dom) {
                    index += 1;
                }
                child = dom.previous_element_sibling(element);
            }
        }
        PseudoClassType::NthOfType => {
            let mut sibling = previous_sibling_with_same_type(target, dom);
            while let Some(element) = sibling {
                index += 1;
                sibling = previous_sibling_with_same_type(element, dom);
            }
        }
        PseudoClassType::NthLastOfType => {
            let mut sibling = next_sibling_with_same_type(target, dom);
            while let Some(element) = sibling {
                index += 1;
                sibling = next_sibling_with_same_type(element, dom);
            }
        }
        _ => unreachable!(),
    }
    pseudo_class.an_plus_b_pattern.matches(index)
}

fn previous_sibling_with_same_type<Dom: SelectorDom>(element: Dom::Element, dom: &mut Dom) -> Option<Dom::Element> {
    let mut sibling = dom.previous_element_sibling(element);
    while let Some(candidate) = sibling {
        if dom.has_same_type(candidate, element) {
            return Some(candidate);
        }
        sibling = dom.previous_element_sibling(candidate);
    }
    None
}

fn next_sibling_with_same_type<Dom: SelectorDom>(element: Dom::Element, dom: &mut Dom) -> Option<Dom::Element> {
    let mut sibling = dom.next_element_sibling(element);
    while let Some(candidate) = sibling {
        if dom.has_same_type(candidate, element) {
            return Some(candidate);
        }
        sibling = dom.next_element_sibling(candidate);
    }
    None
}

fn matches_has_pseudo_class<Dom: SelectorDom>(
    selector: &CompiledSelector,
    anchor: Dom::Element,
    shadow_host: Option<Dom::Element>,
    scope: Option<Dom::Element>,
    dom: &mut Dom,
) -> bool {
    if let Some(result) = dom.has_cache_get(selector.id(), anchor) {
        return result;
    }
    let previous_inside_has = dom.enter_has_argument_matching();
    let result = if dom.should_reject_has_argument(selector, anchor) {
        false
    } else if dom.collects_selector_involvement_metadata() {
        matches_relative_selector(selector, 0, anchor, shadow_host, scope, anchor, dom)
    } else if let Some(simple_selector) = simple_has_child_tag_selector(selector) {
        matches_has_child_tag_fast_path(simple_selector, anchor, shadow_host, dom)
    } else if let Some(compound_selector) = simple_has_descendant_tag_and_class_compound(selector) {
        matches_has_descendant_tag_and_class_fast_path(selector, compound_selector, anchor, shadow_host, dom)
    } else {
        matches_relative_selector(selector, 0, anchor, shadow_host, scope, anchor, dom)
    };
    dom.leave_has_argument_matching(previous_inside_has);
    dom.has_cache_set(selector.id(), anchor, result);
    result
}

fn simple_has_child_tag_selector(selector: &CompiledSelector) -> Option<&SimpleSelector> {
    let [compound_selector] = &*selector.compound_selectors else {
        return None;
    };
    if compound_selector.combinator != Combinator::ImmediateChild {
        return None;
    }
    let [simple_selector @ SimpleSelector::TagName(_)] = &*compound_selector.simple_selectors else {
        return None;
    };
    Some(simple_selector)
}

fn simple_has_descendant_tag_and_class_compound(selector: &CompiledSelector) -> Option<&CompoundSelector> {
    let [compound_selector] = &*selector.compound_selectors else {
        return None;
    };
    if compound_selector.combinator != Combinator::Descendant
        || compound_selector.simple_selectors.is_empty()
        || !compound_selector
            .simple_selectors
            .iter()
            .all(|selector| matches!(selector, SimpleSelector::TagName(_) | SimpleSelector::Class(_)))
    {
        return None;
    }
    Some(compound_selector)
}

fn matches_has_child_tag_fast_path<Dom: SelectorDom>(
    simple_selector: &SimpleSelector,
    anchor: Dom::Element,
    shadow_host: Option<Dom::Element>,
    dom: &mut Dom,
) -> bool {
    let mut child = dom.first_element_child(anchor);
    while let Some(element) = child {
        if matches_simple_selector(
            simple_selector,
            MatchTarget {
                element,
                pseudo_element: None,
            },
            shadow_host,
            None,
            SelectorKind::Normal,
            TagNameMatchingMode::Fast,
            dom,
        ) {
            return true;
        }
        child = dom.next_element_sibling(element);
    }
    false
}

fn matches_has_descendant_tag_and_class_fast_path<Dom: SelectorDom>(
    selector: &CompiledSelector,
    compound_selector: &CompoundSelector,
    anchor: Dom::Element,
    shadow_host: Option<Dom::Element>,
    dom: &mut Dom,
) -> bool {
    let mut descendant = dom.first_element_descendant(anchor);
    while let Some(element) = descendant {
        if fast_matches_compound_selector(compound_selector, element, shadow_host, dom) {
            cache_matching_has_ancestors(selector.id(), element, anchor, dom);
            return true;
        }
        descendant = dom.next_element_descendant(element, anchor);
    }
    false
}

fn matches_relative_selector<Dom: SelectorDom>(
    selector: &CompiledSelector,
    compound_index: usize,
    element: Dom::Element,
    shadow_host: Option<Dom::Element>,
    scope: Option<Dom::Element>,
    anchor: Dom::Element,
    dom: &mut Dom,
) -> bool {
    if compound_index >= selector.compound_selectors.len() {
        return matches_selector_internal(
            selector,
            MatchTarget {
                element,
                pseudo_element: None,
            },
            shadow_host,
            scope,
            SelectorKind::Relative,
            Some(anchor),
            dom,
        );
    }

    match selector.compound_selectors[compound_index].combinator {
        Combinator::None => false,
        Combinator::Descendant => {
            let mut descendant = dom.first_element_descendant(element);
            while let Some(candidate) = descendant {
                dom.note_has_scope_element(candidate);
                if matches_selector_internal(
                    selector,
                    MatchTarget {
                        element: candidate,
                        pseudo_element: None,
                    },
                    shadow_host,
                    scope,
                    SelectorKind::Relative,
                    Some(anchor),
                    dom,
                ) {
                    cache_matching_has_ancestors(selector.id(), candidate, element, dom);
                    return true;
                }
                descendant = dom.next_element_descendant(candidate, element);
            }
            false
        }
        Combinator::ImmediateChild => {
            let mut child = dom.first_element_child(element);
            while let Some(candidate) = child {
                dom.note_has_scope_element(candidate);
                if matches_compound_selector(
                    selector,
                    compound_index,
                    MatchTarget {
                        element: candidate,
                        pseudo_element: None,
                    },
                    shadow_host,
                    MatchState {
                        scope,
                        selector_kind: SelectorKind::Relative,
                        anchor: Some(anchor),
                    },
                    dom,
                ) && matches_relative_selector(
                    selector,
                    compound_index + 1,
                    candidate,
                    shadow_host,
                    scope,
                    anchor,
                    dom,
                ) {
                    return true;
                }
                child = dom.next_element_sibling(candidate);
            }
            false
        }
        Combinator::NextSibling => {
            dom.note_has_sibling_combinator_anchor(anchor);
            let Some(sibling) = dom.next_element_sibling(element) else {
                return false;
            };
            dom.note_has_scope_element(sibling);
            dom.note_has_sibling_combinator_element(sibling);
            matches_compound_selector(
                selector,
                compound_index,
                MatchTarget {
                    element: sibling,
                    pseudo_element: None,
                },
                shadow_host,
                MatchState {
                    scope,
                    selector_kind: SelectorKind::Relative,
                    anchor: Some(anchor),
                },
                dom,
            ) && matches_relative_selector(selector, compound_index + 1, sibling, shadow_host, scope, anchor, dom)
        }
        Combinator::SubsequentSibling => {
            dom.note_has_sibling_combinator_anchor(anchor);
            let mut sibling = dom.next_element_sibling(element);
            while let Some(candidate) = sibling {
                dom.note_has_scope_element(candidate);
                dom.note_has_sibling_combinator_element(candidate);
                if matches_compound_selector(
                    selector,
                    compound_index,
                    MatchTarget {
                        element: candidate,
                        pseudo_element: None,
                    },
                    shadow_host,
                    MatchState {
                        scope,
                        selector_kind: SelectorKind::Relative,
                        anchor: Some(anchor),
                    },
                    dom,
                ) && matches_relative_selector(
                    selector,
                    compound_index + 1,
                    candidate,
                    shadow_host,
                    scope,
                    anchor,
                    dom,
                ) {
                    return true;
                }
                sibling = dom.next_element_sibling(candidate);
            }
            false
        }
        Combinator::PseudoElement | Combinator::Column => false,
    }
}

fn cache_matching_has_ancestors<Dom: SelectorDom>(
    selector_id: u64,
    matching_descendant: Dom::Element,
    anchor: Dom::Element,
    dom: &mut Dom,
) {
    let mut ancestor = dom.parent_element_in_light_tree(matching_descendant);
    while let Some(element) = ancestor {
        if element == anchor {
            break;
        }
        dom.has_cache_set(selector_id, element, true);
        ancestor = dom.parent_element_in_light_tree(element);
    }
}

fn compound_may_match_host_for_part_scope(compound_selector: &CompoundSelector) -> bool {
    compound_selector.simple_selectors.iter().any(|simple_selector| {
        let SimpleSelector::PseudoClass(pseudo_class) = simple_selector else {
            return false;
        };
        if pseudo_class.pseudo_class == PseudoClassType::Host {
            return true;
        }
        pseudo_class.pseudo_class == PseudoClassType::Is
            && pseudo_class.argument_selector_list.iter().any(|selector| {
                selector.compound_selectors.iter().any(|compound| {
                    compound.simple_selectors.iter().any(|simple| {
                        matches!(
                            simple,
                            SimpleSelector::PseudoClass(PseudoClassSelector {
                                pseudo_class: PseudoClassType::Host,
                                ..
                            })
                        )
                    })
                })
            })
    })
}

fn pseudo_element_transition_target<Dom: SelectorDom>(
    selector: &CompiledSelector,
    component_list_index: usize,
    target: MatchTarget<Dom::Element>,
    shadow_host: Option<Dom::Element>,
    scope: Option<Dom::Element>,
    dom: &mut Dom,
) -> Option<PseudoElementTransition<Dom::Element>> {
    let compound_selector = &selector.compound_selectors[component_list_index];
    let SimpleSelector::PseudoElement(pseudo_element_selector) = compound_selector.simple_selectors.first()? else {
        return None;
    };

    match &pseudo_element_selector.value {
        PseudoElementValue::CompoundSelector(slotted_selector)
            if pseudo_element_selector.pseudo_element == PseudoElementType::Slotted =>
        {
            if target.pseudo_element.is_some()
                || !matches_selector_internal(
                    slotted_selector,
                    target,
                    shadow_host,
                    scope,
                    SelectorKind::Normal,
                    None,
                    dom,
                )
            {
                return None;
            }
            dom.slotted_parent(target.element)
                .map(|(slot, host)| PseudoElementTransition {
                    target: MatchTarget {
                        element: slot,
                        pseudo_element: None,
                    },
                    shadow_host: host,
                })
        }
        PseudoElementValue::Identifiers(identifiers)
            if pseudo_element_selector.pseudo_element == PseudoElementType::Part =>
        {
            if target.pseudo_element.is_some() || component_list_index == 0 {
                return None;
            }
            let allow_same_shadow_root_scope =
                compound_may_match_host_for_part_scope(&selector.compound_selectors[component_list_index - 1]);
            dom.part_parent(target.element, identifiers, allow_same_shadow_root_scope, shadow_host)
                .map(|(host, next_shadow_host)| PseudoElementTransition {
                    target: MatchTarget {
                        element: host,
                        pseudo_element: None,
                    },
                    shadow_host: next_shadow_host,
                })
        }
        _ if target.pseudo_element == Some(pseudo_element_selector.pseudo_element) => Some(PseudoElementTransition {
            target: MatchTarget {
                element: target.element,
                pseudo_element: None,
            },
            shadow_host,
        }),
        _ => None,
    }
}

fn fast_matches<Dom: SelectorDom>(
    selector: &CompiledSelector,
    element_to_match: Dom::Element,
    shadow_host: Option<Dom::Element>,
    dom: &mut Dom,
) -> bool {
    let mut current = element_to_match;
    let mut compound_selector_index = selector.compound_selectors.len() - 1;
    if !fast_matches_compound_selector(
        &selector.compound_selectors[compound_selector_index],
        current,
        shadow_host,
        dom,
    ) {
        return false;
    }

    let mut backtrack_state = None;
    loop {
        let compound_selector = &selector.compound_selectors[compound_selector_index];
        match compound_selector.combinator {
            Combinator::None => return true,
            Combinator::Descendant => {
                backtrack_state = dom
                    .parent_element_in_light_tree(current)
                    .map(|parent| (parent, compound_selector_index));
                compound_selector_index -= 1;
                let previous_compound = &selector.compound_selectors[compound_selector_index];
                let mut ancestor = dom.parent_element_in_light_tree(current);
                loop {
                    let Some(element) = ancestor else {
                        return false;
                    };
                    if fast_matches_compound_selector(previous_compound, element, shadow_host, dom) {
                        current = element;
                        break;
                    }
                    ancestor = dom.parent_element_in_light_tree(element);
                }
            }
            Combinator::ImmediateChild => {
                compound_selector_index -= 1;
                let Some(parent) = dom.parent_element_in_light_tree(current) else {
                    return false;
                };
                current = parent;
                if !fast_matches_compound_selector(
                    &selector.compound_selectors[compound_selector_index],
                    current,
                    shadow_host,
                    dom,
                ) {
                    let Some((element, index)) = backtrack_state.take() else {
                        return false;
                    };
                    current = element;
                    compound_selector_index = index;
                }
            }
            _ => unreachable!("selector marked fast-matchable contains an unsupported combinator"),
        }
    }
}

fn fast_matches_compound_selector<Dom: SelectorDom>(
    compound_selector: &CompoundSelector,
    element: Dom::Element,
    shadow_host: Option<Dom::Element>,
    dom: &mut Dom,
) -> bool {
    compound_selector.simple_selectors.iter().all(|simple_selector| {
        matches_simple_selector(
            simple_selector,
            MatchTarget {
                element,
                pseudo_element: None,
            },
            shadow_host,
            None,
            SelectorKind::Normal,
            TagNameMatchingMode::Fast,
            dom,
        )
    })
}

#[derive(Clone, Copy)]
#[repr(u8)]
pub enum FfiSimpleSelectorType {
    Universal,
    TagName,
    Id,
    Class,
    Attribute,
    PseudoClass,
    PseudoElement,
    Nesting,
    Invalid,
}

#[derive(Clone, Copy)]
#[repr(u8)]
pub enum FfiCombinator {
    None,
    ImmediateChild,
    Descendant,
    NextSibling,
    SubsequentSibling,
    Column,
    PseudoElement,
}

#[derive(Clone, Copy)]
#[repr(u8)]
pub enum FfiNamespaceType {
    Default,
    None,
    Any,
    Named,
}

#[derive(Clone, Copy)]
#[repr(u8)]
pub enum FfiAttributeMatchType {
    HasAttribute,
    ExactValue,
    ContainsWord,
    ContainsString,
    StartsWithSegment,
    StartsWithString,
    EndsWithString,
}

#[derive(Clone, Copy)]
#[repr(u8)]
pub enum FfiAttributeCaseType {
    Default,
    Sensitive,
    Insensitive,
}

#[derive(Clone, Copy)]
#[repr(u8)]
pub enum FfiDirection {
    None,
    LeftToRight,
    RightToLeft,
    Other,
}

#[derive(Clone, Copy)]
#[repr(u8)]
pub enum FfiPseudoElementValueType {
    None,
    CompoundSelector,
    Identifiers,
    TransitionName,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiStringView {
    pub data: *const u16,
    pub length: usize,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiSimpleSelector {
    pub selector_type: FfiSimpleSelectorType,
    pub namespace_type: FfiNamespaceType,
    pub namespace: FfiStringView,
    pub name: FfiStringView,
    pub lowercase_name: FfiStringView,
    pub attribute_match_type: FfiAttributeMatchType,
    pub attribute_case_type: FfiAttributeCaseType,
    pub attribute_value: FfiStringView,
    pub pseudo_class: u8,
    pub an_plus_b_step_size: i32,
    pub an_plus_b_offset: i32,
    pub argument_selectors: *const *const RustSelector,
    pub argument_selector_count: usize,
    pub languages: *const FfiStringView,
    pub language_count: usize,
    pub direction: FfiDirection,
    pub identifier: FfiStringView,
    pub levels: *const i64,
    pub level_count: usize,
    pub pseudo_element: u8,
    pub pseudo_element_value_type: FfiPseudoElementValueType,
    pub pseudo_element_selector: *const RustSelector,
    pub pseudo_element_identifiers: *const FfiStringView,
    pub pseudo_element_identifier_count: usize,
    pub transition_name_is_universal: bool,
    pub transition_name: FfiStringView,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiCompoundSelector {
    pub combinator: FfiCombinator,
    pub simple_selectors: *const FfiSimpleSelector,
    pub simple_selector_count: usize,
}

#[repr(C)]
pub struct FfiSelector {
    pub cxx_selector: *const c_void,
    pub compound_selectors: *const FfiCompoundSelector,
    pub compound_selector_count: usize,
}

pub struct RustSelector {
    selector: Arc<CompiledSelector>,
}

#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(transparent)]
struct FfiElement(*const c_void);

#[derive(Clone, Copy)]
#[repr(C)]
struct FfiElementAndShadowHost {
    element: *const c_void,
    shadow_host: *const c_void,
}

unsafe extern "C" {
    fn selector_ffi_matches_universal(
        context: *mut c_void,
        element: *const c_void,
        namespace_type: FfiNamespaceType,
        namespace: FfiStringView,
    ) -> bool;
    fn selector_ffi_matches_tag_name(
        context: *mut c_void,
        element: *const c_void,
        namespace_type: FfiNamespaceType,
        namespace: FfiStringView,
        name: FfiStringView,
        lowercase_name: FfiStringView,
        matching_mode: u8,
    ) -> bool;
    fn selector_ffi_matches_id(element: *const c_void, id: FfiStringView) -> bool;
    fn selector_ffi_matches_class(element: *const c_void, class_name: FfiStringView) -> bool;
    fn selector_ffi_matches_attribute(
        context: *mut c_void,
        element: *const c_void,
        namespace_type: FfiNamespaceType,
        namespace: FfiStringView,
        name: FfiStringView,
        lowercase_name: FfiStringView,
        match_type: FfiAttributeMatchType,
        case_type: FfiAttributeCaseType,
        value: FfiStringView,
    ) -> bool;
    fn selector_ffi_matches_pseudo_class(element: *const c_void, pseudo_class: u8) -> bool;
    fn selector_ffi_matches_language(element: *const c_void, language: FfiStringView) -> bool;
    fn selector_ffi_matches_direction(element: *const c_void, direction: FfiDirection) -> bool;
    fn selector_ffi_matches_state(element: *const c_void, identifier: FfiStringView) -> bool;
    fn selector_ffi_matches_heading(element: *const c_void, levels: *const i64, level_count: usize) -> bool;

    fn selector_ffi_parent_element(element: *const c_void, shadow_host: *const c_void) -> *const c_void;
    fn selector_ffi_parent_element_in_light_tree(element: *const c_void) -> *const c_void;
    fn selector_ffi_previous_element_sibling(element: *const c_void) -> *const c_void;
    fn selector_ffi_next_element_sibling(element: *const c_void) -> *const c_void;
    fn selector_ffi_first_element_child(element: *const c_void) -> *const c_void;
    fn selector_ffi_last_element_child(element: *const c_void) -> *const c_void;
    fn selector_ffi_first_element_descendant(element: *const c_void) -> *const c_void;
    fn selector_ffi_next_element_descendant(element: *const c_void, root: *const c_void) -> *const c_void;
    fn selector_ffi_has_no_element_or_nonempty_text_children(element: *const c_void) -> bool;
    fn selector_ffi_has_same_type(first: *const c_void, second: *const c_void) -> bool;
    fn selector_ffi_is_document_root(element: *const c_void) -> bool;

    fn selector_ffi_slotted_parent(context: *mut c_void, element: *const c_void) -> FfiElementAndShadowHost;
    fn selector_ffi_part_parent(
        context: *mut c_void,
        element: *const c_void,
        identifiers: *const FfiStringView,
        identifier_count: usize,
        allow_same_shadow_root_scope: bool,
        shadow_host: *const c_void,
    ) -> FfiElementAndShadowHost;

    fn selector_ffi_note_structural_pseudo_class(context: *mut c_void, element: *const c_void, pseudo_class: u8);
    fn selector_ffi_note_has_pseudo_class(context: *mut c_void, element: *const c_void);
    fn selector_ffi_note_sibling_combinator(
        context: *mut c_void,
        element: *const c_void,
        combinator: FfiCombinator,
        sibling_invalidation_distance: usize,
    );
    fn selector_ffi_note_has_sibling_combinator_anchor(context: *mut c_void, anchor: *const c_void);
    fn selector_ffi_note_has_sibling_combinator_element(context: *mut c_void, element: *const c_void);
    fn selector_ffi_note_has_scope_element(context: *mut c_void, element: *const c_void);
    fn selector_ffi_collects_selector_involvement_metadata(context: *mut c_void) -> bool;
    fn selector_ffi_inside_has_argument(context: *mut c_void) -> bool;
    fn selector_ffi_set_inside_has_argument(context: *mut c_void, value: bool);
    fn selector_ffi_has_cache_get(context: *mut c_void, selector_id: u64, anchor: *const c_void) -> u8;
    fn selector_ffi_has_cache_set(context: *mut c_void, selector_id: u64, anchor: *const c_void, result: bool);
    fn selector_ffi_should_reject_has_argument(
        context: *mut c_void,
        selector: *const c_void,
        anchor: *const c_void,
    ) -> bool;
}

fn ffi_string_view(string: &[u16]) -> FfiStringView {
    FfiStringView {
        data: string.as_ptr(),
        length: string.len(),
    }
}

fn ffi_namespace_type(namespace_type: NamespaceType) -> FfiNamespaceType {
    match namespace_type {
        NamespaceType::Default => FfiNamespaceType::Default,
        NamespaceType::None => FfiNamespaceType::None,
        NamespaceType::Any => FfiNamespaceType::Any,
        NamespaceType::Named => FfiNamespaceType::Named,
    }
}

fn ffi_attribute_match_type(match_type: AttributeMatchType) -> FfiAttributeMatchType {
    match match_type {
        AttributeMatchType::HasAttribute => FfiAttributeMatchType::HasAttribute,
        AttributeMatchType::ExactValue => FfiAttributeMatchType::ExactValue,
        AttributeMatchType::ContainsWord => FfiAttributeMatchType::ContainsWord,
        AttributeMatchType::ContainsString => FfiAttributeMatchType::ContainsString,
        AttributeMatchType::StartsWithSegment => FfiAttributeMatchType::StartsWithSegment,
        AttributeMatchType::StartsWithString => FfiAttributeMatchType::StartsWithString,
        AttributeMatchType::EndsWithString => FfiAttributeMatchType::EndsWithString,
    }
}

fn ffi_attribute_case_type(case_type: AttributeCaseType) -> FfiAttributeCaseType {
    match case_type {
        AttributeCaseType::Default => FfiAttributeCaseType::Default,
        AttributeCaseType::Sensitive => FfiAttributeCaseType::Sensitive,
        AttributeCaseType::Insensitive => FfiAttributeCaseType::Insensitive,
    }
}

fn ffi_combinator(combinator: Combinator) -> FfiCombinator {
    match combinator {
        Combinator::None => FfiCombinator::None,
        Combinator::ImmediateChild => FfiCombinator::ImmediateChild,
        Combinator::Descendant => FfiCombinator::Descendant,
        Combinator::NextSibling => FfiCombinator::NextSibling,
        Combinator::SubsequentSibling => FfiCombinator::SubsequentSibling,
        Combinator::Column => FfiCombinator::Column,
        Combinator::PseudoElement => FfiCombinator::PseudoElement,
    }
}

fn ffi_element(element: *const c_void) -> Option<FfiElement> {
    (!element.is_null()).then_some(FfiElement(element))
}

fn ffi_element_and_shadow_host(value: FfiElementAndShadowHost) -> Option<(FfiElement, Option<FfiElement>)> {
    Some((ffi_element(value.element)?, ffi_element(value.shadow_host)))
}

struct FfiDom {
    context: *mut c_void,
}

impl SelectorDom for FfiDom {
    type Element = FfiElement;

    fn matches_universal_selector(&mut self, element: FfiElement, name: &QualifiedName) -> bool {
        unsafe {
            selector_ffi_matches_universal(
                self.context,
                element.0,
                ffi_namespace_type(name.namespace_type),
                ffi_string_view(&name.namespace),
            )
        }
    }

    fn matches_tag_name_selector(
        &mut self,
        element: FfiElement,
        name: &QualifiedName,
        mode: TagNameMatchingMode,
    ) -> bool {
        unsafe {
            selector_ffi_matches_tag_name(
                self.context,
                element.0,
                ffi_namespace_type(name.namespace_type),
                ffi_string_view(&name.namespace),
                ffi_string_view(&name.name),
                ffi_string_view(&name.lowercase_name),
                match mode {
                    TagNameMatchingMode::Normal => 0,
                    TagNameMatchingMode::Fast => 1,
                },
            )
        }
    }

    fn matches_id_selector(&mut self, element: FfiElement, id: &[u16]) -> bool {
        unsafe { selector_ffi_matches_id(element.0, ffi_string_view(id)) }
    }

    fn matches_class_selector(&mut self, element: FfiElement, class_name: &[u16]) -> bool {
        unsafe { selector_ffi_matches_class(element.0, ffi_string_view(class_name)) }
    }

    fn matches_attribute_selector(&mut self, element: FfiElement, attribute: &AttributeSelector) -> bool {
        unsafe {
            selector_ffi_matches_attribute(
                self.context,
                element.0,
                ffi_namespace_type(attribute.qualified_name.namespace_type),
                ffi_string_view(&attribute.qualified_name.namespace),
                ffi_string_view(&attribute.qualified_name.name),
                ffi_string_view(&attribute.qualified_name.lowercase_name),
                ffi_attribute_match_type(attribute.match_type),
                ffi_attribute_case_type(attribute.case_type),
                ffi_string_view(&attribute.value),
            )
        }
    }

    fn matches_pseudo_class_state(&mut self, element: FfiElement, pseudo_class: &PseudoClassSelector) -> bool {
        match pseudo_class.pseudo_class {
            PseudoClassType::Lang => pseudo_class
                .languages
                .iter()
                .any(|language| unsafe { selector_ffi_matches_language(element.0, ffi_string_view(language)) }),
            PseudoClassType::Dir => match pseudo_class.direction {
                Some(Direction::LeftToRight) => unsafe {
                    selector_ffi_matches_direction(element.0, FfiDirection::LeftToRight)
                },
                Some(Direction::RightToLeft) => unsafe {
                    selector_ffi_matches_direction(element.0, FfiDirection::RightToLeft)
                },
                _ => false,
            },
            PseudoClassType::State => pseudo_class.identifier.as_ref().is_some_and(|identifier| unsafe {
                selector_ffi_matches_state(element.0, ffi_string_view(identifier))
            }),
            PseudoClassType::Heading => unsafe {
                selector_ffi_matches_heading(element.0, pseudo_class.levels.as_ptr(), pseudo_class.levels.len())
            },
            _ => unsafe { selector_ffi_matches_pseudo_class(element.0, pseudo_class.pseudo_class as u8) },
        }
    }

    fn parent_element(&mut self, element: FfiElement, shadow_host: Option<FfiElement>) -> Option<FfiElement> {
        ffi_element(unsafe {
            selector_ffi_parent_element(element.0, shadow_host.map_or(std::ptr::null(), |host| host.0))
        })
    }

    fn parent_element_in_light_tree(&mut self, element: FfiElement) -> Option<FfiElement> {
        ffi_element(unsafe { selector_ffi_parent_element_in_light_tree(element.0) })
    }

    fn previous_element_sibling(&mut self, element: FfiElement) -> Option<FfiElement> {
        ffi_element(unsafe { selector_ffi_previous_element_sibling(element.0) })
    }

    fn next_element_sibling(&mut self, element: FfiElement) -> Option<FfiElement> {
        ffi_element(unsafe { selector_ffi_next_element_sibling(element.0) })
    }

    fn first_element_child(&mut self, element: FfiElement) -> Option<FfiElement> {
        ffi_element(unsafe { selector_ffi_first_element_child(element.0) })
    }

    fn last_element_child(&mut self, element: FfiElement) -> Option<FfiElement> {
        ffi_element(unsafe { selector_ffi_last_element_child(element.0) })
    }

    fn first_element_descendant(&mut self, element: FfiElement) -> Option<FfiElement> {
        ffi_element(unsafe { selector_ffi_first_element_descendant(element.0) })
    }

    fn next_element_descendant(&mut self, element: FfiElement, root: FfiElement) -> Option<FfiElement> {
        ffi_element(unsafe { selector_ffi_next_element_descendant(element.0, root.0) })
    }

    fn has_no_element_or_nonempty_text_children(&mut self, element: FfiElement) -> bool {
        unsafe { selector_ffi_has_no_element_or_nonempty_text_children(element.0) }
    }

    fn has_same_type(&mut self, first: FfiElement, second: FfiElement) -> bool {
        unsafe { selector_ffi_has_same_type(first.0, second.0) }
    }

    fn is_document_root(&mut self, element: FfiElement) -> bool {
        unsafe { selector_ffi_is_document_root(element.0) }
    }

    fn slotted_parent(&mut self, element: FfiElement) -> Option<(FfiElement, Option<FfiElement>)> {
        ffi_element_and_shadow_host(unsafe { selector_ffi_slotted_parent(self.context, element.0) })
    }

    fn part_parent(
        &mut self,
        element: FfiElement,
        identifiers: &[SelectorString],
        allow_same_shadow_root_scope: bool,
        shadow_host: Option<FfiElement>,
    ) -> Option<(FfiElement, Option<FfiElement>)> {
        let identifiers = identifiers
            .iter()
            .map(|identifier| ffi_string_view(identifier))
            .collect::<Vec<_>>();
        ffi_element_and_shadow_host(unsafe {
            selector_ffi_part_parent(
                self.context,
                element.0,
                identifiers.as_ptr(),
                identifiers.len(),
                allow_same_shadow_root_scope,
                shadow_host.map_or(std::ptr::null(), |host| host.0),
            )
        })
    }

    fn note_structural_pseudo_class(&mut self, element: FfiElement, pseudo_class: PseudoClassType) {
        unsafe { selector_ffi_note_structural_pseudo_class(self.context, element.0, pseudo_class as u8) }
    }

    fn note_has_pseudo_class(&mut self, element: FfiElement) {
        unsafe { selector_ffi_note_has_pseudo_class(self.context, element.0) }
    }

    fn note_sibling_combinator(
        &mut self,
        element: FfiElement,
        combinator: Combinator,
        sibling_invalidation_distance: usize,
    ) {
        unsafe {
            selector_ffi_note_sibling_combinator(
                self.context,
                element.0,
                ffi_combinator(combinator),
                sibling_invalidation_distance,
            );
        }
    }

    fn note_has_sibling_combinator_anchor(&mut self, anchor: FfiElement) {
        unsafe { selector_ffi_note_has_sibling_combinator_anchor(self.context, anchor.0) }
    }

    fn note_has_sibling_combinator_element(&mut self, element: FfiElement) {
        unsafe { selector_ffi_note_has_sibling_combinator_element(self.context, element.0) }
    }

    fn note_has_scope_element(&mut self, element: FfiElement) {
        unsafe { selector_ffi_note_has_scope_element(self.context, element.0) }
    }

    fn collects_selector_involvement_metadata(&mut self) -> bool {
        unsafe { selector_ffi_collects_selector_involvement_metadata(self.context) }
    }

    fn enter_has_argument_matching(&mut self) -> bool {
        let previous_value = unsafe { selector_ffi_inside_has_argument(self.context) };
        unsafe { selector_ffi_set_inside_has_argument(self.context, true) };
        previous_value
    }

    fn leave_has_argument_matching(&mut self, previous_value: bool) {
        unsafe { selector_ffi_set_inside_has_argument(self.context, previous_value) }
    }

    fn has_cache_get(&mut self, selector_id: u64, anchor: FfiElement) -> Option<bool> {
        match unsafe { selector_ffi_has_cache_get(self.context, selector_id, anchor.0) } {
            0 => None,
            1 => Some(false),
            2 => Some(true),
            value => panic!("invalid :has() cache value {value}"),
        }
    }

    fn has_cache_set(&mut self, selector_id: u64, anchor: FfiElement, result: bool) {
        unsafe { selector_ffi_has_cache_set(self.context, selector_id, anchor.0, result) }
    }

    fn should_reject_has_argument(&mut self, selector: &CompiledSelector, anchor: FfiElement) -> bool {
        unsafe { selector_ffi_should_reject_has_argument(self.context, selector.cxx_selector(), anchor.0) }
    }
}

impl From<FfiCombinator> for Combinator {
    fn from(value: FfiCombinator) -> Self {
        match value {
            FfiCombinator::None => Self::None,
            FfiCombinator::ImmediateChild => Self::ImmediateChild,
            FfiCombinator::Descendant => Self::Descendant,
            FfiCombinator::NextSibling => Self::NextSibling,
            FfiCombinator::SubsequentSibling => Self::SubsequentSibling,
            FfiCombinator::Column => Self::Column,
            FfiCombinator::PseudoElement => Self::PseudoElement,
        }
    }
}

impl From<FfiNamespaceType> for NamespaceType {
    fn from(value: FfiNamespaceType) -> Self {
        match value {
            FfiNamespaceType::Default => Self::Default,
            FfiNamespaceType::None => Self::None,
            FfiNamespaceType::Any => Self::Any,
            FfiNamespaceType::Named => Self::Named,
        }
    }
}

impl From<FfiAttributeMatchType> for AttributeMatchType {
    fn from(value: FfiAttributeMatchType) -> Self {
        match value {
            FfiAttributeMatchType::HasAttribute => Self::HasAttribute,
            FfiAttributeMatchType::ExactValue => Self::ExactValue,
            FfiAttributeMatchType::ContainsWord => Self::ContainsWord,
            FfiAttributeMatchType::ContainsString => Self::ContainsString,
            FfiAttributeMatchType::StartsWithSegment => Self::StartsWithSegment,
            FfiAttributeMatchType::StartsWithString => Self::StartsWithString,
            FfiAttributeMatchType::EndsWithString => Self::EndsWithString,
        }
    }
}

impl From<FfiAttributeCaseType> for AttributeCaseType {
    fn from(value: FfiAttributeCaseType) -> Self {
        match value {
            FfiAttributeCaseType::Default => Self::Default,
            FfiAttributeCaseType::Sensitive => Self::Sensitive,
            FfiAttributeCaseType::Insensitive => Self::Insensitive,
        }
    }
}

fn pseudo_class_from_ffi(value: u8) -> PseudoClassType {
    use PseudoClassType::*;
    match value {
        0 => Active,
        1 => AnyLink,
        2 => Autofill,
        3 => Buffering,
        4 => Checked,
        5 => Default,
        6 => Defined,
        7 => Dir,
        8 => Disabled,
        9 => Empty,
        10 => Enabled,
        11 => EvenLessGoodValue,
        12 => FirstChild,
        13 => FirstOfType,
        14 => Focus,
        15 => FocusVisible,
        16 => FocusWithin,
        17 => Fullscreen,
        18 => Has,
        19 => Heading,
        20 => HighValue,
        21 => Host,
        22 => Hover,
        23 => Indeterminate,
        24 => Invalid,
        25 => Is,
        26 => Lang,
        27 => LastChild,
        28 => LastOfType,
        29 => Link,
        30 => LocalLink,
        31 => LowValue,
        32 => Modal,
        33 => Muted,
        34 => Not,
        35 => NthChild,
        36 => NthLastChild,
        37 => NthLastOfType,
        38 => NthOfType,
        39 => OnlyChild,
        40 => OnlyOfType,
        41 => Open,
        42 => OptimalValue,
        43 => Optional,
        44 => PopoverOpen,
        45 => Paused,
        46 => PlaceholderShown,
        47 => Playing,
        48 => ReadOnly,
        49 => ReadWrite,
        50 => Required,
        51 => Root,
        52 => Scope,
        53 => Seeking,
        54 => Stalled,
        55 => State,
        56 => SuboptimalValue,
        57 => Target,
        58 => Unchecked,
        59 => UserInvalid,
        60 => UserValid,
        61 => Valid,
        62 => Visited,
        63 => VolumeLocked,
        64 => Where,
        _ => panic!("invalid pseudo-class {value}"),
    }
}

fn pseudo_element_from_ffi(value: u8) -> PseudoElementType {
    use PseudoElementType::*;
    match value {
        0 => After,
        1 => Backdrop,
        2 => Before,
        3 => FirstLetter,
        4 => FirstLine,
        5 => Marker,
        6 => Selection,
        7 => ViewTransition,
        8 => DetailsContent,
        9 => FileSelectorButton,
        10 => Placeholder,
        11 => SliderFill,
        12 => SliderThumb,
        13 => SliderTrack,
        14 => Part,
        15 => Slotted,
        16 => ViewTransitionGroup,
        17 => ViewTransitionImagePair,
        18 => ViewTransitionNew,
        19 => ViewTransitionOld,
        20 => UnknownWebKit,
        _ => panic!("invalid pseudo-element {value}"),
    }
}

unsafe fn ffi_slice<'a, T>(data: *const T, length: usize) -> &'a [T] {
    if length == 0 {
        return &[];
    }
    assert!(!data.is_null());
    unsafe { std::slice::from_raw_parts(data, length) }
}

unsafe fn string_from_ffi(value: FfiStringView) -> SelectorString {
    unsafe { ffi_slice(value.data, value.length) }.into()
}

fn qualified_name_from_ffi(selector: &FfiSimpleSelector) -> QualifiedName {
    QualifiedName {
        namespace_type: selector.namespace_type.into(),
        namespace: unsafe { string_from_ffi(selector.namespace) },
        name: unsafe { string_from_ffi(selector.name) },
        lowercase_name: unsafe { string_from_ffi(selector.lowercase_name) },
    }
}

unsafe fn selector_from_handle(handle: *const RustSelector) -> Arc<CompiledSelector> {
    assert!(!handle.is_null());
    unsafe { (*handle).selector.clone() }
}

unsafe fn simple_selector_from_ffi(selector: &FfiSimpleSelector) -> SimpleSelector {
    match selector.selector_type {
        FfiSimpleSelectorType::Universal => SimpleSelector::Universal(qualified_name_from_ffi(selector)),
        FfiSimpleSelectorType::TagName => SimpleSelector::TagName(qualified_name_from_ffi(selector)),
        FfiSimpleSelectorType::Id => SimpleSelector::Id(unsafe { string_from_ffi(selector.name) }),
        FfiSimpleSelectorType::Class => SimpleSelector::Class(unsafe { string_from_ffi(selector.name) }),
        FfiSimpleSelectorType::Attribute => SimpleSelector::Attribute(AttributeSelector {
            match_type: selector.attribute_match_type.into(),
            qualified_name: qualified_name_from_ffi(selector),
            value: unsafe { string_from_ffi(selector.attribute_value) },
            case_type: selector.attribute_case_type.into(),
        }),
        FfiSimpleSelectorType::PseudoClass => {
            let argument_selector_list = unsafe {
                ffi_slice(selector.argument_selectors, selector.argument_selector_count)
                    .iter()
                    .map(|handle| selector_from_handle(*handle))
                    .collect::<Vec<_>>()
                    .into_boxed_slice()
            };
            let languages = unsafe {
                ffi_slice(selector.languages, selector.language_count)
                    .iter()
                    .map(|language| string_from_ffi(*language))
                    .collect::<Vec<_>>()
                    .into_boxed_slice()
            };
            let direction = match selector.direction {
                FfiDirection::None => None,
                FfiDirection::LeftToRight => Some(Direction::LeftToRight),
                FfiDirection::RightToLeft => Some(Direction::RightToLeft),
                FfiDirection::Other => Some(Direction::Other),
            };
            let identifier = (selector.identifier.length != 0).then(|| unsafe { string_from_ffi(selector.identifier) });
            let levels = unsafe { ffi_slice(selector.levels, selector.level_count) }.into();

            SimpleSelector::PseudoClass(PseudoClassSelector {
                pseudo_class: pseudo_class_from_ffi(selector.pseudo_class),
                an_plus_b_pattern: AnPlusBPattern {
                    step_size: selector.an_plus_b_step_size,
                    offset: selector.an_plus_b_offset,
                },
                argument_selector_list,
                languages,
                direction,
                identifier,
                levels,
            })
        }
        FfiSimpleSelectorType::PseudoElement => {
            let value = match selector.pseudo_element_value_type {
                FfiPseudoElementValueType::None => PseudoElementValue::None,
                FfiPseudoElementValueType::CompoundSelector => PseudoElementValue::CompoundSelector(unsafe {
                    selector_from_handle(selector.pseudo_element_selector)
                }),
                FfiPseudoElementValueType::Identifiers => PseudoElementValue::Identifiers(unsafe {
                    ffi_slice(
                        selector.pseudo_element_identifiers,
                        selector.pseudo_element_identifier_count,
                    )
                    .iter()
                    .map(|identifier| string_from_ffi(*identifier))
                    .collect::<Vec<_>>()
                    .into_boxed_slice()
                }),
                FfiPseudoElementValueType::TransitionName => PseudoElementValue::TransitionName {
                    is_universal: selector.transition_name_is_universal,
                    value: unsafe { string_from_ffi(selector.transition_name) },
                },
            };
            SimpleSelector::PseudoElement(PseudoElementSelector {
                pseudo_element: pseudo_element_from_ffi(selector.pseudo_element),
                value,
            })
        }
        FfiSimpleSelectorType::Nesting => SimpleSelector::Nesting,
        FfiSimpleSelectorType::Invalid => SimpleSelector::Invalid,
    }
}

unsafe fn compiled_selector_from_ffi(selector: &FfiSelector) -> Arc<CompiledSelector> {
    let compound_selectors = unsafe { ffi_slice(selector.compound_selectors, selector.compound_selector_count) }
        .iter()
        .map(|compound| CompoundSelector {
            combinator: compound.combinator.into(),
            simple_selectors: unsafe { ffi_slice(compound.simple_selectors, compound.simple_selector_count) }
                .iter()
                .map(|simple| unsafe { simple_selector_from_ffi(simple) })
                .collect::<Vec<_>>()
                .into_boxed_slice(),
        })
        .collect::<Vec<_>>()
        .into_boxed_slice();
    CompiledSelector::new_with_cxx_selector(compound_selectors, selector.cxx_selector)
}

/// # Safety
/// `selector` and all transitively referenced arrays and strings must remain valid for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_create(selector: *const FfiSelector) -> *mut RustSelector {
    abort_on_panic(|| {
        assert!(!selector.is_null());
        let selector = unsafe { compiled_selector_from_ffi(&*selector) };
        Box::into_raw(Box::new(RustSelector { selector }))
    })
}

/// # Safety
/// `selector` must be a pointer returned by `rust_selector_create` that has not already been destroyed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_destroy(selector: *mut RustSelector) {
    abort_on_panic(|| {
        if !selector.is_null() {
            drop(unsafe { Box::from_raw(selector) });
        }
    });
}

/// # Safety
/// All pointers must remain valid for this call. `element` must point to a C++
/// DOM element and `context` must point to a C++ Rust matching context.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_matches(
    selector: *const RustSelector,
    element: *const c_void,
    pseudo_element: u8,
    shadow_host: *const c_void,
    context: *mut c_void,
    scope: *const c_void,
) -> bool {
    abort_on_panic(|| {
        assert!(!selector.is_null());
        assert!(!element.is_null());
        assert!(!context.is_null());
        let target = MatchTarget {
            element: FfiElement(element),
            pseudo_element: (pseudo_element != u8::MAX).then(|| pseudo_element_from_ffi(pseudo_element)),
        };
        // NB: Relative matching with an anchor only happens inside :has() argument evaluation,
        //     which stays within the Rust engine, so the entry point always matches normally.
        matches_selector_internal(
            unsafe { &(*selector).selector },
            target,
            ffi_element(shadow_host),
            ffi_element(scope),
            SelectorKind::Normal,
            None,
            &mut FfiDom { context },
        )
    })
}

/// # Safety
/// All pointers must remain valid for this call. `element` must point to a C++
/// DOM element and `context` must point to a C++ Rust matching context.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_matches_originating_element(
    selector: *const RustSelector,
    pseudo_element: u8,
    element: *const c_void,
    shadow_host: *const c_void,
    context: *mut c_void,
    scope: *const c_void,
) -> bool {
    abort_on_panic(|| {
        assert!(!selector.is_null());
        assert!(!element.is_null());
        assert!(!context.is_null());
        matches_originating_element_for_pseudo_element(
            unsafe { &(*selector).selector },
            pseudo_element_from_ffi(pseudo_element),
            FfiElement(element),
            ffi_element(shadow_host),
            ffi_element(scope),
            &mut FfiDom { context },
        )
    })
}

fn can_simple_selector_use_fast_matches(simple_selector: &SimpleSelector) -> bool {
    match simple_selector {
        SimpleSelector::Universal(_)
        | SimpleSelector::TagName(_)
        | SimpleSelector::Id(_)
        | SimpleSelector::Class(_)
        | SimpleSelector::Attribute(_) => true,
        SimpleSelector::PseudoClass(selector) => matches!(
            selector.pseudo_class,
            PseudoClassType::Active
                | PseudoClassType::AnyLink
                | PseudoClassType::Autofill
                | PseudoClassType::Checked
                | PseudoClassType::Disabled
                | PseudoClassType::Empty
                | PseudoClassType::Enabled
                | PseudoClassType::FirstChild
                | PseudoClassType::Focus
                | PseudoClassType::FocusVisible
                | PseudoClassType::FocusWithin
                | PseudoClassType::Hover
                | PseudoClassType::LastChild
                | PseudoClassType::Link
                | PseudoClassType::LocalLink
                | PseudoClassType::OnlyChild
                | PseudoClassType::Root
                | PseudoClassType::State
                | PseudoClassType::Unchecked
                | PseudoClassType::Visited
        ),
        SimpleSelector::PseudoElement(_) | SimpleSelector::Nesting | SimpleSelector::Invalid => false,
    }
}

fn sibling_invalidation_distance(compound_selectors: &[CompoundSelector]) -> usize {
    let mut maximum_distance = 0usize;
    let mut current_distance = 0usize;

    for compound_selector in compound_selectors {
        match compound_selector.combinator {
            Combinator::None | Combinator::PseudoElement => {}
            Combinator::SubsequentSibling => return usize::MAX,
            Combinator::NextSibling => current_distance = current_distance.saturating_add(1),
            Combinator::ImmediateChild | Combinator::Descendant | Combinator::Column => {
                maximum_distance = maximum_distance.max(current_distance);
                current_distance = 0;
            }
        }
    }

    maximum_distance.max(current_distance)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    #[derive(Default)]
    struct TestNode {
        parent: Option<usize>,
        tag_name: &'static str,
        classes: &'static [&'static str],
    }

    #[derive(Default)]
    struct TestDom {
        nodes: Vec<TestNode>,
        has_cache: HashMap<(u64, usize), bool>,
        inside_has: bool,
    }

    impl TestDom {
        fn is_descendant_of(&self, element: usize, ancestor: usize) -> bool {
            let mut parent = self.nodes[element].parent;
            while let Some(candidate) = parent {
                if candidate == ancestor {
                    return true;
                }
                parent = self.nodes[candidate].parent;
            }
            false
        }

        fn children(&self, element: usize) -> impl DoubleEndedIterator<Item = usize> + '_ {
            self.nodes
                .iter()
                .enumerate()
                .filter_map(move |(index, node)| (node.parent == Some(element)).then_some(index))
        }
    }

    impl SelectorDom for TestDom {
        type Element = usize;

        fn matches_universal_selector(&mut self, _element: usize, _name: &QualifiedName) -> bool {
            true
        }

        fn matches_tag_name_selector(
            &mut self,
            element: usize,
            name: &QualifiedName,
            _mode: TagNameMatchingMode,
        ) -> bool {
            self.nodes[element]
                .tag_name
                .encode_utf16()
                .eq(name.lowercase_name.iter().copied())
        }

        fn matches_id_selector(&mut self, _element: usize, _id: &[u16]) -> bool {
            false
        }

        fn matches_class_selector(&mut self, element: usize, class_name: &[u16]) -> bool {
            self.nodes[element]
                .classes
                .iter()
                .any(|class| class.encode_utf16().eq(class_name.iter().copied()))
        }

        fn matches_attribute_selector(&mut self, _element: usize, _attribute: &AttributeSelector) -> bool {
            false
        }

        fn matches_pseudo_class_state(&mut self, _element: usize, _pseudo_class: &PseudoClassSelector) -> bool {
            false
        }

        fn parent_element(&mut self, element: usize, shadow_host: Option<usize>) -> Option<usize> {
            (Some(element) != shadow_host)
                .then(|| self.nodes[element].parent)
                .flatten()
        }

        fn parent_element_in_light_tree(&mut self, element: usize) -> Option<usize> {
            self.nodes[element].parent
        }

        fn previous_element_sibling(&mut self, element: usize) -> Option<usize> {
            let parent = self.nodes[element].parent?;
            self.children(parent).take_while(|sibling| *sibling != element).last()
        }

        fn next_element_sibling(&mut self, element: usize) -> Option<usize> {
            let parent = self.nodes[element].parent?;
            self.children(parent).skip_while(|sibling| *sibling != element).nth(1)
        }

        fn first_element_child(&mut self, element: usize) -> Option<usize> {
            self.children(element).next()
        }

        fn last_element_child(&mut self, element: usize) -> Option<usize> {
            self.children(element).next_back()
        }

        fn first_element_descendant(&mut self, element: usize) -> Option<usize> {
            (0..self.nodes.len()).find(|candidate| self.is_descendant_of(*candidate, element))
        }

        fn next_element_descendant(&mut self, element: usize, root: usize) -> Option<usize> {
            ((element + 1)..self.nodes.len()).find(|candidate| self.is_descendant_of(*candidate, root))
        }

        fn has_no_element_or_nonempty_text_children(&mut self, element: usize) -> bool {
            self.children(element).next().is_none()
        }

        fn has_same_type(&mut self, first: usize, second: usize) -> bool {
            self.nodes[first].tag_name == self.nodes[second].tag_name
        }

        fn is_document_root(&mut self, element: usize) -> bool {
            element == 0
        }

        fn slotted_parent(&mut self, _element: usize) -> Option<(usize, Option<usize>)> {
            None
        }

        fn part_parent(
            &mut self,
            _element: usize,
            _identifiers: &[SelectorString],
            _allow_same_shadow_root_scope: bool,
            _shadow_host: Option<usize>,
        ) -> Option<(usize, Option<usize>)> {
            None
        }

        fn note_structural_pseudo_class(&mut self, _element: usize, _pseudo_class: PseudoClassType) {}
        fn note_has_pseudo_class(&mut self, _element: usize) {}
        fn note_sibling_combinator(&mut self, _element: usize, _combinator: Combinator, _distance: usize) {}
        fn note_has_sibling_combinator_anchor(&mut self, _anchor: usize) {}
        fn note_has_sibling_combinator_element(&mut self, _element: usize) {}
        fn note_has_scope_element(&mut self, _element: usize) {}

        fn collects_selector_involvement_metadata(&mut self) -> bool {
            false
        }

        fn enter_has_argument_matching(&mut self) -> bool {
            let previous_value = self.inside_has;
            self.inside_has = true;
            previous_value
        }

        fn leave_has_argument_matching(&mut self, previous_value: bool) {
            self.inside_has = previous_value;
        }

        fn has_cache_get(&mut self, selector_id: u64, anchor: usize) -> Option<bool> {
            self.has_cache.get(&(selector_id, anchor)).copied()
        }

        fn has_cache_set(&mut self, selector_id: u64, anchor: usize, result: bool) {
            self.has_cache.insert((selector_id, anchor), result);
        }

        fn should_reject_has_argument(&mut self, _selector: &CompiledSelector, _anchor: usize) -> bool {
            false
        }
    }

    fn class(name: &str) -> SimpleSelector {
        SimpleSelector::Class(name.encode_utf16().collect())
    }

    fn compound(combinator: Combinator, simple_selectors: Vec<SimpleSelector>) -> CompoundSelector {
        CompoundSelector {
            combinator,
            simple_selectors: simple_selectors.into_boxed_slice(),
        }
    }

    fn selector(compound_selectors: Vec<CompoundSelector>) -> Arc<CompiledSelector> {
        CompiledSelector::new(compound_selectors.into_boxed_slice())
    }

    fn test_tree() -> TestDom {
        TestDom {
            nodes: vec![
                TestNode {
                    tag_name: "html",
                    ..Default::default()
                },
                TestNode {
                    parent: Some(0),
                    tag_name: "div",
                    classes: &["anchor", "outer"],
                },
                TestNode {
                    parent: Some(1),
                    tag_name: "section",
                    classes: &["hit", "middle"],
                },
                TestNode {
                    parent: Some(2),
                    tag_name: "span",
                    classes: &["hit", "target"],
                },
                TestNode {
                    parent: Some(1),
                    tag_name: "span",
                    classes: &["hit"],
                },
            ],
            ..Default::default()
        }
    }

    fn selector_with_combinators(combinators: &[Combinator]) -> Arc<CompiledSelector> {
        let compounds = combinators
            .iter()
            .map(|combinator| CompoundSelector {
                combinator: *combinator,
                simple_selectors: vec![SimpleSelector::Id(Box::from([b'x' as u16]))].into_boxed_slice(),
            })
            .collect::<Vec<_>>()
            .into_boxed_slice();
        CompiledSelector::new(compounds)
    }

    #[test]
    fn an_plus_b_matching() {
        assert!(
            AnPlusBPattern {
                step_size: 2,
                offset: 1
            }
            .matches(5)
        );
        assert!(
            !AnPlusBPattern {
                step_size: 2,
                offset: 1
            }
            .matches(4)
        );
        assert!(
            AnPlusBPattern {
                step_size: -1,
                offset: 3
            }
            .matches(1)
        );
        assert!(
            !AnPlusBPattern {
                step_size: -1,
                offset: 3
            }
            .matches(4)
        );
        assert!(
            AnPlusBPattern {
                step_size: 0,
                offset: 2
            }
            .matches(2)
        );
        assert!(
            AnPlusBPattern {
                step_size: 2,
                offset: i32::MIN
            }
            .matches(2)
        );
        assert!(
            !AnPlusBPattern {
                step_size: 2,
                offset: i32::MIN
            }
            .matches(1)
        );
        assert!(
            AnPlusBPattern {
                step_size: i32::MIN,
                offset: 1
            }
            .matches(1)
        );
        assert!(
            !AnPlusBPattern {
                step_size: i32::MIN,
                offset: 1
            }
            .matches(2)
        );
        assert!(
            AnPlusBPattern {
                step_size: i32::MAX,
                offset: i32::MIN
            }
            .matches(i32::MAX - 1)
        );
    }

    #[test]
    fn computes_fast_match_eligibility() {
        let fast = selector_with_combinators(&[Combinator::None, Combinator::Descendant]);
        assert!(fast.can_use_fast_matches);

        let pseudo = CompiledSelector::new(Box::new([CompoundSelector {
            combinator: Combinator::None,
            simple_selectors: Box::new([SimpleSelector::PseudoClass(PseudoClassSelector::without_arguments(
                PseudoClassType::NthChild,
            ))]),
        }]));
        assert!(!pseudo.can_use_fast_matches);
    }

    #[test]
    fn computes_sibling_invalidation_distance() {
        let adjacent = selector_with_combinators(&[
            Combinator::None,
            Combinator::NextSibling,
            Combinator::NextSibling,
            Combinator::ImmediateChild,
        ]);
        assert_eq!(adjacent.sibling_invalidation_distance, 2);

        let subsequent = selector_with_combinators(&[Combinator::None, Combinator::SubsequentSibling]);
        assert_eq!(subsequent.sibling_invalidation_distance, usize::MAX);
    }

    #[test]
    fn assigns_unique_nonzero_ids() {
        let first = selector_with_combinators(&[Combinator::None]);
        let second = selector_with_combinators(&[Combinator::None]);
        assert_ne!(first.id(), 0);
        assert_ne!(first.id(), second.id());
        assert_eq!(first, second);
    }

    #[test]
    fn matches_combinators_with_descendant_backtracking() {
        let selector = selector(vec![
            compound(Combinator::None, vec![class("outer")]),
            compound(Combinator::ImmediateChild, vec![class("middle")]),
            compound(Combinator::Descendant, vec![class("target")]),
        ]);
        let mut dom = test_tree();
        assert!(matches_selector(
            &selector,
            MatchTarget {
                element: 3,
                pseudo_element: None,
            },
            None,
            None,
            &mut dom,
        ));
        assert!(!matches_selector(
            &selector,
            MatchTarget {
                element: 4,
                pseudo_element: None,
            },
            None,
            None,
            &mut dom,
        ));
    }

    #[test]
    fn matches_relative_has_selectors() {
        let relative_selector = selector(vec![compound(Combinator::ImmediateChild, vec![class("hit")])]);
        let has = SimpleSelector::PseudoClass(PseudoClassSelector {
            pseudo_class: PseudoClassType::Has,
            an_plus_b_pattern: AnPlusBPattern::default(),
            argument_selector_list: vec![relative_selector].into_boxed_slice(),
            languages: Box::new([]),
            direction: None,
            identifier: None,
            levels: Box::new([]),
        });
        let selector = selector(vec![compound(Combinator::None, vec![class("anchor"), has])]);
        let mut dom = test_tree();
        assert!(matches_selector(
            &selector,
            MatchTarget {
                element: 1,
                pseudo_element: None,
            },
            None,
            None,
            &mut dom,
        ));
        assert!(!dom.inside_has);
    }

    #[test]
    fn matches_nth_child_of_selector_lists() {
        let of_selector = selector(vec![compound(Combinator::None, vec![class("hit")])]);
        let nth_child = SimpleSelector::PseudoClass(PseudoClassSelector {
            pseudo_class: PseudoClassType::NthChild,
            an_plus_b_pattern: AnPlusBPattern {
                step_size: 0,
                offset: 2,
            },
            argument_selector_list: vec![of_selector].into_boxed_slice(),
            languages: Box::new([]),
            direction: None,
            identifier: None,
            levels: Box::new([]),
        });
        let selector = selector(vec![compound(Combinator::None, vec![nth_child])]);
        let mut dom = test_tree();
        assert!(matches_selector(
            &selector,
            MatchTarget {
                element: 4,
                pseudo_element: None,
            },
            None,
            None,
            &mut dom,
        ));
    }
}
