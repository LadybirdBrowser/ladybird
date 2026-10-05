/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Selector matching for the DOM query APIs (`querySelector()`, `querySelectorAll()`, `matches()` and `closest()`),
//! against the DOM itself.
//!
//! A query's selectors compile into a selector program whose atoms are the query's own names, and the one selector
//! evaluator runs it with the live DOM as its subject. A query reads the DOM where it stands: nothing is mirrored for
//! it and the style engine is not consulted. Every fact a selector tests is asked of the DOM through the callbacks the
//! host passes in.

use std::convert::Infallible;
use std::ffi::c_void;
use std::mem::MaybeUninit;

use smallvec::SmallVec;

use super::css_tokenizer::TokenizerInput;
use super::ffi_support::FfiUtf16View;
use super::selector::RustSelector;
use super::style::compiler::SelectorCompiler;
use super::style::fast_hash::FastMap as HashMap;
use super::style::index::DispatchKey;
use super::style::index::StyleAtomID;
use super::style::relative_selector::RelativeQueryID;
use super::style::selector::FeatureTest;
use super::style::selector::NthPosition;
use super::style::selector::QueryAtoms;
use super::style::selector::SelectorNodeID;
use super::style::selector::SelectorOp;
use super::style::selector::SelectorProgram;
use super::style::selector_evaluation::ElementFeatures;
use super::style::selector_evaluation::PrecedingSiblingPrefix;
use super::style::selector_evaluation::RememberedPrefix;
use super::style::selector_evaluation::SelectorBindings;
use super::style::selector_evaluation::SelectorEvaluator;
use super::style::selector_evaluation::SelectorSubject;
use super::style::selector_evaluation::SelectorTree;
use super::style::transaction::StateFact;

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
pub struct FfiDomAttribute {
    pub local_name: usize,
    /// Zero for the null namespace.
    pub namespace_uri: usize,
    pub value: FfiUtf16View,
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
    /// has that local name, skipping the subtrees whose attribute name filter lacks one of the bits of
    /// `attribute_names`. `node` is `root` itself to start. Both may be any node.
    pub next_element_in_subtree: unsafe extern "C" fn(
        node: *const c_void,
        root: *const c_void,
        local_name: usize,
        attribute_names: u64,
    ) -> *const c_void,
    /// The bit of the DOM's attribute name filter for a local name.
    pub attribute_name_filter_bit: unsafe extern "C" fn(local_name: usize) -> u64,
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
    /// The element's resolved language tag, or an empty view when it has none. Only valid until the next callback.
    pub language: unsafe extern "C" fn(element: *const c_void) -> FfiUtf16View,
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

type DomNode = *const c_void;

fn optional_node(node: *const c_void) -> Option<DomNode> {
    (!node.is_null()).then_some(node)
}

/// The names a query's program holds as atoms, which are the raw identities of the interned strings its selectors
/// hold. Atom `n` is entry `n - 1`, so no name is `StyleAtomID::NONE`.
#[derive(Default)]
struct QueryNames(Vec<(usize, Option<StyleAtomID>)>);

impl QueryNames {
    fn intern(&mut self, raw: usize, namespace: Option<StyleAtomID>) -> StyleAtomID {
        let index = match self.0.iter().position(|&name| name == (raw, namespace)) {
            Some(index) => index,
            None => {
                self.0.push((raw, namespace));
                self.0.len() - 1
            }
        };
        StyleAtomID(u32::try_from(index + 1).unwrap_or(u32::MAX))
    }

    /// The raw identity of an atom's name, or zero for `NONE`.
    #[inline]
    fn raw(&self, atom: StyleAtomID) -> usize {
        atom.0
            .checked_sub(1)
            .and_then(|index| self.0.get(index as usize))
            .map_or(0, |&(raw, _)| raw)
    }
}

/// A selector query compiled for one kind of document, as the host caches it with the query.
pub struct DomSelectorProgram {
    program: SelectorProgram<QueryAtoms>,
    names: QueryNames,
    /// Every attribute name the program's selectors compare, in either case: an element's row is read with them.
    attribute_names: Box<[usize]>,
    /// The roots of the entries that can name an element. A query names elements, and a pseudo-element is not one.
    subjects: Box<[SelectorNodeID]>,
    /// The local name every subject requires its element to have, or zero.
    subject_local_name: usize,
    /// The local names of the attributes every subject requires its element to carry.
    required_attribute_names: Box<[usize]>,
    /// Whether matching one element can ask the same question twice, which is only worth remembering answers for.
    lone_match_repeats_questions: bool,
}

impl DomSelectorProgram {
    /// The attribute names every match carries, as bits of the DOM's attribute name filter.
    fn required_attribute_name_bits(&self, dom: &FfiDomSelectorCallbacks) -> u64 {
        self.required_attribute_names
            .iter()
            .fold(0, |bits, &name| bits | unsafe { (dom.attribute_name_filter_bit)(name) })
    }
}

/// Compile a selector list for the DOM query APIs, in a document whose HTML elements are in `html_namespace`, which
/// is zero for a document that is not an HTML document.
///
/// # Safety
/// `selectors` must point to `count` live selectors, which must outlive the program.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_selector_program_create(
    selectors: *const *const RustSelector,
    count: usize,
    html_namespace: usize,
) -> *mut DomSelectorProgram {
    let selectors = match count {
        0 => &[][..],
        _ => unsafe { std::slice::from_raw_parts(selectors, count) },
    };
    let mut names = QueryNames::default();
    let program = {
        let mut intern = |raw, namespace| names.intern(raw, namespace);
        let html_element_namespace = match html_namespace {
            0 => StyleAtomID::NONE,
            namespace => intern(namespace, None),
        };
        let mut compiler = SelectorCompiler::for_query(&mut intern, html_element_namespace);
        for &selector in selectors {
            compiler.compile_for_query(unsafe { (*selector).compiled() });
        }
        compiler.finish()
    };
    let mut attribute_names = Vec::new();
    // A walk up or back tests each element it passes once, and so does a count of the siblings `of S` matches, unless
    // what it tests reaches beyond the element it tests.
    let mut lone_match_repeats_questions = false;
    // A name that folds is carried in one case or the other depending on the element, so the folded form it
    // dispatches on is not the name every match has.
    let mut folding_names = SmallVec::<[StyleAtomID; 4]>::new();
    for index in 0..program.node_count() {
        let Ok(index) = u32::try_from(index) else {
            break;
        };
        match program.node(SelectorNodeID(index)) {
            SelectorOp::Feature(FeatureTest::Attribute(test)) => {
                for name in [names.raw(test.name), names.raw(test.folded)] {
                    if !attribute_names.contains(&name) {
                        attribute_names.push(name);
                    }
                }
                if test.name != test.folded {
                    folding_names.push(test.folded);
                }
            }
            SelectorOp::Feature(FeatureTest::TagName(tag)) if tag.written != tag.folded => {
                folding_names.push(tag.folded);
            }
            SelectorOp::Ancestor(tested)
            | SelectorOp::PrecedingSibling(tested)
            | SelectorOp::NthPosition(NthPosition {
                of_selector: Some(tested),
                ..
            }) => lone_match_repeats_questions |= !program.selector_node_reads_only_local_facts(tested),
            _ => {}
        }
    }
    let mut subjects = Vec::new();
    let mut subject_local_name = None;
    let mut required_attribute_names: Option<Vec<usize>> = None;
    for (index, entry) in program.entries().iter().enumerate() {
        if entry.pseudo_element.is_some() || program.entry_never_matches(entry) {
            continue;
        }
        subjects.push(entry.root);
        // The keys an entry dispatches on are alternatives, so a lone one is required as well.
        let dispatch = program.subject_dispatch_keys(index);
        let mut required_local_name = None;
        let mut required_by_entry = SmallVec::<[usize; 4]>::new();
        for &key in program
            .subject_required_keys(index)
            .iter()
            .chain(dispatch.iter().filter(|_| dispatch.len() == 1))
        {
            match key {
                DispatchKey::TagName(name) if !folding_names.contains(&name) => required_local_name = Some(name),
                DispatchKey::AttributeName(name) if !folding_names.contains(&name) => {
                    required_by_entry.push(names.raw(name));
                }
                _ => {}
            }
        }
        subject_local_name = match subject_local_name {
            None => Some(required_local_name),
            Some(agreed) => Some(agreed.filter(|&agreed| Some(agreed) == required_local_name)),
        };
        match &mut required_attribute_names {
            None => required_attribute_names = Some(required_by_entry.into_vec()),
            Some(required) => required.retain(|name| required_by_entry.contains(name)),
        }
    }
    let subject_local_name = subject_local_name.flatten().map_or(0, |name| names.raw(name));
    Box::into_raw(Box::new(DomSelectorProgram {
        program,
        names,
        attribute_names: attribute_names.into_boxed_slice(),
        subjects: subjects.into_boxed_slice(),
        subject_local_name,
        required_attribute_names: required_attribute_names.unwrap_or_default().into_boxed_slice(),
        lone_match_repeats_questions,
    }))
}

/// # Safety
/// `program` must be null or a program `rust_dom_selector_program_create` returned, which is not used again.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_selector_program_destroy(program: *mut DomSelectorProgram) {
    if !program.is_null() {
        drop(unsafe { Box::from_raw(program) });
    }
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
    fn first_sibling(self, node: DomNode) -> DomNode {
        unsafe { (self.dom.first_element_sibling)(node) }
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
        optional_node(unsafe { (self.dom.next_element_in_subtree)(node, root, 0, 0) })
    }
}

/// The live DOM as a subject of one query.
struct DomSubject<'q> {
    dom: &'q FfiDomSelectorCallbacks,
    query: &'q DomSelectorProgram,
    ids_and_classes_ignore_case: bool,
    /// The element read last, whose facts are in `element` and whose attributes the program names are in
    /// `attributes`, or in `spilled_attributes` when there are more than fit. The host writes them in place, and they
    /// are read field by field where they lie. Nothing is written to the buffers until an element has such attributes.
    current: Option<DomNode>,
    element: FfiDomElement,
    attributes: [MaybeUninit<FfiDomAttribute>; 4],
    spilled_attributes: Vec<FfiDomAttribute>,
    /// Whether the query remembers answers, which a query matching one element does only if it can ask a question
    /// twice.
    remembers_answers: bool,
    /// The answers remembered, from the first one on.
    answers: Option<RememberedAnswers>,
}

/// What a query has answered so far.
#[derive(Default)]
struct RememberedAnswers {
    relations: HashMap<(SelectorNodeID, DomNode), bool>,
    preceding_sibling_prefixes: HashMap<(SelectorNodeID, DomNode), PrecedingSiblingPrefix<DomNode>>,
    relative_queries: HashMap<(RelativeQueryID, DomNode), bool>,
    /// How many siblings an `of S` selector matches from the start (or the end) of an element's siblings through the
    /// element, by element, S and the end counted from.
    sibling_counts_of: HashMap<(DomNode, SelectorNodeID, bool), u32>,
}

impl<'q> DomSubject<'q> {
    /// Where the query keeps answers, which it makes when it records its first.
    #[inline]
    fn remembered_answers(&mut self) -> Option<&mut RememberedAnswers> {
        match self.remembers_answers {
            true => Some(self.answers.get_or_insert_with(RememberedAnswers::default)),
            false => None,
        }
    }

    #[inline]
    fn names(&self) -> &'q QueryNames {
        &self.query.names
    }

    /// Reads the element into `element`, and the attributes the program names into `attributes`.
    #[inline(never)]
    fn read_element_with_attributes(&mut self, node: DomNode) {
        let dom = self.dom;
        let names = &self.query.attribute_names;
        let read = |attributes: *mut FfiDomAttribute, capacity: usize, element: &mut FfiDomElement| unsafe {
            (dom.element)(node, names.as_ptr(), names.len(), attributes, capacity, element);
        };
        read(
            self.attributes.as_mut_ptr().cast(),
            self.attributes.len(),
            &mut self.element,
        );
        let count = self.element.attribute_count;
        if count > self.attributes.len() {
            self.spilled_attributes.clear();
            self.spilled_attributes.reserve_exact(count);
            read(self.spilled_attributes.as_mut_ptr(), count, &mut self.element);
            // SAFETY: The host wrote every attribute, as they fit.
            unsafe { self.spilled_attributes.set_len(count) };
        }
    }

    /// The attributes the program names of the element read last.
    #[inline]
    fn attributes(&self) -> &[FfiDomAttribute] {
        let count = self.element.attribute_count;
        match count <= self.attributes.len() {
            // SAFETY: The host wrote this many attributes to the buffer when it read the element.
            true => unsafe { std::slice::from_raw_parts(self.attributes.as_ptr().cast(), count) },
            false => &self.spilled_attributes,
        }
    }
}

/// An element, with the subject whose names its atoms are and which holds its facts.
struct DomFeatures<'s> {
    subject: &'s DomSubject<'s>,
    node: DomNode,
}

impl DomFeatures<'_> {
    /// The subject holds the facts of the element read last, which is this one: an element's features are asked of as
    /// soon as it is read.
    #[inline]
    fn element(&self) -> &FfiDomElement {
        debug_assert!(self.subject.current == Some(self.node));
        &self.subject.element
    }
}

impl ElementFeatures for DomFeatures<'_> {
    type Attribute = FfiDomAttribute;

    #[inline]
    fn local_name_is(&self, name: StyleAtomID) -> bool {
        self.element().local_name == self.subject.names().raw(name)
    }

    #[inline]
    fn namespace_is(&self, namespace: StyleAtomID) -> bool {
        self.element().namespace_uri == self.subject.names().raw(namespace)
    }

    #[inline]
    fn has_id(&self, id: StyleAtomID) -> bool {
        let id = self.subject.names().raw(id);
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
        let class = self.subject.names().raw(class);
        let element = self.element();
        if element.class_count == 0 || class == 0 {
            return false;
        }
        match self.subject.ids_and_classes_ignore_case {
            true => unsafe { (self.subject.dom.id_or_class_equals_ignoring_ascii_case)(self.node, true, class) },
            false => unsafe { std::slice::from_raw_parts(element.classes, element.class_count) }.contains(&class),
        }
    }

    #[inline]
    fn attributes_named(&self, name: StyleAtomID, any_namespace: bool) -> impl Iterator<Item = FfiDomAttribute> + '_ {
        let name = self.subject.names().raw(name);
        debug_assert!(self.subject.current == Some(self.node));
        self.subject
            .attributes()
            .iter()
            .copied()
            .filter(move |attribute| attribute.local_name == name && (any_namespace || attribute.namespace_uri == 0))
    }
}

impl<'q> SelectorSubject for DomSubject<'q> {
    type Atoms = QueryAtoms;
    type Node = DomNode;
    type Tree = DomTree<'q>;
    /// An element's facts are read into the subject, where they stay until another element is read.
    type Row = DomNode;
    type Attribute = FfiDomAttribute;
    type Features<'s>
        = DomFeatures<'s>
    where
        Self: 's;
    type Incomplete = Infallible;
    type Counters = ();
    type PrefixSlot = (SelectorNodeID, DomNode);
    const REMEMBERS_SIBLING_COUNTS_OF_SELECTORS: bool = true;

    #[inline]
    fn tree(&self) -> DomTree<'q> {
        DomTree { dom: self.dom }
    }

    #[inline]
    fn row(&mut self, node: DomNode) -> Result<DomNode, Infallible> {
        if self.current != Some(node) {
            match self.query.attribute_names.is_empty() {
                true => unsafe {
                    (self.dom.element)(node, std::ptr::null(), 0, std::ptr::null_mut(), 0, &mut self.element);
                },
                false => self.read_element_with_attributes(node),
            }
            self.current = Some(node);
        }
        Ok(node)
    }

    #[inline]
    fn features(&self, node: DomNode) -> DomFeatures<'_> {
        DomFeatures { subject: self, node }
    }

    #[inline]
    fn same_type(&self, node: DomNode, other: DomNode) -> bool {
        unsafe { (self.dom.same_type)(node, other) }
    }

    #[inline]
    fn attribute_value_atom(&self, _attribute: FfiDomAttribute) -> StyleAtomID {
        StyleAtomID::NONE
    }

    #[inline]
    fn attribute_value_text(&self, attribute: FfiDomAttribute) -> Option<TokenizerInput<'_>> {
        // An empty value crosses as an empty view.
        Some(unsafe { attribute.value.units() }.unwrap_or(TokenizerInput::Utf16(&[])))
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
        actual == self.names().raw(direction)
    }

    #[inline]
    fn has_custom_state(&self, node: DomNode, state: StyleAtomID) -> bool {
        let state = self.names().raw(state);
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

    /// The scoping root and the shadow root stay bound for the whole query, so only a relative anchor or a `:host()`
    /// argument makes an answer local to one evaluation.
    #[inline]
    fn remembers_relations(&self, bindings: &SelectorBindings<DomNode>) -> bool {
        self.remembers_answers && bindings.relative_anchor.is_none() && !bindings.matching_host_argument
    }

    #[inline]
    fn relation_answer(
        &self,
        _program: &SelectorProgram<QueryAtoms>,
        relation: SelectorNodeID,
        node: DomNode,
    ) -> Option<bool> {
        self.answers.as_ref()?.relations.get(&(relation, node)).copied()
    }

    #[inline]
    fn record_relation_answer(
        &mut self,
        _program: &SelectorProgram<QueryAtoms>,
        relation: SelectorNodeID,
        node: DomNode,
        answer: bool,
    ) {
        if let Some(answers) = self.remembered_answers() {
            answers.relations.insert((relation, node), answer);
        }
    }

    #[inline]
    fn preceding_sibling_prefix(
        &mut self,
        _program: &SelectorProgram<QueryAtoms>,
        relation: SelectorNodeID,
        node: DomNode,
    ) -> Option<RememberedPrefix<(SelectorNodeID, DomNode), DomNode>> {
        // A sequence is keyed by its first sibling, which cannot change during a query. Children of
        // a document or fragment have one too, although the tree gives them no parent.
        let slot = (relation, self.tree().first_sibling(node));
        Some(RememberedPrefix {
            slot,
            // The tables are made with the first answer recorded, and until then every prefix is unknown.
            prefix: self
                .answers
                .as_ref()
                .and_then(|answers| answers.preceding_sibling_prefixes.get(&slot).copied()),
        })
    }

    #[inline]
    fn record_preceding_sibling_prefix(
        &mut self,
        _program: &SelectorProgram<QueryAtoms>,
        _relation: SelectorNodeID,
        slot: (SelectorNodeID, DomNode),
        prefix: PrecedingSiblingPrefix<DomNode>,
    ) {
        if let Some(answers) = self.remembered_answers() {
            answers.preceding_sibling_prefixes.insert(slot, prefix);
        }
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

    #[inline]
    fn sibling_count_of(&self, selector: SelectorNodeID, from_end: bool, node: DomNode) -> Option<u32> {
        self.answers
            .as_ref()?
            .sibling_counts_of
            .get(&(node, selector, from_end))
            .copied()
    }

    #[inline]
    fn record_sibling_count_of(&mut self, selector: SelectorNodeID, from_end: bool, node: DomNode, count: u32) {
        if let Some(answers) = self.remembered_answers() {
            answers.sibling_counts_of.insert((node, selector, from_end), count);
        }
    }

    #[inline]
    fn relative_answer(
        &self,
        _program: &SelectorProgram<QueryAtoms>,
        query: RelativeQueryID,
        anchor: DomNode,
    ) -> Option<bool> {
        self.answers.as_ref()?.relative_queries.get(&(query, anchor)).copied()
    }

    #[inline]
    fn record_relative_answer(
        &mut self,
        _program: &SelectorProgram<QueryAtoms>,
        query: RelativeQueryID,
        anchor: DomNode,
        answer: bool,
        _witness: Option<DomNode>,
    ) {
        if let Some(answers) = self.remembered_answers() {
            answers.relative_queries.insert((query, anchor), answer);
        }
    }
}

/// One query as it runs: its program, evaluated against the DOM.
struct DomQuery<'q> {
    query: &'q DomSelectorProgram,
    evaluator: SelectorEvaluator<DomSubject<'q>>,
}

impl<'q> DomQuery<'q> {
    /// A query that matches one element alone remembers answers only if it can ask one twice.
    ///
    /// # Safety
    /// `query` must describe a live program and callbacks.
    unsafe fn new(query: &'q FfiDomSelectorQuery, matches_one_element: bool) -> Self {
        let program = unsafe { &*query.program };
        let shadow_root = optional_node(query.shadow_root);
        Self {
            query: program,
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
                    attributes: [const { MaybeUninit::uninit() }; 4],
                    spilled_attributes: Vec::new(),
                    remembers_answers: !matches_one_element || program.lone_match_repeats_questions,
                    answers: None,
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
        let query = self.query;
        query.subjects.iter().any(|&subject| {
            let Ok(matches) = self.evaluator.matches_node(&query.program, subject, element, &());
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
    if query_run.query.subjects.is_empty() {
        return;
    }
    let dom = query_run.evaluator.subject.dom;
    // Only an element with the local name every match has can match, and a subtree none of whose elements carries
    // every attribute name each match carries holds no match.
    let local_name = query_run.query.subject_local_name;
    let attribute_names = query_run.query.required_attribute_name_bits(dom);
    let next = |node| optional_node(unsafe { (dom.next_element_in_subtree)(node, root, local_name, attribute_names) });
    let mut candidate = next(root);
    while let Some(element) = candidate {
        if query_run.matches(element) && unsafe { found(context, element) } {
            return;
        }
        candidate = next(element);
    }
}
