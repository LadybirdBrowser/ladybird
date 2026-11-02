/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Selector matching for the DOM query APIs (`querySelector()`, `querySelectorAll()`, `matches()` and `closest()`),
//! against the DOM itself.
//!
//! A query's selectors are matched by the one selector evaluator with the live DOM as its subject, and a query plan
//! holds what its selectors tell it ahead of walking. A query reads the DOM where it stands: nothing is mirrored for
//! it and the style engine is not consulted. Every fact a selector tests is asked of the DOM through the callbacks the
//! host passes in.

use crate::css::ffi_support::OwnedBox;
use std::convert::Infallible;
use std::ffi::c_void;

use smallvec::SmallVec;

use super::css_tokenizer::TokenizerInput;
use super::ffi_support::FfiUtf16View;
use super::selector::RustSelector;
use super::selector::SimpleSelector;
use super::style::fast_hash::FastMap as HashMap;
use super::style::index::StyleAtomID;
use super::style::input_types::StateFact;
use super::style::selector::NthPosition;
use super::style::selector_evaluation::ElementFeatures;
use super::style::selector_evaluation::SelectorBindings;
use super::style::selector_evaluation::SelectorEvaluator;
use super::style::selector_evaluation::SelectorSubject;
use super::style::selector_evaluation::SelectorTree;

/// What a selector compares of one element: its names, as interned string identities, and how many of its attributes
/// have a local name the matcher asked for.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiDomElement {
    pub local_name: usize,
    /// Zero for the null namespace.
    pub namespace_uri: usize,
    /// Zero for an element with no id.
    pub id: usize,
    pub classes: *const usize,
    pub class_count: usize,
    pub attribute_count: usize,
}

/// One attribute of an element, borrowed from the DOM until it next changes.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiDomAttribute<'a> {
    pub local_name: usize,
    /// Zero for the null namespace.
    pub namespace_uri: usize,
    pub value: FfiUtf16View<'a>,
}

/// Which of an element's child indices a child-indexed pseudo-class counts with.
#[repr(u8)]
#[derive(Clone, Copy)]
pub enum FfiChildIndex {
    FromStart,
    FromEnd,
    /// Among the siblings with the element's local name and namespace.
    OfTypeFromStart,
    OfTypeFromEnd,
}

/// What the matcher asks of the DOM. Every node pointer is a live node for the duration of the query, and every node a
/// callback returns is one too, or null for none.
#[repr(C)]
pub struct FfiDomSelectorCallbacks {
    /// Writes the element to `facts`, and the first `capacity` of its attributes whose local name is one of the
    /// `name_count` in `names` to `attributes`.
    pub element: unsafe extern "C" fn(
        element: *const c_void,
        names: *const usize,
        name_count: usize,
        attributes: *mut FfiDomAttribute,
        capacity: usize,
        facts: &mut FfiDomElement,
    ),
    /// Whether two elements have the same local name and namespace.
    pub same_type: unsafe extern "C" fn(element: *const c_void, other: *const c_void) -> bool,
    /// The node's parent as selectors see it: its parent element, or the shadow root it is a child of. Null for a
    /// shadow root, and for a child of a document or a fragment.
    pub parent: unsafe extern "C" fn(node: *const c_void) -> *const c_void,
    pub previous_element_sibling: unsafe extern "C" fn(node: *const c_void) -> *const c_void,
    pub next_element_sibling: unsafe extern "C" fn(node: *const c_void) -> *const c_void,
    pub first_element_child: unsafe extern "C" fn(node: *const c_void) -> *const c_void,
    /// The first element child of the node's parent, whatever node that is, or the node itself when it has none.
    pub first_element_sibling: unsafe extern "C" fn(node: *const c_void) -> *const c_void,
    /// The element's 1-based index among its inclusive element siblings, as `which` counts them.
    pub child_index: unsafe extern "C" fn(element: *const c_void, which: FfiChildIndex) -> u32,
    /// The first element after `node` in tree order that is a descendant of `root` and, unless `local_name` is zero,
    /// has that local name, and unless `class_name` is zero, has that class, skipping the subtrees whose name filter
    /// lacks one of the bits of `names`. `node` is `root` itself to start. Both may be any node.
    pub next_element_in_subtree: unsafe extern "C" fn(
        node: *const c_void,
        root: *const c_void,
        local_name: usize,
        class_name: usize,
        names: u64,
    ) -> *const c_void,
    /// The bit of the DOM's filter of the attribute names and classes in a subtree for a name.
    pub name_filter_bit: unsafe extern "C" fn(local_name: usize) -> u64,
    /// The name filter of the whole tree `node` is in, with every bit set where no one element holds that tree.
    pub tree_name_filter: unsafe extern "C" fn(node: *const c_void) -> u64,
    /// The shadow root the element hosts, or null.
    pub shadow_root: unsafe extern "C" fn(element: *const c_void) -> *const c_void,
    /// The host of a shadow root.
    pub host: unsafe extern "C" fn(shadow_root: *const c_void) -> *const c_void,
    /// Whether the element's id (or one of its classes, when `is_class` is set) is `name`, compared ASCII
    /// case-insensitively.
    pub id_or_class_equals_ignoring_ascii_case:
        unsafe extern "C" fn(element: *const c_void, is_class: bool, name: usize) -> bool,
    /// Whether the element is in a state, by the state's `StateFact` value.
    pub matches_state: unsafe extern "C" fn(element: *const c_void, state: u8) -> bool,
    /// The element's resolved language tag, or an empty view when it has none. Only valid until the next callback, which
    /// the caller ensures, as no Rust lifetime names that.
    pub language: unsafe extern "C" fn(element: *const c_void) -> FfiUtf16View<'static>,
    /// The interned identity of the element's directionality: `ltr` or `rtl`.
    pub directionality: unsafe extern "C" fn(element: *const c_void) -> usize,
    /// The element's heading level, or zero when it is not a heading.
    pub heading_level: unsafe extern "C" fn(element: *const c_void) -> u32,
    pub has_custom_state: unsafe extern "C" fn(element: *const c_void, state: usize) -> bool,
    /// Whether no child of the element keeps it from being `:empty`.
    pub is_empty: unsafe extern "C" fn(element: *const c_void) -> bool,
    /// Whether the element is the document element of its document.
    pub is_document_element: unsafe extern "C" fn(element: *const c_void) -> bool,
}

/// One selector query: the compiled selectors, and the context `:scope` and `:host` are resolved in.
#[repr(C)]
pub struct FfiDomSelectorQuery {
    pub program: *const DomSelectorProgram,
    pub callbacks: *const FfiDomSelectorCallbacks,
    /// The element `:scope` names: the element the query is scoped to, or the document element for a query scoped to
    /// a document, a shadow root or a fragment. Null for none.
    pub scope: *const c_void,
    /// The shadow root of the tree the query is made in, or null outside one.
    pub shadow_root: *const c_void,
    /// Whether ids and classes compare ASCII case-insensitively, as they do in a quirks-mode document.
    pub ids_and_classes_ignore_case: bool,
}

/// Where the matches of a query lie relative to the element with the id it names.
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum FfiDomIdLookupKind {
    /// The query names no id its matches can be found by.
    None,
    /// Every match has the id.
    Subject,
    /// Every match is a descendant of the element with the id.
    Ancestor,
    /// Every match is a descendant of the parent of the element with the id, which is a sibling of an ancestor of
    /// the match, or of the match itself.
    AncestorsParent,
}

/// An id a query's matches can be found by, which the host looks up in the id map of the tree it queries instead
/// of walking the whole tree.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiDomIdLookup {
    /// The raw identity of the id, or zero for `FfiDomIdLookupKind::None`.
    pub id: usize,
    pub kind: FfiDomIdLookupKind,
}

impl FfiDomIdLookup {
    const NONE: Self = Self {
        id: 0,
        kind: FfiDomIdLookupKind::None,
    };

    /// The id the one selector of a query names nearest its subject, as long as only descendant, child and sibling
    /// combinators lie between them. Only the combinator right of the compound with the id decides how its element
    /// relates to the matches: past a descendant or child combinator the matches are its descendants, and past a
    /// sibling combinator they are descendants of its parent.
    fn of(selector: crate::css::selector::Selector<'_>, raw: impl Fn(StyleAtomID) -> usize) -> Self {
        use crate::css::selector::Combinator;
        let id_of = |compound: crate::css::selector::Selector<'_>| {
            compound
                .first_compound()
                .simple_selectors
                .iter()
                .find_map(|simple| match simple {
                    SimpleSelector::Id(name) => Some(raw(name.name)),
                    _ => None,
                })
                .filter(|&id| id != 0)
        };
        if let Some(id) = id_of(selector) {
            return Self {
                id,
                kind: FfiDomIdLookupKind::Subject,
            };
        }
        let mut current = selector;
        while let Some((combinator, left)) = current.left() {
            let kind = match combinator {
                Combinator::Descendant | Combinator::ImmediateChild => FfiDomIdLookupKind::Ancestor,
                Combinator::NextSibling | Combinator::SubsequentSibling => FfiDomIdLookupKind::AncestorsParent,
                _ => return Self::NONE,
            };
            if left.is_relative_anchor() {
                return Self::NONE;
            }
            if let Some(id) = id_of(left) {
                return Self { id, kind };
            }
            current = left;
        }
        Self::NONE
    }
}

type DomNode = *const c_void;

/// The raw identities of the names the subject of a relative selector requires, each with whether it is an id or a
/// class, which a quirks-mode document compares ignoring case.
type SubjectNames = Box<[(bool, usize)]>;

/// How many bits the filter of the names in an anchor's subtree has.
const SUBTREE_NAME_FILTER_WORDS: usize = 64;

/// The names of the elements in the subtree of a `:has()` anchor, as bits of a bloom filter.
type SubtreeNameFilter = std::rc::Rc<[u64; SUBTREE_NAME_FILTER_WORDS]>;

/// The two bits of a subtree name filter a name sets.
fn subtree_name_filter_bits(raw: usize) -> [usize; 2] {
    let hash = (raw as u64).wrapping_mul(0x9E37_79B9_7F4A_7C15);
    let bits = (SUBTREE_NAME_FILTER_WORDS * 64) as u64;
    [((hash >> 32) % bits) as usize, ((hash >> 16) % bits) as usize]
}

fn optional_node(node: *const c_void) -> Option<DomNode> {
    (!node.is_null()).then_some(node)
}

/// A `:has()` argument list, with the names each of its selectors' subjects requires, if each requires any.
type DescendantList = (
    crate::css::style::selector_evaluation::SelectorListKey,
    Option<Box<[SubjectNames]>>,
);

/// A selector query compiled for one kind of document, as the host caches it with the query.
pub struct DomSelectorProgram {
    /// The raw identity of each name the selectors compare, by its atom, in an open-addressed table at most half full,
    /// whose size is a power of two: a name lies in the slot its atom's low bits name, or in the first free one after.
    raws: Box<[(StyleAtomID, usize)]>,
    /// Every attribute name the program's selectors compare, in either case: an element's row is read with them.
    attribute_names: Box<[usize]>,
    /// The roots of the entries that can name an element. A query names elements, and a pseudo-element is not one.
    /// The local name every subject requires its element to have, or zero.
    subject_local_name: usize,
    /// Every class each subject requires its element to have, as written.
    subject_classes: Box<[usize]>,
    /// The classes and attribute names the compounds of each selector name directly, each with whether it is a class.
    /// A selector matches only in a tree holding all of its names.
    names_in_the_tree: Box<[SubjectNames]>,
    /// The local names of the attributes every subject requires its element to carry.
    required_attribute_names: Box<[usize]>,
    /// Whether matching one element can ask the same question twice, which is only worth remembering answers for.
    lone_match_repeats_questions: bool,
    /// The `:has()` argument lists whose every selector starts with a descendant combinator, which match under no
    /// descendant of an anchor they do not match under: the elements a match under the descendant would be are all
    /// under the anchor too. Where every selector's subject requires a name, with those names by selector: such a list
    /// matches under no anchor whose subtree lacks a name of every selector.
    descendant_lists: Box<[DescendantList]>,
    /// The id a query of one selector finds its matches by.
    id_lookup: FfiDomIdLookup,
    /// The selectors the query matches by, and the HTML namespace's atom.
    sources: Box<[crate::css::selector::SelectorRef]>,
    html_namespace: Option<crate::css::style::AtomLease>,
}

impl DomSelectorProgram {
    /// The attribute names and the classes every match carries, as bits of the DOM's filter of names in a subtree. A
    /// class compared ignoring case can be written in another case than the element's.
    fn required_name_bits(&self, dom: &FfiDomSelectorCallbacks, ignore_case: bool) -> u64 {
        let classes: &[usize] = if ignore_case { &[] } else { &self.subject_classes };
        self.required_attribute_names
            .iter()
            .chain(classes)
            .fold(0, |bits, &name| bits | unsafe { (dom.name_filter_bit)(name) })
    }
}

fn raw_of(atom: StyleAtomID) -> usize {
    crate::css::style::process_atoms().local_raw_of(atom).unwrap_or(0)
}

/// What the selectors of a query name, anywhere in them.
#[derive(Default)]
struct QueryNames {
    /// The raw identity of every name the query compares, which it looks up once here.
    raws: Vec<(StyleAtomID, usize)>,
    attribute_names: Vec<usize>,
    /// A walk up or back tests each element it passes once, and so does a count of the siblings `of S` matches, unless
    /// what it tests reaches beyond the element it tests.
    lone_match_repeats_questions: bool,
    descendant_lists: Vec<DescendantList>,
}

fn collect_query_names(selectors: &[crate::css::selector::SelectorRef]) -> QueryNames {
    use crate::css::selector::PseudoClassType;
    let mut found = QueryNames::default();
    let QueryNames {
        raws,
        attribute_names,
        lone_match_repeats_questions,
        descendant_lists,
    } = &mut found;
    let atoms = crate::css::style::process_atoms();
    for selector in selectors {
        visit_query_selector(
            selector.view(),
            &mut |simple| {
                let names = match simple {
                    SimpleSelector::TagName(name) => Some(name.name),
                    SimpleSelector::Id(name) | SimpleSelector::Class(name) => Some(name.name),
                    SimpleSelector::Attribute(attribute) => Some(attribute.qualified_name.name),
                    SimpleSelector::PseudoClass(pseudo_class)
                        if matches!(pseudo_class.pseudo_class, PseudoClassType::Dir | PseudoClassType::State)
                            && !pseudo_class.identifier_identity.is_none() =>
                    {
                        Some(pseudo_class.identifier_identity)
                    }
                    _ => None,
                };
                for name in names.into_iter().flat_map(|name| [name, atoms.ascii_lowercase(name)]) {
                    if !raws.iter().any(|&(known, _)| known == name) {
                        raws.push((name, raw_of(name)));
                    }
                }
                if let SimpleSelector::PseudoClass(pseudo_class) = simple
                    && pseudo_class.pseudo_class == PseudoClassType::Has
                    && let Some(list) = pseudo_class.argument_selector_list.identity()
                    && !descendant_lists.iter().any(|&(known, _)| known == list)
                    && pseudo_class.argument_selector_list.iter().all(|relative| {
                        matches!(
                            relative.leftmost_compound().combinator,
                            crate::css::selector::Combinator::Descendant
                        )
                    })
                {
                    descendant_lists.push((list, names_required_under_anchor(pseudo_class.argument_selector_list)));
                }
                if let SimpleSelector::Attribute(attribute) = simple {
                    let name = attribute.qualified_name.name;
                    for name in [raw_of(name), raw_of(atoms.ascii_lowercase(name))] {
                        if !attribute_names.contains(&name) {
                            attribute_names.push(name);
                        }
                    }
                }
            },
            lone_match_repeats_questions,
        );
    }
    found
}

/// What the subject compound of every selector of a query tests directly, written as every element carrying it carries
/// it, which is what each match has.
#[derive(Default)]
struct SubjectRequirements {
    local_name: usize,
    classes: Vec<usize>,
    attribute_names: Vec<usize>,
}

/// Whether `attribute` names an attribute every element carrying it carries under that name, as the filter of names in
/// a subtree holds it.
fn names_a_filtered_attribute(attribute: &crate::css::selector::AttributeSelector<'_>) -> bool {
    attribute.qualified_name.name_is_ascii_lowercase
        && matches!(
            attribute.qualified_name.namespace_type,
            crate::css::selector::NamespaceType::Default | crate::css::selector::NamespaceType::None
        )
}

fn subject_requirements(selectors: &[crate::css::selector::SelectorRef]) -> SubjectRequirements {
    use crate::css::selector::PseudoClassType;
    let mut local_name = None;
    let mut classes: Option<Vec<usize>> = None;
    let mut attribute_names: Option<Vec<usize>> = None;
    for selector in selectors {
        let selector = selector.view();
        let mut required_local_name = None;
        let mut required_by_entry = SmallVec::<[usize; 4]>::new();
        let mut classes_of_entry = SmallVec::<[usize; 4]>::new();
        // NB: An `:is()` or `:where()` of one compound requires what the compound does.
        let mut subject_simples = SmallVec::<[SimpleSelector<'_>; 8]>::new();
        for simple in selector.first_compound().simple_selectors.iter() {
            if let SimpleSelector::PseudoClass(pseudo_class) = &simple
                && matches!(pseudo_class.pseudo_class, PseudoClassType::Is | PseudoClassType::Where)
            {
                if let Some(argument) = pseudo_class.argument_selector_list.single()
                    && argument.left().is_none()
                {
                    subject_simples.extend(argument.first_compound().simple_selectors.iter());
                }
                continue;
            }
            subject_simples.push(simple);
        }
        for simple in &subject_simples {
            match simple {
                SimpleSelector::TagName(name) if name.name_is_ascii_lowercase => {
                    required_local_name = Some(raw_of(name.name));
                }
                SimpleSelector::Attribute(attribute) if names_a_filtered_attribute(attribute) => {
                    required_by_entry.push(raw_of(attribute.qualified_name.name));
                }
                SimpleSelector::Class(name) => classes_of_entry.push(raw_of(name.name)),
                _ => {}
            }
        }
        local_name = match local_name {
            None => Some(required_local_name),
            Some(agreed) => Some(agreed.filter(|&agreed| Some(agreed) == required_local_name)),
        };
        match &mut attribute_names {
            None => attribute_names = Some(required_by_entry.into_vec()),
            Some(required) => required.retain(|name| required_by_entry.contains(name)),
        }
        match &mut classes {
            None => classes = Some(classes_of_entry.into_vec()),
            Some(classes) => classes.retain(|class| classes_of_entry.contains(class)),
        }
    }
    let mut classes = classes.unwrap_or_default();
    classes.retain(|&class| class != 0);
    SubjectRequirements {
        local_name: local_name.flatten().unwrap_or(0),
        classes,
        attribute_names: attribute_names.unwrap_or_default(),
    }
}

/// The classes and attribute names the compounds of `selector` name directly.
fn names_in_the_tree_of(selector: crate::css::selector::Selector<'_>) -> SubjectNames {
    selector
        .compounds()
        .flat_map(|compound| compound.simple_selectors.iter())
        .filter_map(|simple| match simple {
            SimpleSelector::Class(name) => Some((true, raw_of(name.name))),
            SimpleSelector::Attribute(attribute) if names_a_filtered_attribute(&attribute) => {
                Some((false, raw_of(attribute.qualified_name.name)))
            }
            _ => None,
        })
        .filter(|&(_, raw)| raw != 0)
        .collect()
}

/// The open-addressed table of `raws`, at most half full, whose size is a power of two.
fn raw_table(raws: Vec<(StyleAtomID, usize)>) -> Box<[(StyleAtomID, usize)]> {
    let mut table = vec![(StyleAtomID::NONE, 0); (raws.len() * 2).next_power_of_two().max(2)].into_boxed_slice();
    let mask = table.len() - 1;
    for (atom, raw) in raws {
        let mut slot = atom.0 as usize & mask;
        while !table[slot].0.is_none() {
            slot = (slot + 1) & mask;
        }
        table[slot] = (atom, raw);
    }
    table
}

/// Compile a selector list for the DOM query APIs, in a document whose HTML elements are in `html_namespace`, which
/// is zero for a document that is not an HTML document.
///
///
/// # Safety
/// `selectors` must point to `count` live selectors.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_selector_program_create(
    selectors: *const *const RustSelector,
    count: usize,
    html_namespace: usize,
) -> *mut DomSelectorProgram {
    let selectors = match count {
        0 => &[][..],
        _ => unsafe { ak::slice_from_raw_parts(selectors, count) },
    };
    // https://dom.spec.whatwg.org/#scope-match-a-selectors-string
    // NB: A selector query matches elements, never pseudo-elements, `::slotted()` and `::part()` among them.
    let selectors: Vec<crate::css::selector::SelectorRef> = selectors
        .iter()
        // SAFETY: The selectors are live.
        .map(|&selector| unsafe { (*selector).selector.clone() })
        .filter(|selector| {
            let selector = selector.view();
            !selector.compounds().any(|compound| {
                compound
                    .simple_selectors
                    .iter()
                    .any(|simple| matches!(simple, SimpleSelector::PseudoElement(_)))
            })
            // NB: A selector with a compound no element satisfies matches nothing, and leaves the others free to share
            //     what they require.
            && !selector
                .compounds()
                .any(|compound| crate::css::style::selector_analysis::never_matches(&compound))
        })
        .collect();
    let QueryNames {
        mut raws,
        attribute_names,
        lone_match_repeats_questions,
        descendant_lists,
    } = collect_query_names(&selectors);
    let subject = subject_requirements(&selectors);
    let names_in_the_tree = selectors
        .iter()
        .map(|selector| names_in_the_tree_of(selector.view()))
        .collect();
    // NB: Of several selectors, each could find its matches by another id, which the walk does better than lookups.
    let id_lookup = match selectors.as_slice() {
        [selector] => FfiDomIdLookup::of(selector.view(), raw_of),
        _ => FfiDomIdLookup::NONE,
    };
    // SAFETY: The namespace is a live interned string of the document.
    let html_namespace = (html_namespace != 0).then(|| unsafe {
        crate::css::style::AtomLease::acquire_raw(crate::css::style::process_atoms(), html_namespace)
    });
    // NB: A tag name written in another case than its own folds to compare with the names of the elements in the HTML
    //     namespace, which the subject asks the raw identity of then.
    if let Some(namespace) = &html_namespace {
        let atom = namespace.atom();
        if !raws.iter().any(|&(known, _)| known == atom) {
            raws.push((atom, raw_of(atom)));
        }
    }
    Box::into_raw(Box::new(DomSelectorProgram {
        attribute_names: attribute_names.into_boxed_slice(),
        raws: raw_table(raws),
        subject_local_name: subject.local_name,
        subject_classes: subject.classes.into_boxed_slice(),
        names_in_the_tree,
        required_attribute_names: subject.attribute_names.into_boxed_slice(),
        lone_match_repeats_questions,
        descendant_lists: descendant_lists.into_boxed_slice(),
        id_lookup,
        sources: selectors.into_boxed_slice(),
        html_namespace,
    }))
}

/// The names the subject of each selector of a `:has()` argument list requires, whose walk over the anchor's subtree a
/// filter of that subtree can spare, where each requires a name.
fn names_required_under_anchor(list: crate::css::selector::SelectorListView<'_>) -> Option<Box<[SubjectNames]>> {
    list.iter()
        .map(|relative| {
            let names: Box<[(bool, usize)]> = relative
                .first_compound()
                .simple_selectors
                .iter()
                .filter_map(|simple| match simple {
                    SimpleSelector::TagName(name) if name.name_is_ascii_lowercase => Some((false, raw_of(name.name))),
                    SimpleSelector::Id(name) | SimpleSelector::Class(name) => Some((true, raw_of(name.name))),
                    _ => None,
                })
                .filter(|&(_, raw)| raw != 0)
                .collect();
            (!names.is_empty()).then_some(names)
        })
        .collect()
}

/// Visit every simple selector of `selector` and of the selectors nested in it, noting in `repeats` whether a walk
/// up or back, or a count of the siblings `of S` matches, tests what reaches beyond the elements it passes.
fn visit_query_selector(
    selector: crate::css::selector::Selector<'_>,
    visit: &mut impl FnMut(&SimpleSelector<'_>),
    repeats: &mut bool,
) {
    use crate::css::selector::{Combinator, PseudoClassType, PseudoElementSelector, PseudoElementValue};
    let mut compound = Some(selector);
    while let Some(current) = compound {
        if current.is_relative_anchor() {
            return;
        }
        let simple_selectors = current.first_compound().simple_selectors;
        for simple in simple_selectors.iter() {
            let simple = &simple;
            visit(simple);
            match simple {
                SimpleSelector::PseudoClass(pseudo_class) => {
                    let of_counts = matches!(
                        pseudo_class.pseudo_class,
                        PseudoClassType::NthChild | PseudoClassType::NthLastChild
                    );
                    for nested in pseudo_class.argument_selector_list {
                        if of_counts && !reads_only_local_facts(nested) {
                            *repeats = true;
                        }
                        visit_query_selector(nested, visit, repeats);
                    }
                }
                SimpleSelector::PseudoElement(PseudoElementSelector {
                    value: PseudoElementValue::CompoundSelector(argument),
                    ..
                }) => visit_query_selector(*argument, visit, repeats),
                _ => {}
            }
        }
        compound = match current.left() {
            Some((combinator, left)) => {
                if matches!(combinator, Combinator::Descendant | Combinator::SubsequentSibling)
                    && !reads_only_local_facts(left)
                {
                    *repeats = true;
                }
                Some(left)
            }
            None => None,
        };
    }
}

/// Whether the first compound of `selector` tests only facts of the element it is tested on.
fn reads_only_local_facts(selector: crate::css::selector::Selector<'_>) -> bool {
    use crate::css::selector::PseudoClassType;
    fn simple_is_local(simple: &SimpleSelector<'_>) -> bool {
        match simple {
            SimpleSelector::Universal(_)
            | SimpleSelector::TagName(_)
            | SimpleSelector::Id(_)
            | SimpleSelector::Class(_)
            | SimpleSelector::Attribute(_) => true,
            SimpleSelector::PseudoClass(pseudo_class) => match pseudo_class.pseudo_class {
                PseudoClassType::Is | PseudoClassType::Where | PseudoClassType::Not => {
                    pseudo_class.argument_selector_list.iter().all(|nested| {
                        nested.left().is_none()
                            && nested
                                .first_compound()
                                .simple_selectors
                                .iter()
                                .all(|simple| simple_is_local(&simple))
                    })
                }
                PseudoClassType::Dir | PseudoClassType::Lang | PseudoClassType::Heading | PseudoClassType::State => {
                    true
                }
                kind => crate::css::style::component_matching::names_a_state(kind),
            },
            _ => false,
        }
    }
    selector
        .first_compound()
        .simple_selectors
        .iter()
        .all(|simple| simple_is_local(&simple))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_dom_selector_program_destroy(program: OwnedBox<DomSelectorProgram>) {
    drop(program);
}

/// The id a query finds its matches by, if any.
#[unsafe(no_mangle)]
pub extern "C" fn rust_dom_selector_program_id_lookup(program: &DomSelectorProgram) -> FfiDomIdLookup {
    program.id_lookup
}

/// The DOM, as the callbacks show it.
#[derive(Clone, Copy)]
struct DomTree<'q> {
    dom: &'q FfiDomSelectorCallbacks,
}

impl SelectorTree for DomTree<'_> {
    type Node = DomNode;

    #[inline]
    fn parent(self, node: DomNode) -> Option<DomNode> {
        optional_node(unsafe { (self.dom.parent)(node) })
    }

    #[inline]
    fn previous_sibling(self, node: DomNode) -> Option<DomNode> {
        optional_node(unsafe { (self.dom.previous_element_sibling)(node) })
    }

    #[inline]
    fn next_sibling(self, node: DomNode) -> Option<DomNode> {
        optional_node(unsafe { (self.dom.next_element_sibling)(node) })
    }

    #[inline]
    fn first_child(self, parent: DomNode) -> Option<DomNode> {
        optional_node(unsafe { (self.dom.first_element_child)(parent) })
    }

    #[inline]
    fn shadow_root_of(self, host: DomNode) -> Option<DomNode> {
        optional_node(unsafe { (self.dom.shadow_root)(host) })
    }

    #[inline]
    fn host_of(self, shadow_root: DomNode) -> Option<DomNode> {
        optional_node(unsafe { (self.dom.host)(shadow_root) })
    }

    #[inline]
    fn next_in_subtree(self, node: DomNode, root: DomNode) -> Option<DomNode> {
        optional_node(unsafe { (self.dom.next_element_in_subtree)(node, root, 0, 0, 0) })
    }
}

/// The live DOM as a subject of one query.
struct DomSubject<'q> {
    dom: &'q FfiDomSelectorCallbacks,
    query: &'q DomSelectorProgram,
    ids_and_classes_ignore_case: bool,
    /// The element read last, whose facts are in `element`. The attributes the program names of the element whose
    /// attributes were asked for last, `attributes_of`, are in `attributes`: an element's attributes are read the first
    /// time a test asks for them, as most elements fail a selector before it tests an attribute.
    current: Option<DomNode>,
    element: FfiDomElement,
    attributes_of: Option<DomNode>,
    attributes: SmallVec<[FfiDomAttribute<'q>; 4]>,
    /// Whether the query remembers answers, which a query matching one element does only if it can ask a question
    /// twice.
    remembers_answers: bool,
    /// The answers remembered, from the first one on.
    answers: Option<RememberedAnswers>,
    /// The raw identities of the process atoms outside the program asked about so far, which the query looks up once
    /// each.
    raws: std::cell::RefCell<SmallVec<[(StyleAtomID, usize); 8]>>,
    /// The filter of the names in the subtree of each `:has()` anchor checked, made when the anchor is checked a second
    /// time, with another list.
    subtree_name_filters: HashMap<DomNode, Option<SubtreeNameFilter>>,
}

/// What a query has answered so far.
#[derive(Default)]
struct RememberedAnswers {
    /// The answers of `:has()` argument lists, by list and anchor, and the misses a miss of an ancestor answers.
    relative_lists: HashMap<(crate::css::style::selector_evaluation::SelectorListKey, DomNode), bool>,
    /// Whether an element or an ancestor of it matches a selector from one of its compounds leftwards, as a walk up
    /// across a descendant combinator found, for every element it passed.
    ancestors: HashMap<(crate::css::style::selector_evaluation::SelectorKey, DomNode), bool>,
    /// The index of an element among its inclusive siblings an `of S` list matches, by list, by whether it counts
    /// from the end, and by element.
    of_indices: HashMap<(crate::css::style::selector_evaluation::SelectorListKey, bool, DomNode), i64>,
}

impl DomSubject<'_> {
    /// Where the query keeps answers, which it makes when it records its first.
    #[inline]
    fn remembered_answers(&mut self) -> Option<&mut RememberedAnswers> {
        match self.remembers_answers {
            true => Some(self.answers.get_or_insert_with(RememberedAnswers::default)),
            false => None,
        }
    }

    /// The raw identity of the name the process atom `atom` names, or zero for `NONE`.
    #[inline]
    fn raw(&self, atom: StyleAtomID) -> usize {
        if atom.is_none() {
            return 0;
        }
        let table = &self.query.raws;
        let mask = table.len() - 1;
        let mut slot = atom.0 as usize & mask;
        loop {
            let (known, raw) = table[slot];
            if known == atom {
                return raw;
            }
            if known.is_none() {
                break;
            }
            slot = (slot + 1) & mask;
        }
        if let Some(&(_, raw)) = self.raws.borrow().iter().find(|(known, _)| *known == atom) {
            return raw;
        }
        // NB: A query names attributes in any namespace by their local names, qualified by no namespace.
        let raw = crate::css::style::process_atoms().local_raw_of(atom).unwrap_or(0);
        self.raws.borrow_mut().push((atom, raw));
        raw
    }

    /// The filter of the local names, ids and classes of the elements under `anchor`, read through a copy of `facts`.
    #[inline(never)]
    fn subtree_name_filter(dom: &FfiDomSelectorCallbacks, facts: FfiDomElement, anchor: DomNode) -> SubtreeNameFilter {
        let mut filter = [0u64; SUBTREE_NAME_FILTER_WORDS];
        let mut add = |raw: usize| {
            if raw != 0 {
                for bit in subtree_name_filter_bits(raw) {
                    filter[bit / 64] |= 1 << (bit % 64);
                }
            }
        };
        let mut facts = facts;
        let mut element = optional_node(unsafe { (dom.next_element_in_subtree)(anchor, anchor, 0, 0, 0) });
        while let Some(current) = element {
            // SAFETY: The host writes the element's facts, and no attribute where it is asked for none.
            unsafe {
                (dom.element)(current, std::ptr::null(), 0, std::ptr::null_mut(), 0, &mut facts);
            }
            add(facts.local_name);
            add(facts.id);
            // SAFETY: The classes stay as they are until the DOM next changes, which it does not during a query.
            for &class in unsafe { ak::slice_from_raw_parts(facts.classes, facts.class_count) } {
                add(class);
            }
            element = optional_node(unsafe { (dom.next_element_in_subtree)(current, anchor, 0, 0, 0) });
        }
        std::rc::Rc::new(filter)
    }

    /// Reads the attributes the program names of `node` into `attributes`.
    #[inline(never)]
    fn fill_attributes(&mut self, node: DomNode) {
        let dom = self.dom;
        let names = &self.query.attribute_names;
        let mut facts = self.element;
        self.attributes.clear();
        let capacity = self.attributes.capacity();
        // SAFETY: The host writes at most the capacity it is told of, and says how many there are, all of which it
        //         wrote once they fit.
        unsafe {
            (dom.element)(
                node,
                names.as_ptr(),
                names.len(),
                self.attributes.as_mut_ptr(),
                capacity,
                &mut facts,
            );
            let count = facts.attribute_count;
            if count > capacity {
                self.attributes.reserve_exact(count);
                (dom.element)(
                    node,
                    names.as_ptr(),
                    names.len(),
                    self.attributes.as_mut_ptr(),
                    count,
                    &mut facts,
                );
            }
            self.attributes.set_len(count);
        }
        self.attributes_of = Some(node);
    }
}

/// An element, with the subject whose names its atoms are and which holds its facts.
struct DomFeatures<'s, 'q> {
    subject: &'s DomSubject<'q>,
    node: DomNode,
}

impl DomFeatures<'_, '_> {
    /// The subject holds the facts of the element read last, which is this one: an element's features are asked of as
    /// soon as it is read.
    #[inline]
    fn element(&self) -> &FfiDomElement {
        debug_assert!(self.subject.current == Some(self.node));
        &self.subject.element
    }
}

impl<'q> ElementFeatures for DomFeatures<'_, 'q> {
    type Attribute = FfiDomAttribute<'q>;

    #[inline]
    fn local_name_is(&self, name: StyleAtomID) -> bool {
        self.element().local_name == self.subject.raw(name)
    }

    #[inline]
    fn namespace_is(&self, namespace: StyleAtomID) -> bool {
        self.element().namespace_uri == self.subject.raw(namespace)
    }

    #[inline]
    fn has_id(&self, id: StyleAtomID) -> bool {
        let id = self.subject.raw(id);
        if self.element().id == 0 || id == 0 {
            return false;
        }
        match self.subject.ids_and_classes_ignore_case {
            true => unsafe { (self.subject.dom.id_or_class_equals_ignoring_ascii_case)(self.node, false, id) },
            false => self.element().id == id,
        }
    }

    #[inline]
    fn has_class(&self, class: StyleAtomID) -> bool {
        let class = self.subject.raw(class);
        let element = self.element();
        if element.class_count == 0 || class == 0 {
            return false;
        }
        match self.subject.ids_and_classes_ignore_case {
            true => unsafe { (self.subject.dom.id_or_class_equals_ignoring_ascii_case)(self.node, true, class) },
            false => unsafe { ak::slice_from_raw_parts(element.classes, element.class_count) }.contains(&class),
        }
    }

    #[inline]
    fn attributes_named(
        &self,
        name: StyleAtomID,
        any_namespace: bool,
    ) -> impl Iterator<Item = FfiDomAttribute<'q>> + '_ {
        let name = self.subject.raw(name);
        debug_assert!(self.subject.current == Some(self.node));
        debug_assert!(self.subject.query.attribute_names.is_empty() || self.subject.attributes_of == Some(self.node));
        self.subject
            .attributes
            .iter()
            .copied()
            .filter(move |attribute| attribute.local_name == name && (any_namespace || attribute.namespace_uri == 0))
    }
}

impl<'q> SelectorSubject for DomSubject<'q> {
    type Node = DomNode;
    type Tree = DomTree<'q>;
    /// An element's facts are read into the subject, where they stay until another element is read.
    type Row = DomNode;
    type Attribute = FfiDomAttribute<'q>;
    type Features<'s>
        = DomFeatures<'s, 'q>
    where
        Self: 's;
    type Incomplete = Infallible;

    #[inline]
    fn tree(&self) -> DomTree<'q> {
        DomTree { dom: self.dom }
    }

    #[inline]
    fn row(&mut self, node: DomNode) -> Result<DomNode, Infallible> {
        if self.current != Some(node) {
            // SAFETY: The host writes the element's facts, and no attribute where it is asked for none.
            unsafe {
                (self.dom.element)(node, std::ptr::null(), 0, std::ptr::null_mut(), 0, &mut self.element);
            }
            self.current = Some(node);
        }
        Ok(node)
    }

    #[inline]
    fn read_attributes(&mut self, node: DomNode) {
        if self.attributes_of != Some(node) && !self.query.attribute_names.is_empty() {
            self.fill_attributes(node);
        }
    }

    #[inline]
    fn features(&self, node: DomNode) -> DomFeatures<'_, 'q> {
        DomFeatures { subject: self, node }
    }

    #[inline]
    fn same_type(&self, node: DomNode, other: DomNode) -> bool {
        unsafe { (self.dom.same_type)(node, other) }
    }

    #[inline]
    fn attribute_value_atom(&self, _attribute: FfiDomAttribute<'q>) -> StyleAtomID {
        StyleAtomID::NONE
    }

    #[inline]
    fn attribute_value_text(&self, attribute: FfiDomAttribute<'q>) -> Option<TokenizerInput<'_>> {
        // An empty value crosses as an empty view.
        Some(attribute.value.units().unwrap_or(TokenizerInput::Utf16(&[])))
    }

    #[inline]
    fn has_state(&self, node: DomNode, fact: StateFact) -> bool {
        unsafe { (self.dom.matches_state)(node, fact as u8) }
    }

    #[inline]
    fn language_tag(&self, node: DomNode) -> TokenizerInput<'_> {
        unsafe { (self.dom.language)(node).units() }.unwrap_or_default()
    }

    #[inline]
    fn directionality_is(&self, node: DomNode, direction: StyleAtomID) -> bool {
        let actual = unsafe { (self.dom.directionality)(node) };
        actual == self.raw(direction)
    }

    #[inline]
    fn has_custom_state(&self, node: DomNode, state: StyleAtomID) -> bool {
        let state = self.raw(state);
        state != 0 && unsafe { (self.dom.has_custom_state)(node, state) }
    }

    #[inline]
    fn heading_level(&self, node: DomNode) -> u8 {
        u8::try_from(unsafe { (self.dom.heading_level)(node) }).unwrap_or(0)
    }

    #[inline]
    fn is_empty(&mut self, node: DomNode) -> Result<bool, Infallible> {
        Ok(unsafe { (self.dom.is_empty)(node) })
    }

    #[inline]
    fn is_root(&self, node: DomNode) -> bool {
        unsafe { (self.dom.is_document_element)(node) }
    }

    #[inline]
    fn sibling_index(&mut self, position: NthPosition, node: DomNode) -> Result<Option<i64>, Infallible> {
        // A fixed position near the end it counts from, such as `:last-child` or `:nth-child(3)`, is found by a scan of
        // at most that many siblings. An index could cost a walk over the whole child list after it changes. A scan to a
        // far position costs as much as that walk, though, and one per candidate makes asking for every position of a
        // list quadratic, while the element keeps its index once it is counted.
        const NEAR_POSITION_SCAN_LIMIT: i32 = 8;
        if position.step == 0 && position.offset <= NEAR_POSITION_SCAN_LIMIT {
            return Ok(None);
        }
        let which = match (position.of_type, position.from_end) {
            (false, false) => FfiChildIndex::FromStart,
            (false, true) => FfiChildIndex::FromEnd,
            (true, false) => FfiChildIndex::OfTypeFromStart,
            (true, true) => FfiChildIndex::OfTypeFromEnd,
        };
        Ok(Some(i64::from(unsafe { (self.dom.child_index)(node, which) })))
    }

    const REMEMBERS_ANCESTOR_ANSWERS: bool = true;

    const REMEMBERS_SIBLING_ANSWERS: bool = true;

    // NB: A query's walks share their bindings, and a compound left of `~` is behind no other
    //     combinator, so its answers share the ancestors' table.
    type WalkAnswers = crate::css::style::selector_evaluation::SelectorKey;

    #[inline]
    fn walk_answers(
        &mut self,
        selector: crate::css::style::selector_evaluation::SelectorKey,
        _bindings: &crate::css::style::selector_evaluation::SelectorBindings<DomNode>,
    ) -> Option<Self::WalkAnswers> {
        Some(selector)
    }

    #[inline]
    fn walk_answer(&self, selector: Self::WalkAnswers, node: DomNode) -> Option<bool> {
        self.answers.as_ref()?.ancestors.get(&(selector, node)).copied()
    }

    #[inline]
    fn record_walk_answers(&mut self, selector: Self::WalkAnswers, nodes: &[DomNode], answer: bool) {
        if let Some(answers) = self.remembered_answers() {
            // NB: A query that walks up once walks up for most of the elements it matches, across about as many
            //     ancestors each time, which the table makes room for at once.
            if answers.ancestors.is_empty() {
                answers.ancestors.reserve(256);
            }
            for &node in nodes {
                answers.ancestors.insert((selector, node), answer);
            }
        }
    }

    #[inline]
    fn remembers_of_indices(&self) -> bool {
        self.remembers_answers
    }

    #[inline]
    fn of_index_of(
        &self,
        list: crate::css::style::selector_evaluation::SelectorListKey,
        from_end: bool,
        node: DomNode,
    ) -> Option<i64> {
        self.answers.as_ref()?.of_indices.get(&(list, from_end, node)).copied()
    }

    #[inline]
    fn record_of_index(
        &mut self,
        list: crate::css::style::selector_evaluation::SelectorListKey,
        from_end: bool,
        node: DomNode,
        index: i64,
    ) {
        if let Some(answers) = self.remembered_answers() {
            answers.of_indices.insert((list, from_end, node), index);
        }
    }

    #[inline]
    fn relative_answer_of(
        &mut self,
        list: crate::css::style::selector_evaluation::SelectorListKey,
        anchor: DomNode,
        bindings: &SelectorBindings<DomNode>,
    ) -> Option<bool> {
        let tree = self.tree();
        let query = self.query;
        let answers = self.answers.as_mut()?;
        if let Some(&answer) = answers.relative_lists.get(&(list, anchor)) {
            return Some(answer);
        }
        // A list starting with a descendant combinator that misses under an ancestor of the anchor misses under the
        // anchor too. An ancestor it matches under says nothing of the anchor, and the ones above that cannot be misses.
        let descendant_list = query.descendant_lists.iter().find(|&&(known, _)| known == list);
        if descendant_list.is_some() {
            let mut passed = SmallVec::<[DomNode; 16]>::new();
            passed.push(anchor);
            let mut ancestor = tree.parent(anchor);
            while let Some(current) = ancestor {
                match answers.relative_lists.get(&(list, current)) {
                    Some(false) => {
                        // NB: Each element passed misses as well, which spares later walks up from passing it again.
                        for node in passed {
                            answers.relative_lists.insert((list, node), false);
                        }
                        return Some(false);
                    }
                    Some(true) => break,
                    None => passed.push(current),
                }
                ancestor = tree.parent(current);
            }
        }
        // A list whose every selector requires a name under the anchor matches under no anchor whose subtree lacks
        // one of each. The names are gathered once the anchor is checked again, as many anchors are checked once.
        // NB: A `:host:has()` looks under the host's shadow root, not under the host.
        if let Some((_, Some(required))) = descendant_list
            && bindings.scope_shadow_root.and_then(|root| tree.host_of(root)) != Some(anchor)
        {
            let filters = &mut self.subtree_name_filters;
            // NB: Many anchors are checked only once, and a miss of an ancestor answered those that a filter could.
            let Some(own) = filters.get(&anchor).cloned() else {
                filters.insert(anchor, None);
                return None;
            };
            // NB: The filter of an ancestor holds every name under the anchor, so anchors nested in one share its filter
            //     and only the outermost walks its subtree.
            let filter = own.unwrap_or_else(|| {
                let mut ancestor = tree.parent(anchor);
                while let Some(current) = ancestor {
                    if let Some(Some(filter)) = filters.get(&current) {
                        return filter.clone();
                    }
                    ancestor = tree.parent(current);
                }
                Self::subtree_name_filter(self.dom, self.element, anchor)
            });
            let lacks = |raw: usize| {
                subtree_name_filter_bits(raw)
                    .iter()
                    .any(|&bit| filter[bit / 64] & (1 << (bit % 64)) == 0)
            };
            let ignore_case = self.ids_and_classes_ignore_case;
            let rejected = required
                .iter()
                .all(|names| names.iter().any(|&(folds, raw)| !(folds && ignore_case) && lacks(raw)));
            filters.insert(anchor, Some(filter));
            if rejected {
                answers.relative_lists.insert((list, anchor), false);
                return Some(false);
            }
        }
        None
    }

    #[inline]
    fn record_relative_answer_of(
        &mut self,
        list: crate::css::style::selector_evaluation::SelectorListKey,
        anchor: DomNode,
        _bindings: &SelectorBindings<DomNode>,
        answer: bool,
    ) {
        if let Some(answers) = self.remembered_answers() {
            answers.relative_lists.insert((list, anchor), answer);
        }
    }
}

/// One query as it runs: its program, evaluated against the DOM.
struct DomQuery<'q> {
    query: &'q DomSelectorProgram,
    evaluator: SelectorEvaluator<DomSubject<'q>>,
    /// What the query's selectors match in, the same for every element.
    context: crate::css::style::component_matching::MatchContext<'q>,
}

impl<'q> DomQuery<'q> {
    /// A query that matches one element alone remembers answers only if it can ask one twice.
    ///
    /// # Safety
    /// `query` must describe a live program and callbacks.
    unsafe fn new(query: &'q FfiDomSelectorQuery, matches_one_element: bool) -> Self {
        let program = unsafe { &*query.program };
        let shadow_root = optional_node(query.shadow_root);
        // NB: A query declares no namespaces.
        let context = crate::css::style::component_matching::MatchContext {
            html_namespace: program
                .html_namespace
                .as_ref()
                .map_or(StyleAtomID::NONE, crate::css::style::AtomLease::atom),
            fold_ids_and_classes: false,
            namespaces: crate::css::selector_arena::SheetNamespaces::default(),
            escapes_its_namespace: false,
            logical_subject: false,
        };
        Self {
            query: program,
            context,
            evaluator: SelectorEvaluator {
                subject: DomSubject {
                    dom: unsafe { &*query.callbacks },
                    query: program,
                    ids_and_classes_ignore_case: query.ids_and_classes_ignore_case,
                    current: None,
                    element: FfiDomElement {
                        local_name: 0,
                        namespace_uri: 0,
                        id: 0,
                        classes: std::ptr::null(),
                        class_count: 0,
                        attribute_count: 0,
                    },
                    attributes_of: None,
                    attributes: SmallVec::new(),
                    remembers_answers: !matches_one_element || program.lone_match_repeats_questions,
                    answers: None,
                    raws: Default::default(),
                    subtree_name_filters: Default::default(),
                },
                bindings: SelectorBindings {
                    scope_shadow_root: shadow_root,
                    scope_root_instance: optional_node(query.scope),
                    ..SelectorBindings::default()
                },
            },
        }
    }

    fn matches(&mut self, element: DomNode) -> bool {
        let context = self.context;
        // SAFETY: The selectors outlive the program.
        self.query.sources.iter().any(|selector| {
            let Ok(matches) = self.evaluator.matches_selector(selector.view(), element, context);
            matches
        })
    }
}

/// Whether an element matches any selector of a query.
///
/// # Safety
/// `query` must describe a live program and callbacks, and `element` must be a live element.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_selector_query_matches(query: &FfiDomSelectorQuery, element: *const c_void) -> bool {
    unsafe { DomQuery::new(query, true) }.matches(element)
}

/// The nearest inclusive ancestor of an element that matches any selector of a query, or null.
///
/// # Safety
/// As for `rust_dom_selector_query_matches`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_selector_query_closest(
    query: &FfiDomSelectorQuery,
    element: *const c_void,
) -> *const c_void {
    let mut query_run = unsafe { DomQuery::new(query, false) };
    let tree = query_run.evaluator.subject.tree();
    let shadow_root = query_run.evaluator.bindings.scope_shadow_root;
    let mut candidate = Some(element);
    // The walk up ends at the root of the element's tree, which is not an element when it is a shadow root.
    while let Some(current) = candidate
        && Some(current) != shadow_root
    {
        if query_run.matches(current) {
            return current;
        }
        candidate = tree.parent(current);
    }
    std::ptr::null()
}

/// Calls `found` with each element under `root` that matches any selector of a query, in tree order, until it returns
/// true.
///
/// # Safety
/// As for `rust_dom_selector_query_matches`, and `root` must be a live node.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_selector_query_subtree(
    query: &FfiDomSelectorQuery,
    root: *const c_void,
    context: *mut c_void,
    found: unsafe extern "C" fn(context: *mut c_void, element: *const c_void) -> bool,
) {
    let mut query_run = unsafe { DomQuery::new(query, false) };
    // A query whose every selector matches nothing needs no walk.
    if query_run.query.sources.is_empty() {
        return;
    }
    let dom = query_run.evaluator.subject.dom;
    // A tree lacking a name of every selector holds no match. A class compared ignoring case can be written in another
    // case than the element's.
    let ignore_case = query_run.evaluator.subject.ids_and_classes_ignore_case;
    let tree_names = unsafe { (dom.tree_name_filter)(root) };
    if query_run.query.names_in_the_tree.iter().all(|names| {
        names.iter().any(|&(is_class, name)| {
            !(is_class && ignore_case) && tree_names & unsafe { (dom.name_filter_bit)(name) } == 0
        })
    }) {
        return;
    }
    // Only an element with the local name and the class every match has can match, and a subtree whose name filter
    // lacks an attribute name or a class each match carries holds no match.
    let local_name = query_run.query.subject_local_name;
    // NB: A class compared ignoring case can be written in another case than the element's.
    let class_name = match query_run.evaluator.subject.ids_and_classes_ignore_case {
        true => 0,
        false => query_run.query.subject_classes.first().copied().unwrap_or(0),
    };
    let names = query_run.query.required_name_bits(dom, ignore_case);
    let next =
        |node| optional_node(unsafe { (dom.next_element_in_subtree)(node, root, local_name, class_name, names) });
    let mut candidate = next(root);
    while let Some(element) = candidate {
        if query_run.matches(element) && unsafe { found(context, element) } {
            return;
        }
        candidate = next(element);
    }
}
