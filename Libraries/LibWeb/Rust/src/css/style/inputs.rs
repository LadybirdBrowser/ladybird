/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use smallvec::SmallVec;

use super::*;

/// What the style mirror publishes about a text node's characters. The characters are shared with
/// the document rather than copied. The language tag is the one the text node's DOM parent element
/// resolves to, and is read only where the transform uses one, since a tag the transform never
/// consults must not reach the rendering key.
#[derive(Default)]
pub struct PublishedTextSource {
    pub data: ak::Utf16String,
    pub locale: Option<Vec<u16>>,
    pub is_password_input: bool,
}

/// What an element gives the natural size of its replaced content, which layout resolves against
/// the style of the element's box. See `bridge::FfiReplacedContentInputKind`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum ReplacedContentInput {
    #[default]
    None,
    /// A `<textarea>`'s `cols` and `rows`: its natural size is that many `ch` by that many `lh`.
    TextArea { cols: u32, rows: u32 },
    /// An `<input>`'s `size`, and whether its type makes it a text entry widget, whose default
    /// preferred size is that many `ch` by one line.
    Input { size: u32, is_text_entry: bool },
    /// A `<canvas>`'s `width` and `height`, its natural size in CSS pixels.
    Canvas { width: u32, height: u32 },
    /// The natural size of what an element has loaded, such as a video's or an image's.
    NaturalSize(NaturalSize),
    /// The natural size of an SVG `<image>`'s image, which has decoded.
    DecodedSvgImage(NaturalSize),
}

/// A natural width, height and aspect ratio, any of which can be missing, as raw fixed-point CSS
/// pixels.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct NaturalSize {
    pub width: Option<i32>,
    pub height: Option<i32>,
    /// The numerator and denominator.
    pub aspect_ratio: Option<(i32, i32)>,
}

impl ReplacedContentInput {
    #[must_use]
    pub fn from_raw(kind: u8, present: u8, values: [u32; 4]) -> Self {
        use super::bridge::{FfiReplacedContentInputKind as Kind, FfiReplacedContentInputPresent as Present};
        let has = |value: Present| present & value as u8 != 0;
        let natural_size = || NaturalSize {
            width: has(Present::First).then_some(values[0].cast_signed()),
            height: has(Present::Second).then_some(values[1].cast_signed()),
            aspect_ratio: has(Present::ThirdAndFourth).then_some((values[2].cast_signed(), values[3].cast_signed())),
        };
        match kind {
            kind if kind == Kind::NaturalSize as u8 => Self::NaturalSize(natural_size()),
            kind if kind == Kind::DecodedSvgImage as u8 => Self::DecodedSvgImage(natural_size()),
            kind if kind == Kind::TextArea as u8 => Self::TextArea {
                cols: values[0],
                rows: values[1],
            },
            kind if kind == Kind::Input as u8 || kind == Kind::TextEntryInput as u8 => Self::Input {
                size: values[0],
                is_text_entry: kind == Kind::TextEntryInput as u8,
            },
            kind if kind == Kind::Canvas as u8 => Self::Canvas {
                width: values[0],
                height: values[1],
            },
            _ => Self::None,
        }
    }
}

/// What the style mirror says about the element a text node's box takes its style from: the text's
/// flat-tree parent, which is the slot it is assigned to or else its DOM parent. A text under a
/// shadow root or the document has no element above it and answers with every field cleared.
#[derive(Clone, Copy, Default)]
pub struct TextStyleParentFacts {
    pub has_style_parent: bool,
    pub parent_display_is_contents: bool,
    pub parent_collapses_whitespace: bool,
    pub style_record: u64,
}

/// What an element's published style record says about the box it asks for. The layout tree build
/// reads this for an element that may have no box yet, where the arena has nothing to answer from.
#[derive(Clone, Copy)]
pub struct PublishedBoxFacts {
    pub display: crate::css::display::FfiDisplay,
    pub content_visibility: u8,
    pub position: u8,
    pub float_: u8,
    pub appearance: u8,
}

/// What a published record says about the content a box is generated from.
pub struct PublishedContentFacts {
    pub counters_are_none: bool,
    pub content_is_keyword: bool,
    pub content_is_strings_only: bool,
}

unsafe extern "C" {
    fn web_css_custom_property_data_reference(data: *const std::ffi::c_void);
    fn web_css_custom_property_data_unreference(data: *const std::ffi::c_void);
}

/// A `Web::CSS::CustomPropertyData`, which Rust only names by pointer.
#[repr(C)]
pub(crate) struct CustomPropertyDataObject {
    _opaque: [u8; 0],
    _not_send_or_sync: std::marker::PhantomData<*const ()>,
}

// SAFETY: The data is immutable once built and counts its references atomically, so a shared
// reference to it may be read and taken on any thread. The StyleLayout thread leaves giving one up
// to the host (see `RetainedCustomPropertyData`'s `Drop`).
unsafe impl Sync for CustomPropertyDataObject {}

/// The custom-property environment one element holds, a `Web::CSS::CustomPropertyData` the engine
/// keeps a reference to. The element keeps no copy of its own.
pub(crate) struct RetainedCustomPropertyData {
    data: crate::css::host_shared::HostShared<CustomPropertyDataObject>,
}

impl RetainedCustomPropertyData {
    /// # Safety
    /// `data` must be a live `Web::CSS::CustomPropertyData`.
    pub(super) unsafe fn retain(data: *const std::ffi::c_void) -> Self {
        unsafe { web_css_custom_property_data_reference(data) };
        Self {
            data: crate::css::host_shared::HostShared::new(data.cast()),
        }
    }

    pub(crate) fn data(&self) -> *const std::ffi::c_void {
        self.data.as_ptr().cast()
    }
}

/// The custom-property environment an element or one of its synthetic pseudo-elements holds, with
/// what a move of the environment it inherits reads of it.
#[derive(Clone)]
pub(crate) struct HeldCustomPropertyEnvironment {
    /// The identity the host's object names the environment by.
    pub(crate) identity: u64,
    /// For the element's animation overlay, the environment its style resolved to, which its
    /// animations sampled their custom properties over.
    pub(crate) sampled_over: Option<u64>,
    /// Whether the environment the style resolves to declares custom properties of its own, over
    /// the one it inherits.
    pub(crate) declares: bool,
    pub(crate) data: RetainedCustomPropertyData,
}

/// A fork's environment holds its own reference.
impl Clone for RetainedCustomPropertyData {
    fn clone(&self) -> Self {
        // SAFETY: The row holds a reference, so the data is live.
        unsafe { Self::retain(self.data()) }
    }
}

impl Drop for RetainedCustomPropertyData {
    fn drop(&mut self) {
        // The last reference destroys the data, which gives up its references to its values, counted without atomics
        // on the host's thread. The StyleLayout thread, which lets go of an environment beside the host's task as a
        // fork or a streamed write does, leaves it to the host.
        if crate::stage_thread::is_on_style_layout_thread() {
            CUSTOM_PROPERTY_DATA_LET_GO_BESIDE_THE_HOST
                .lock()
                .unwrap_or_else(std::sync::PoisonError::into_inner)
                .push(self.data() as usize);
            return;
        }
        // SAFETY: The row owns exactly one reference, taken in `retain`.
        unsafe { web_css_custom_property_data_unreference(self.data()) };
    }
}

/// The references to custom-property environments the StyleLayout thread let go of, which the host gives up.
static CUSTOM_PROPERTY_DATA_LET_GO_BESIDE_THE_HOST: std::sync::Mutex<Vec<usize>> = std::sync::Mutex::new(Vec::new());

/// Gives up the references to custom-property environments the StyleLayout thread let go of. On the host's thread, or
/// in a job the host waits for.
pub(crate) fn give_up_custom_property_data_let_go_beside_the_host(_: &crate::stage::MainThread) {
    let let_go = std::mem::take(
        &mut *CUSTOM_PROPERTY_DATA_LET_GO_BESIDE_THE_HOST
            .lock()
            .unwrap_or_else(std::sync::PoisonError::into_inner),
    );
    for data in let_go {
        // SAFETY: Each entry is a reference a dropped environment owned.
        unsafe { web_css_custom_property_data_unreference(data as *const std::ffi::c_void) };
    }
}

/// Compile one rule's selectors, interning the names they test through `atoms`.
pub(super) fn compile_selector_program(
    atoms: &mut DocumentAtoms,
    fold_id_and_class_name_case: bool,
    html_element_namespace: StyleAtomID,
    selectors: &[&CompiledSelector],
    namespaces: NamespaceScope,
    scope: &ScopeChain<'_>,
    counters: &Counters,
) -> SelectorProgram {
    let mut intern = |raw: usize, namespace: Option<StyleAtomID>| -> StyleAtomID {
        let local = atoms.intern_raw(raw);
        let Some(namespace) = namespace else {
            return local;
        };
        atoms.intern_qualified(namespace, local)
    };
    let mut compiler = SelectorCompiler::new(
        &mut intern,
        fold_id_and_class_name_case,
        html_element_namespace,
        namespaces,
    );
    let count_entry = |compiled: &compiler::CompiledEntry, counters: &Counters| {
        if let Some(counter) = compiled.marker.and_then(|marker| marker.counter()) {
            counters.bump(counter);
        }
        counters.bump(Counter::ExactSelectorEntries);
    };
    for selector in selectors {
        count_entry(&compiler.compile_in_scope(selector, scope), counters);
        if selector.styles_every_search_text_match() {
            count_entry(
                &compiler.compile_in_scope_for_current_search_text_match(selector, scope),
                counters,
            );
        }
    }
    compiler.finish()
}

impl RetainedState {
    pub(super) fn push_pending_region(&mut self, regions: &mut Vec<ImpactRegion>, region: ImpactRegion) {
        let before = regions.capacity();
        regions.push(region);
        let after = regions.capacity();
        self.memory.reserve_required(
            MemoryCategory::BatchScratch,
            ((after - before) * size_of::<ImpactRegion>()) as u64,
        );
    }

    /// Says whether the document matches id and class selectors ASCII case-insensitively.
    ///
    /// A document only learns its mode while it is still empty - a parser reads it from the doctype
    /// before the first element arrives, and `document.open()` has already removed every element and
    /// every sheet by the time it resets it - so no rule compiled against the other folding and no
    /// fact published under it can outlive the change.
    pub fn set_fold_id_and_class_name_case(&mut self, fold: bool) {
        self.fold_id_and_class_name_case = fold;
    }

    /// The HTML namespace, for an HTML document, or none for any other kind. It is what decides
    /// whether an attribute name from the legacy list compares its value case-insensitively.
    pub fn set_html_element_namespace(&mut self, namespace: StyleAtomID) {
        self.html_element_namespace = namespace;
    }

    #[must_use]
    pub fn tree(&self) -> &StyleNodeTree {
        &self.tree
    }

    #[must_use]
    pub fn connected_element_count(&self) -> u32 {
        self.tree.connected_element_count()
    }

    #[must_use]
    #[cfg(test)]
    pub(super) fn program(&self) -> &StyleSheetProgram {
        &self.program
    }

    pub(super) fn add_routing_rule(&mut self, rule: RuleID, program: SelectorProgramID) {
        self.programs.settle_memory(&mut self.memory);
        // A detached sheet's routes were shed, and reattachment restores the current routes of
        // every live rule in the sheet, so routes added for a rule edited while its sheet is
        // detached would come back twice. The exclusion covers the edit until the sheet reattaches.
        if self
            .sheets_excluded_from_routing
            .contains(self.program.rule_sheet(rule).0 as usize)
        {
            return;
        }
        let routing = Arc::make_mut(&mut self.routing);
        routing.add_rule(rule, program, &self.programs);
    }

    /// The process-global atom for one interned name identity.
    ///
    /// Selector names and DOM facts intern through here and nowhere else. Two tables keyed by the
    /// same word but assigning their own sequences would compare unequal for the same name, which
    /// is a silent failure to match rather than a loud one.
    pub fn intern_atom(&mut self, raw: usize) -> StyleAtomID {
        self.atoms.intern_cpp_raw(raw)
    }

    /// The document-local atom for a name qualified by a namespace.
    ///
    /// `[ns|x]` names an attribute that `[x]` does not, and one element can carry both. So the
    /// qualified form is a name of its own: the attribute is published under it as well as under
    /// its local name, and the selector that names a namespace tests only this one.
    pub fn intern_qualified_atom(&mut self, namespace: StyleAtomID, name: StyleAtomID) -> StyleAtomID {
        self.atoms.intern_qualified(namespace, name)
    }

    /// Record what a custom property's name atom spells, and the fly string it is.
    ///
    /// # Safety
    /// `raw` must be zero or a live `AK::Utf16FlyString` raw representation.
    pub unsafe fn note_custom_property_name(&mut self, name: StyleAtomID, raw: usize, text: &[u16]) {
        if unsafe { self.custom_property_environments.note_name(name, raw, text) } {
            self.counters.bump(Counter::CustomPropertyNamesPublished);
        }
    }

    // A stable declaration owner changes contents without changing its address. Reserve zero for
    // absence and one for the initial block, then issue fresh identities for subsequent edits.
    pub(crate) fn next_declaration_block_version(&self) -> u32 {
        next_declaration_block_version(&self.declaration_block_version)
    }

    /// Mints declaration blocks from `versions`, which the document's host mints from as well, rather than from
    /// versions of the engine's own. The engine has minted none yet.
    pub(crate) fn share_declaration_block_versions(&mut self, versions: Arc<std::sync::atomic::AtomicU32>) {
        debug_assert_eq!(
            self.declaration_block_version
                .load(std::sync::atomic::Ordering::Relaxed),
            1
        );
        self.declaration_block_version = versions;
    }

    pub(super) fn compile_selectors(
        &mut self,
        selectors: &[&CompiledSelector],
        namespaces: NamespaceScope,
        scope: &ScopeChain<'_>,
        reusable: Option<SelectorProgramID>,
    ) -> SelectorProgramID {
        let compiled = compile_selector_program(
            &mut self.atoms,
            self.fold_id_and_class_name_case,
            self.html_element_namespace,
            selectors,
            namespaces,
            scope,
            &self.counters,
        );
        self.note_attribute_value_text_names(&compiled);
        if let Some(reusable) = reusable
            && self.programs.get(reusable) == &compiled
        {
            return reusable;
        }
        let program = self.programs.add(compiled);
        self.selector_programs_need_sweep |= reusable.is_some();
        self.programs.settle_memory(&mut self.memory);
        program
    }

    /// Record the attribute names whose values `program` reads as text.
    pub(super) fn note_attribute_value_text_names(&mut self, program: &SelectorProgram) {
        if program
            .attribute_value_text_names()
            .all(|name| self.attribute_value_text_names.contains(&name))
        {
            return;
        }
        Arc::make_mut(&mut self.attribute_value_text_names).extend(program.attribute_value_text_names());
        self.attribute_value_text_requirements_version += 1;
    }

    /// Moves whenever an attribute name comes to require its value text: a selector's here, or an
    /// `attr()`'s anywhere in the process.
    pub fn attribute_value_text_requirements_version(&self) -> u64 {
        with_attr_names_read(self.selector_attribute_value_text_requirements_version())
    }

    /// Moves whenever an attribute name comes to require its value text for a selector here.
    pub fn selector_attribute_value_text_requirements_version(&self) -> u64 {
        self.attribute_value_text_requirements_version
    }

    /// The attribute names whose value text a selector here reads.
    pub(crate) fn selector_attribute_value_text_names(&self) -> &SelectorValueTextNames {
        &self.attribute_value_text_names
    }

    /// Whether the host records what the values of an attribute name spell: for a selector whose
    /// operator an atom cannot answer, or for an `attr()`, which reads an attribute in no namespace
    /// by its local name.
    #[must_use]
    pub fn attribute_name_requires_value_text(&self, name: StyleAtomID) -> bool {
        self.facts
            .attribute_name_keys(name)
            .any(|key| self.attribute_value_text_names.contains(&key))
            || self
                .facts
                .attribute_substitution_name(name)
                .is_some_and(crate::css::parser::arbitrary_substitution::attr_may_read_name)
    }

    #[must_use]
    #[cfg(test)]
    pub fn memory(&self) -> &MemoryController {
        &self.memory
    }

    /// Whether the last transaction planned nothing but derived child reactions, read once.
    pub fn take_only_derived_child_reactions(&mut self) -> bool {
        std::mem::take(&mut self.last_transaction_only_derived_child_reactions)
    }

    /// Apply an accepted input to the authoritative fact arrangement.
    ///
    /// A fact exists in the store exactly because a mutation published it, which is what lets the
    /// evaluator distinguish "this element has no such class" from "nothing ever told me about this
    /// element".
    pub(super) fn apply_to_facts_without_settling(&mut self, key: InputKey, new: InputValue) {
        match (key, new) {
            (InputKey::LocalFeature(node, feature), InputValue::Feature(value)) => match feature {
                LocalFeatureKey::TagName => {
                    if let FeatureValue::Atom(atom) = value {
                        self.facts.set_tag(node, atom, &mut self.memory);
                    }
                }
                LocalFeatureKey::PartExposure => self.facts.set_part_exposure(
                    node,
                    match value {
                        FeatureValue::Atom(atom) => atom,
                        _ => StyleAtomID::NONE,
                    },
                ),
                LocalFeatureKey::Language => self.facts.set_language(
                    node,
                    match value {
                        FeatureValue::Atom(atom) => atom,
                        _ => StyleAtomID::NONE,
                    },
                ),
                LocalFeatureKey::Directionality => self.facts.set_directionality(
                    node,
                    match value {
                        FeatureValue::Atom(atom) => atom,
                        _ => StyleAtomID::NONE,
                    },
                    &mut self.memory,
                ),
                LocalFeatureKey::HeadingLevel => self.facts.set_heading_level(
                    node,
                    match value {
                        FeatureValue::Number(level) => level as u8,
                        _ => 0,
                    },
                ),
                LocalFeatureKey::FoldedTagName => self.facts.set_folded_tag(
                    node,
                    match value {
                        FeatureValue::Atom(atom) => atom,
                        _ => StyleAtomID::NONE,
                    },
                    &mut self.memory,
                ),
                // Parts and custom states are published as complete sets after their individual
                // journal deltas have been recorded. Arrival is only a routing key.
                LocalFeatureKey::Part(_) | LocalFeatureKey::CustomState(_) | LocalFeatureKey::ArrivingFacts => {}
                // A text node is not a style node, so nothing in the tree can say it is there.
                // `Present` on this key means the element is empty.
                LocalFeatureKey::Emptiness => self.facts.set_has_text_content(node, !value.holds()),
                LocalFeatureKey::Id => self.facts.set_id(
                    node,
                    match value {
                        FeatureValue::Atom(atom) => atom,
                        _ => StyleAtomID::NONE,
                    },
                    &mut self.memory,
                ),
                LocalFeatureKey::Class(class) => self.facts.set_class(node, class, value.holds(), &mut self.memory),
                LocalFeatureKey::Attribute(name) => {
                    // The value's atom rides on the same delta. Presence is what routing reads; the
                    // value is what an exact test compares, and a cold pass has no DOM to ask.
                    let atom = match value {
                        FeatureValue::Atom(atom) => atom,
                        _ => StyleAtomID::NONE,
                    };
                    self.facts
                        .set_attribute(node, name, atom, value.holds(), &mut self.memory);
                }
            },
            (InputKey::State(node, fact), InputValue::State(value)) => {
                self.facts.set_state(node, fact, value, &mut self.memory);
            }
            _ => {}
        }
    }

    /// The routing keys of every fact an arriving element announced, read back off the element.
    ///
    /// A fact is in the store by the time routing runs, so this is what the individual inputs would
    /// have published between them - a class each, the tag, the id, each attribute name.
    #[must_use]
    pub(super) fn routing_keys_of_arriving_facts(&self, node: StyleNodeID) -> SmallVec<[RoutingKey; 4]> {
        let mut keys = SmallVec::new();

        let tag = self.facts.tag_of_node(node);
        if !tag.is_none() {
            keys.push(RoutingKey::TagName(tag));
        }
        let folded_tag = self.facts.folded_tag_of_node(node);
        if !folded_tag.is_none() && folded_tag != tag {
            keys.push(RoutingKey::TagName(folded_tag));
        }
        let id = self.facts.id_of_node(node);
        if !id.is_none() {
            keys.push(RoutingKey::Id(id));
        }
        for &class in self.facts.classes_of_node(node) {
            keys.push(RoutingKey::Class(class));
        }
        for attribute in self.facts.attributes_of_node(node) {
            for key in self.facts.attribute_name_keys(attribute) {
                keys.push(RoutingKey::AttributeName(key));
            }
        }
        if !self.facts.language_of(node).is_none() {
            keys.push(RoutingKey::Language);
        }
        let directionality = self.facts.directionality_of(node);
        if !directionality.is_none() {
            keys.push(RoutingKey::Directionality(directionality));
        }
        if let Some(row) = self.facts.primary().row_of(node) {
            for &part in self.facts.primary().parts_of(row) {
                keys.push(RoutingKey::Part(part));
            }
            for &state in self.facts.primary().custom_states_of(row) {
                keys.push(RoutingKey::CustomState(state));
            }
        }
        for fact in self.facts.states_of_node(node).facts() {
            keys.push(RoutingKey::State(fact));
        }
        // An element arrives empty or not, and either way that is a positional truth about it that
        // nothing else in this transaction says.
        keys.push(RoutingKey::Structural);

        keys
    }

    pub(super) fn settled_tree_relations(&self, node: StyleNodeID) -> TreeRelations {
        TreeRelations {
            parent: self.tree.parent(node),
            previous_element_sibling: self.tree.previous_element_sibling(node),
            next_element_sibling: self.tree.next_element_sibling(node),
            tree_scope: self.tree.tree_scope(node),
            assigned_slot: self.tree.assigned_slot_of(node),
        }
    }

    pub(super) fn depth_recompute_nodes(
        &self,
        staged_rows: &[(StyleNodeID, Option<TreeRelations>, Option<TreeRelations>)],
    ) -> HashSet<StyleNodeID> {
        staged_rows
            .iter()
            .filter_map(|&(node, before, relations)| {
                let relations = relations?;
                (before.is_none() || self.tree.parent(node) != relations.parent).then_some(node)
            })
            .collect()
    }

    pub(super) fn link(&mut self, node: StyleNodeID, new: TreeRelations) {
        self.tree.set_parent(node, new.parent);
        self.tree.set_next_element_sibling(node, new.next_element_sibling);
        self.tree
            .set_previous_element_sibling(node, new.previous_element_sibling);
        if let Some(next) = new.next_element_sibling {
            self.tree.set_previous_element_sibling(next, Some(node));
        }
        match new.previous_element_sibling {
            Some(previous) => self.tree.set_next_element_sibling(previous, Some(node)),
            None => {
                if let Some(parent) = new.parent {
                    self.tree.set_first_element_child(parent, Some(node));
                }
            }
        }

        if new.tree_scope != TreeScopeID::DOCUMENT {
            self.tree.enable_tree_scopes(&mut self.memory);
        }
        if self.tree.has_tree_scopes() {
            self.tree.set_tree_scope(node, new.tree_scope);
        }

        // The slot a slottable is assigned to is its parent in the flat tree, and `::slotted()`
        // walks back along it. A slot name change reassigns it with no DOM mutation at all, so the
        // relation has to be carried by the delta rather than derived from the tree.
        self.tree.set_assigned_slot(node, new.assigned_slot, &mut self.memory);
    }

    /// A fact whose value is an atom is absent when the atom is, so the two are one conversion.
    pub(super) fn atom_or_absent(atom: StyleAtomID) -> FeatureValue {
        match atom.is_none() {
            true => FeatureValue::Absent,
            false => FeatureValue::Atom(atom),
        }
    }

    /// Report the element's resolved language tag.
    /// Record the element's namespace.
    ///
    /// An element's namespace is fixed when it is created, so this is a fact the store holds rather
    /// than an input that moves: nothing routes from it, and no journal entry is needed.
    #[cfg(test)]
    pub fn set_element_namespace(&mut self, node: StyleNodeID, namespace: StyleAtomID) {
        self.facts.set_namespace(node, namespace);
    }

    /// Replace the element facts the style computation's adjustments read.
    pub fn set_element_adjustment_facts(&mut self, node: StyleNodeID, facts: u32) {
        self.computed_group_sets.set_adjustment_facts(node, facts);
    }

    /// The element facts the store holds, as `bridge::element_adjustment_fact` names them. A text
    /// node and a retired identity hold none.
    #[must_use]
    pub fn element_adjustment_facts(&self, node: StyleNodeID) -> u32 {
        self.computed_group_sets.adjustment_facts(node)
    }

    /// Replace the element facts a layout row built for the element records.
    pub fn set_element_construction_facts(&mut self, node: StyleNodeID, facts: u32) {
        self.computed_group_sets.set_construction_facts(node, facts);
    }

    /// Record what the element gives the natural size of its replaced content.
    pub fn set_element_replaced_content_input(&mut self, node: StyleNodeID, input: ReplacedContentInput) {
        if input == ReplacedContentInput::None {
            self.replaced_content_inputs.remove(&node);
        } else {
            self.replaced_content_inputs.insert(node, input);
        }
    }

    /// What the element gives the natural size of its replaced content.
    #[must_use]
    pub fn element_replaced_content_input(&self, node: StyleNodeID) -> ReplacedContentInput {
        self.replaced_content_inputs.get(&node).copied().unwrap_or_default()
    }

    /// Record which principal box the element asks for.
    pub fn set_element_box_kind(&mut self, node: StyleNodeID, box_kind: super::bridge::ElementBoxKind) {
        self.computed_group_sets.set_box_kind(node, box_kind);
    }

    /// Which principal box the element asks for. A text node and a retired identity ask for
    /// nothing in particular.
    #[must_use]
    pub(crate) fn element_box_kind(&self, node: StyleNodeID) -> super::bridge::ElementBoxKind {
        self.computed_group_sets.box_kind(node)
    }

    /// The facts a layout row built for the node records, as `bridge::element_construction_fact`
    /// names them. A retired identity holds none. A text node has no element columns and holds one
    /// of the facts on its own row: which kind of tree it sits in.
    #[must_use]
    pub fn element_construction_facts(&self, node: StyleNodeID) -> u32 {
        if node.text_index().is_some() {
            return if self.tree.text_is_in_user_agent_shadow_tree(node) {
                super::bridge::element_construction_fact::IS_IN_USER_AGENT_SHADOW_TREE
            } else {
                0
            };
        }
        self.computed_group_sets.construction_facts(node)
    }

    /// The record the element published, which its box is built from: the one a clock frame shows in its place, the
    /// record the element holds, or the one the engine assigned it before it holds one. `None` while the element has
    /// no record: a text node, a retired identity, or an element style has not reached yet.
    #[must_use]
    pub fn element_published_style_record(&self, node: StyleNodeID) -> Option<u64> {
        let shown = self.tick_shown.element(node);
        shown
            .map(computed::FinalStyleRecordID::raw)
            .or_else(|| self.held_style_records.get(&node).copied())
            .or_else(|| {
                self.computed_group_sets
                    .assigned_style_record(node)
                    .map(computed::FinalStyleRecordID::raw)
            })
    }

    /// The style the element published for one pseudo-element kind. `None` while the element
    /// styles no such pseudo-element.
    pub(crate) fn pseudo_published_style_view(
        &self,
        node: StyleNodeID,
        pseudo_kind: u8,
    ) -> Option<crate::css::computed_value_views::ComputedValuesView<'_>> {
        self.published_style_record_view(self.published_pseudo_record(node, pseudo_kind))
    }

    /// The record the element published for one pseudo-element kind. `None` while the element
    /// styles no such pseudo-element.
    #[must_use]
    pub fn pseudo_published_style_record(&self, node: StyleNodeID, pseudo_kind: u8) -> Option<u64> {
        self.published_pseudo_record(node, pseudo_kind)
            .map(computed::FinalStyleRecordID::raw)
    }

    /// The box facts the element's published style record holds. `None` while the element has no
    /// record.
    #[must_use]
    pub fn element_published_box_facts(&self, node: StyleNodeID) -> Option<PublishedBoxFacts> {
        self.published_box_facts(self.published_element_record(node))
    }

    /// The box facts the element's published record for one pseudo-element kind holds. `None`
    /// while the element styles no such pseudo-element.
    #[must_use]
    pub fn pseudo_published_box_facts(&self, node: StyleNodeID, pseudo_kind: u8) -> Option<PublishedBoxFacts> {
        self.published_box_facts(self.published_pseudo_record(node, pseudo_kind))
    }

    /// What the element's published record for one pseudo-element kind says about its generated
    /// content. `None` while the element styles no such pseudo-element.
    #[must_use]
    pub fn pseudo_published_content_facts(&self, node: StyleNodeID, pseudo_kind: u8) -> Option<PublishedContentFacts> {
        let view = self.published_style_record_view(self.published_pseudo_record(node, pseudo_kind))?;
        Some(PublishedContentFacts {
            counters_are_none: view.counter_properties_are_none(),
            content_is_keyword: view.content_is_keyword(),
            content_is_strings_only: view.content_is_strings_only(),
        })
    }

    /// Whether the element's published style record counts a counter down from its own last item,
    /// which nothing short of a full rebuild can renumber.
    #[must_use]
    pub fn element_counter_reset_has_reversed_counter(&self, node: StyleNodeID) -> bool {
        self.published_style_record_view(self.published_element_record(node))
            .is_some_and(crate::css::computed_value_views::ComputedValuesView::counter_reset_has_reversed_counter)
    }

    /// Whether `node` or one of its inclusive ancestors computed `display: none` in its record, as
    /// the record stood before any animation was layered on it: the host's
    /// `has_inclusive_ancestor_with_display_none_ignoring_animations()`. A node that holds no
    /// record, a shadow root or one style has not reached yet, is passed over, as the host passes
    /// over a node that is not an element.
    #[must_use]
    pub(crate) fn has_inclusive_ancestor_with_display_none_ignoring_animations(&self, node: StyleNodeID) -> bool {
        std::iter::successors(Some(node), |&current| self.tree.parent_or_shadow_host(current)).any(|ancestor| {
            self.computed_group_sets
                .assigned_style_record(ancestor)
                .and_then(|record| self.computed_group_sets.style_record_view(record.raw()))
                .is_some_and(|view| {
                    !view.base_payloads.is_empty()
                        && crate::css::computed_value_views::ComputedValuesView::new(SharedPayload::as_pointer_slice(
                            view.base_payloads,
                        ))
                        .display()
                        .is_none()
                })
        })
    }

    /// Whether the element is a `<slot>`, whose children the flat tree takes elsewhere.
    #[must_use]
    pub fn element_is_slot(&self, node: StyleNodeID) -> bool {
        self.facts.is_slot(node)
    }

    fn published_box_facts(&self, style_record: Option<computed::FinalStyleRecordID>) -> Option<PublishedBoxFacts> {
        self.style_record_box_facts(style_record?.raw())
    }

    /// The box facts a style record holds. `None` for a record that holds no payloads.
    #[must_use]
    pub fn style_record_box_facts(&self, style_record: u64) -> Option<PublishedBoxFacts> {
        let payloads = self.computed_group_sets.style_record_payloads(style_record)?;
        let view = crate::css::computed_value_views::ComputedValuesView::new(SharedPayload::as_pointer_slice(payloads));
        Some(PublishedBoxFacts {
            display: view.display(),
            content_visibility: view.content_visibility(),
            position: view.position(),
            float_: view.float_(),
            appearance: view.appearance(),
        })
    }

    /// What the text node's flat-tree parent publishes, for the anonymous inline wrapper a text
    /// under a `display: contents` element needs.
    #[must_use]
    pub fn text_style_parent_facts(&self, node: StyleNodeID) -> TextStyleParentFacts {
        let parent = self
            .tree
            .assigned_slot_of(node)
            .or_else(|| self.tree.text_parent(node))
            .filter(|parent| self.tree.host_of(*parent).is_none() && !self.tree.is_relation_only(*parent));
        let Some(parent) = parent else {
            return TextStyleParentFacts::default();
        };
        let style_record = self.published_element_record(parent);
        let Some(view) = self.published_style_record_view(style_record) else {
            return TextStyleParentFacts::default();
        };
        TextStyleParentFacts {
            has_style_parent: true,
            parent_display_is_contents: view.display().is_contents(),
            parent_collapses_whitespace: view.white_space_collapse()
                == crate::css::css_enums::white_space_collapse::COLLAPSE,
            style_record: style_record.map_or(0, computed::FinalStyleRecordID::raw),
        }
    }

    /// Whether the text node's data is nothing but ASCII whitespace.
    #[must_use]
    pub fn text_is_ascii_whitespace(&self, node: StyleNodeID) -> bool {
        self.tree.text_is_ascii_whitespace(node)
    }

    /// Everything a text node's box renders from, taken in one borrow of the mirror.
    #[must_use]
    pub fn published_text_source(&self, node: StyleNodeID, uses_locale: bool) -> PublishedTextSource {
        let Some(data) = self.tree.text_data(node) else {
            return PublishedTextSource::default();
        };
        PublishedTextSource {
            data: data.clone(),
            locale: uses_locale
                .then(|| self.text_language_tag(node))
                .filter(|tag| !tag.is_empty())
                .map(<[u16]>::to_vec),
            is_password_input: self.tree.text_is_password_input(node),
        }
    }

    /// The element's resolved language tag, empty where it has none.
    #[must_use]
    pub fn element_language_tag(&self, node: StyleNodeID) -> &[u16] {
        self.facts.language_tag_of(node)
    }

    /// The language tag a text node's transform reads: the one its DOM parent element resolves to.
    /// A text node under a shadow root or the document has no element above it and reads none.
    fn text_language_tag(&self, node: StyleNodeID) -> &[u16] {
        self.tree
            .text_parent(node)
            .filter(|&parent| self.tree.host_of(parent).is_none() && !self.tree.is_relation_only(parent))
            .map_or(&[], |parent| self.facts.language_tag_of(parent))
    }

    /// Record the text node's whitespace-only state, as its data now spells it.
    pub fn set_text_is_ascii_whitespace(&mut self, node: StyleNodeID, value: bool) {
        self.tree.set_text_is_ascii_whitespace(node, value, &mut self.memory);
    }

    /// Record the unique node id the document names the element by. It arrives with the identity
    /// and never changes while the element holds it.
    pub fn set_element_unique_node_id(&mut self, node: StyleNodeID, unique_node_id: i64) {
        self.tree.set_unique_node_id(node, unique_node_id, &mut self.memory);
    }

    /// The unique node id the document names the element by, or zero for anything else.
    #[must_use]
    pub fn element_unique_node_id(&self, node: StyleNodeID) -> i64 {
        self.tree.unique_node_id(node)
    }

    /// What a row built for the node is painted and hit-tested with, as `DomPaintFact` names them.
    #[must_use]
    pub fn node_dom_paint_facts(&self, node: StyleNodeID) -> u8 {
        self.tree.dom_paint_facts(node)
    }

    /// Record what a row built for the node is painted and hit-tested with.
    pub fn set_node_dom_paint_facts(&mut self, node: StyleNodeID, facts: u8) {
        self.tree.set_dom_paint_facts(node, facts, &mut self.memory);
    }

    /// The spans a row built for the element takes from its attributes.
    #[must_use]
    pub fn element_table_spans(&self, node: StyleNodeID) -> super::tree::TableSpans {
        self.tree.table_spans(node)
    }

    /// Record the spans a row built for the element takes from its attributes.
    pub fn set_element_table_spans(&mut self, node: StyleNodeID, spans: super::tree::TableSpans) {
        self.tree.set_table_spans(node, spans, &mut self.memory);
    }

    /// Record whether the text node holds the value of a password input.
    pub fn set_text_is_password_input(&mut self, node: StyleNodeID, value: bool) {
        self.tree.set_text_is_password_input(node, value, &mut self.memory);
    }

    /// Record the characters the text node now holds.
    pub fn set_text_data(&mut self, node: StyleNodeID, data: ak::Utf16String) {
        self.tree.set_text_data(node, data);
    }

    /// Record which kind of tree the text node arrived in.
    pub fn set_text_is_in_user_agent_shadow_tree(&mut self, node: StyleNodeID, value: bool) {
        self.tree
            .set_text_is_in_user_agent_shadow_tree(node, value, &mut self.memory);
    }

    /// Whether the element's published style record replaces its contents with a single image.
    #[must_use]
    pub fn element_content_is_single_image(&self, node: StyleNodeID) -> bool {
        self.published_style_record_view(self.published_element_record(node))
            .is_some_and(crate::css::computed_value_views::ComputedValuesView::content_is_single_image)
    }

    /// The tree scope a counter style name `node` uses is looked up from: its own, unless its shadow
    /// tree takes the document's styles, which looks names up in the document's scope instead.
    #[must_use]
    pub(crate) fn counter_style_tree_scope(&self, node: StyleNodeID) -> u32 {
        let tree_scope = self.tree.tree_scope(node);
        if self.program.scope_uses_document_sheets(tree_scope) {
            TreeScopeID::DOCUMENT.0
        } else {
            tree_scope.0
        }
    }

    /// The element's published style record, or its record for one pseudo-element kind, as a view.
    /// `None` while there is no such record.
    #[must_use]
    pub(crate) fn published_style_view(
        &self,
        node: StyleNodeID,
        pseudo_kind: Option<u8>,
    ) -> Option<crate::css::computed_value_views::ComputedValuesView<'_>> {
        let style_record = match pseudo_kind {
            Some(pseudo_kind) => self.published_pseudo_record(node, pseudo_kind),
            None => self.published_element_record(node),
        };
        self.published_style_record_view(style_record)
    }

    /// The published style record `raw_style_record` names, as a view. `None` for zero, which
    /// names no record.
    #[must_use]
    pub(crate) fn published_record_view(
        &self,
        raw_style_record: u64,
    ) -> Option<crate::css::computed_value_views::ComputedValuesView<'_>> {
        self.published_style_record_view(computed::FinalStyleRecordID::from_raw(raw_style_record))
    }

    /// The dependency flags of the published style record `raw_style_record` names. `None` for
    /// zero, which names no record.
    #[must_use]
    pub(crate) fn published_record_dependency_flags(&self, raw_style_record: u64) -> Option<u8> {
        let style_record = computed::FinalStyleRecordID::from_raw(raw_style_record)?;
        self.computed_group_sets
            .style_record_dependency_flags(style_record.raw())
    }

    /// The computed longhand table of the published style record `raw_style_record` names, which
    /// keeps what the record's values were specified as. `None` for zero, which names no record.
    #[must_use]
    pub(crate) fn published_record_longhand_table(
        &self,
        raw_style_record: u64,
    ) -> Option<&crate::css::computed_longhand_table::ComputedLonghandTable> {
        let style_record = computed::FinalStyleRecordID::from_raw(raw_style_record)?;
        let view = self.computed_group_sets.style_record_view(style_record.raw())?;
        // SAFETY: The record is live, and shares its table for as long as it is.
        unsafe { view.longhand_table.as_ref() }
    }

    pub(super) fn published_style_record_view(
        &self,
        style_record: Option<computed::FinalStyleRecordID>,
    ) -> Option<crate::css::computed_value_views::ComputedValuesView<'_>> {
        let payloads = self.computed_group_sets.style_record_payloads(style_record?.raw())?;
        Some(crate::css::computed_value_views::ComputedValuesView::new(
            SharedPayload::as_pointer_slice(payloads),
        ))
    }

    /// Record what an attribute-value atom spells, for the operators an atom cannot answer and for
    /// `attr()`.
    pub fn set_attribute_value_text(&mut self, value: StyleAtomID, text: &[u16]) {
        self.facts.set_attribute_value_text(value, text);
    }

    #[must_use]
    #[cfg(test)]
    pub fn has_attribute_value_text(&self, value: StyleAtomID) -> bool {
        self.facts.has_attribute_value_text(value)
    }

    /// Record what a language atom spells, so `:lang()` can compare its ranges against the tag.
    pub fn set_element_language_text(&mut self, language: StyleAtomID, text: &[u16]) {
        self.counters.bump(Counter::LanguageTextsPublished);
        self.facts.set_language_text(language, text);
    }

    /// See `ElementFactStore::note_attribute_name_forms`.
    pub fn note_attribute_name_forms(&mut self, name: StyleAtomID, forms: index::AttributeNameForms) {
        self.facts.note_attribute_name_forms(name, forms);
    }

    /// Record the id an element answers to, or clear it with atom zero.
    pub fn set_element_id_name(&mut self, node: StyleNodeID, name: StyleAtomID) {
        self.tree.set_element_id_name(node, name, &mut self.memory);
    }

    /// The first element in tree order that answers to `name` inside `tree_scope`.
    #[must_use]
    pub fn element_by_id(&self, tree_scope: TreeScopeID, name: StyleAtomID) -> Option<StyleNodeID> {
        self.tree.element_by_id(tree_scope, name)
    }

    /// The style group payloads the element's published record holds, for a reader that reaches an
    /// element by identity rather than through a layout row. `None` while the element has no
    /// record.
    #[must_use]
    pub fn element_published_style_payloads(&self, node: StyleNodeID) -> Option<&[*const std::ffi::c_void]> {
        let record = self.published_element_record(node)?;
        let payloads = self.computed_group_sets.style_record_payloads(record.raw())?;
        Some(SharedPayload::as_pointer_slice(payloads))
    }

    /// Keep the atom a layout publication names live for as long as the publication does.
    ///
    /// A published SVG reference names an id that may name no element at all, and an atom nothing
    /// answers to has no other owner: a sweep would reclaim it and hand its number to the next
    /// name interned, which the publication would then read as the element it points at.
    pub fn retain_published_atom(&mut self, atom: StyleAtomID) {
        self.atoms.retain_published(atom);
    }

    /// Give up the retention `retain_published_atom` took, as a publication is cleared or replaced.
    pub fn release_published_atom(&mut self, atom: StyleAtomID) {
        self.atoms.release_published(atom);
    }

    /// See `ElementFactStore::note_attribute_substitution_name`.
    pub fn note_attribute_substitution_name(&mut self, name: StyleAtomID, local_name: &[u16]) {
        self.facts.note_attribute_substitution_name(name, local_name);
    }

    pub fn set_shadow_root(&mut self, host: StyleNodeID, shadow_root: StyleNodeID) {
        self.tree.set_shadow_root(host, shadow_root, &mut self.memory);
    }

    /// Replace the ordered list of nodes a slot has assigned to it, text nodes included.
    pub fn set_slot_assigned_nodes(&mut self, slot: StyleNodeID, nodes: &[StyleNodeID]) {
        self.tree.set_assigned_nodes(slot, nodes, &mut self.memory);
    }

    /// Replace the document's top layer, in the order its members were added.
    pub fn set_top_layer_elements(&mut self, members: &[StyleNodeID]) {
        self.tree.set_top_layer(members, &mut self.memory);
    }

    /// Retire text identities as their nodes disconnect.
    pub fn retire_text_style_nodes(&mut self, nodes: impl IntoIterator<Item = StyleNodeID>) {
        self.tree.retire_texts(nodes, &mut self.memory);
    }

    // -- DOM child sequence ------------------------------------------------------------------
    //
    // Nothing selects, styles or invalidates from the DOM child sequence, so its arrivals and
    // departures are spliced directly rather than journaled.

    /// Splice nodes into the DOM child sequence, given as `(node, parent, previous sibling)`
    /// triples of raw identities in tree order, so that each previous sibling is linked first.
    pub fn link_style_nodes_in_dom_order(&mut self, links: &[u32]) {
        for &[node, parent, previous] in links.as_chunks::<3>().0 {
            if let Some(node) = StyleNodeID::from_raw(node) {
                self.tree
                    .link_in_dom_order(node, StyleNodeID::from_raw(parent), StyleNodeID::from_raw(previous));
            }
        }
    }

    pub fn unlink_style_node_from_dom_order(&mut self, node: StyleNodeID, parent: Option<StyleNodeID>) {
        self.tree.unlink_from_dom_order(node, parent);
    }

    /// Mark an identity that stands in the tree only to be named by relations. The document is one:
    /// it owns the DOM child sequence its children hang from, and it is never styled or matched.
    pub fn mark_relation_only_style_node(&mut self, node: StyleNodeID) {
        self.tree.mark_relation_only(node, &mut self.memory);
    }

    // -- Stylesheet program ------------------------------------------------------------------
    //
    // Every CSSOM mutation maps to a precise typed delta. None of them produces a generic document
    // invalidation, and the granularity is what makes that true: a declaration edit journals the
    // declaration field alone, so selector truth is untouched by an edit that cannot change it.

    pub fn add_sheet(&mut self, object: StyleSheetObjectID, origin: CascadeOrigin) -> SheetID {
        // Reserving an unattached sheet identity cannot change any scope's semantic program.
        let sheet = self.program.add_sheet(object, origin);
        self.settle_program();
        sheet
    }

    /// A sheet whose routes were shed while it was detached contributes routes again the moment
    /// it reattaches. The registry must be whole before the attachment's transaction plans, so
    /// this runs at recording time rather than waiting for the next sweep.
    pub(super) fn restore_routing_for_reattached_sheet(&mut self, sheet: SheetID) {
        if !self.sheets_excluded_from_routing.set(sheet.0 as usize, false).0 {
            return;
        }
        let rules = self
            .program
            .rules_in_sheet(sheet)
            .into_iter()
            .filter(|&rule| self.program.rule_is_live(rule))
            .filter_map(|rule| Some((rule, self.program.rule_version(rule).selector_program?)));
        let programs = &self.programs;
        let routing = Arc::make_mut(&mut self.routing);
        for (rule, program) in rules {
            routing.add_rule(rule, program, programs);
        }
        routing.settle_memory(&mut self.memory);
    }

    /// Record the animation names an element's computed style references.
    ///
    /// This is an index, not an input: an element whose `animation-name` changed was already
    /// recomputed by whatever changed it. What the index is for is the other direction - a
    /// `@keyframes` rule finding the elements running the animation it describes.
    pub fn set_element_animation_names(&mut self, node: StyleNodeID, names: &[StyleAtomID]) {
        self.facts.set_animation_names(node, names, &mut self.memory);
    }

    /// Keep the custom-property environment an element now holds; a null `data` is none. Only
    /// elements that hold one have an entry.
    ///
    /// `data`, if any, is the environment named by `identity`.
    pub(crate) fn set_element_custom_property_data(
        &mut self,
        node: StyleNodeID,
        data: Option<RetainedCustomPropertyData>,
        identity: u64,
        sampled_over: Option<u64>,
        declares: bool,
    ) {
        let Some(data) = data else {
            self.element_custom_property_data.remove(&node);
            return;
        };
        if self
            .element_custom_property_data
            .get(&node)
            .is_some_and(|existing| existing.data.data() == data.data())
        {
            return;
        }
        self.element_custom_property_data.insert(
            node,
            HeldCustomPropertyEnvironment {
                identity,
                sampled_over,
                declares,
                data,
            },
        );
    }

    /// The custom-property environment an element holds, or null.
    pub(crate) fn element_custom_property_data(&self, node: StyleNodeID) -> *const std::ffi::c_void {
        self.element_custom_property_data
            .get(&node)
            .map_or(std::ptr::null(), |held| held.data.data())
    }

    /// Keep the custom-property environment one of an element's synthetic pseudo-elements now
    /// holds; a null `data` is none.
    ///
    /// `data`, if any, is the environment named by `identity`.
    pub(crate) fn set_pseudo_element_custom_property_data(
        &mut self,
        node: StyleNodeID,
        pseudo: u8,
        data: Option<RetainedCustomPropertyData>,
        identity: u64,
    ) {
        // Clearing is the common install, and must not make an entry only to drop it again.
        let Some(data) = data else {
            let Some(environments) = self.pseudo_element_custom_property_data.get_mut(&node) else {
                return;
            };
            environments.retain(|(kind, _)| *kind != pseudo);
            if environments.is_empty() {
                self.pseudo_element_custom_property_data.remove(&node);
            }
            return;
        };
        let held = HeldCustomPropertyEnvironment {
            identity,
            sampled_over: None,
            declares: false,
            data,
        };
        let environments = self.pseudo_element_custom_property_data.entry(node).or_default();
        match environments.iter_mut().find(|(kind, _)| *kind == pseudo) {
            Some((_, environment)) if environment.data.data() == held.data.data() => {}
            Some((_, environment)) => *environment = held,
            None => environments.push((pseudo, held)),
        }
    }

    /// The custom-property environments an element's synthetic pseudo-elements hold, by kind.
    pub(crate) fn pseudo_element_custom_property_environments(
        &self,
        node: StyleNodeID,
    ) -> impl Iterator<Item = (u8, *const std::ffi::c_void)> + '_ {
        self.pseudo_element_custom_property_data
            .get(&node)
            .into_iter()
            .flatten()
            .map(|(kind, environment)| (*kind, environment.data.data()))
    }

    /// Record whether the element's style reads its custom-property environment other than through
    /// `var()`: through `if()`, `inherit()`, a custom function or a style container query. A moved
    /// environment computes such an element again rather than handing it the moved one.
    pub fn set_element_recomputes_on_environment_move(&mut self, node: StyleNodeID, recomputes: bool) {
        if recomputes {
            self.environment_move_recompute_nodes.insert(node);
        } else {
            self.environment_move_recompute_nodes.remove(&node);
        }
    }

    #[must_use]
    pub fn element_recomputes_on_environment_move(&self, node: StyleNodeID) -> bool {
        self.environment_move_recompute_nodes.contains(&node)
    }

    /// Record the element-backed pseudo-element kind an element in its host's shadow tree stands
    /// for, one plus the kind, or zero for none.
    pub fn set_element_associated_pseudo_kind(&mut self, node: StyleNodeID, pseudo_kind_plus_one: u8) {
        self.computed_group_sets
            .set_associated_pseudo_kind(node, pseudo_kind_plus_one);
        if pseudo_kind_plus_one != 0
            && let Some(host) = self.tree.shadow_host_of(node)
        {
            let backing_elements = self.backing_elements.entry(host).or_default();
            if !backing_elements.contains(&node) {
                backing_elements.push(node);
            }
        }
    }

    /// The elements of `host`'s shadow tree that stand for one of its element-backed
    /// pseudo-elements of `kinds`, a bit per kind.
    pub(super) fn backing_elements(&self, host: StyleNodeID, kinds: u64) -> impl Iterator<Item = StyleNodeID> {
        self.backing_elements
            .get(&host)
            .into_iter()
            .flatten()
            .copied()
            .filter(move |&node| {
                self.tree.shadow_host_of(node) == Some(host)
                    && self
                        .computed_group_sets
                        .associated_pseudo_kind(node)
                        .is_some_and(|kind| kinds & 1_u64.checked_shl(u32::from(kind)).unwrap_or(0) != 0)
            })
    }

    /// What the last layout commit and scroll state say of a container's box.
    pub(crate) fn layout_style_snapshot(
        &self,
        node: StyleNodeID,
    ) -> Option<crate::layout::style_snapshot::LayoutStyleSnapshotRow> {
        self.layout_style_snapshots.get(&node).copied()
    }

    /// Take the box geometry a completed layout commit gathered, keeping each row's scroll state.
    pub(crate) fn apply_layout_style_snapshot_commit(
        &mut self,
        rows: &[crate::layout::style_snapshot::CommittedGeometry],
    ) {
        for geometry in rows {
            let row = self.layout_style_snapshots.entry(geometry.node).or_default();
            row.content_width_raw = geometry.content_width_raw;
            row.content_height_raw = geometry.content_height_raw;
            row.has_committed_box = geometry.has_committed_box;
        }
    }

    /// Record the scroll state a scroll-state container's queries read, as the host snapshots it
    /// after layout, keeping the row's box geometry.
    pub fn set_element_scroll_state(
        &mut self,
        node: StyleNodeID,
        stuck: u8,
        snapped: u8,
        scrollable: u8,
        scrolled: u8,
    ) {
        let row = self.layout_style_snapshots.entry(node).or_default();
        row.stuck = stuck;
        row.snapped = snapped;
        row.scrollable = scrollable;
        row.scrolled = scrolled;
    }

    /// Record the identity of the counter-style registry a tree scope's style scope has now.
    pub fn set_counter_style_environment_identity(&mut self, tree_scope: TreeScopeID, identity: u64) {
        self.counter_style_environment_identities.insert(tree_scope, identity);
    }

    /// Record the style record an element holds; zero is none. Only elements that hold one have an
    /// entry. Keep it alive until the host replaces it, even when the engine publishes a newer
    /// assignment. Whether the element is a query container is what that record says.
    pub fn set_held_style_record(&mut self, node: StyleNodeID, style_record: u64) {
        let previous = if style_record == 0 {
            self.held_style_records.remove(&node)
        } else {
            self.held_style_records.insert(node, style_record)
        };
        if previous != Some(style_record) {
            if style_record != 0 {
                self.computed_group_sets.pin_style_record(style_record);
            }
            if let Some(previous) = previous {
                self.computed_group_sets.unpin_style_record(previous);
            }
        }
        self.set_element_container_query_inputs(node, style_record);
        // A `rem` the host resolves reads the font of the record it holds for the document element,
        // which it can install in the middle of a style update.
        if self.computed_group_sets.adjustment_facts(node) & bridge::element_adjustment_fact::IS_DOCUMENT_ELEMENT != 0 {
            self.held_root_font_inputs = computed::FinalStyleRecordID::from_raw(style_record)
                .and_then(|record| self.root_font_inputs_from_record(record));
        }
    }

    /// Record that a child of an element or shadow root explicitly inherits a non-inherited
    /// property, as the host marks the node. The mark lasts as long as the node's identity.
    pub fn note_children_explicitly_inherit(&mut self, node: StyleNodeID) {
        self.children_explicitly_inherit_marks.insert(node);
    }

    /// Whether, and as what, an element is a query container.
    pub(super) fn container_query_inputs(&self, node: StyleNodeID) -> Option<&tree::ContainerQueryInputRow> {
        self.container_query_inputs.get(node)
    }

    /// Refresh whether, and as what, an element is a query container from a record it is published
    /// with. A record without its box group makes it none, and so does one naming no container
    /// type or name: such an element holds no row.
    pub(super) fn set_element_container_query_inputs(&mut self, node: StyleNodeID, style_record: u64) {
        match self.container_query_input_row(style_record, false) {
            Some(row) => self.container_query_inputs.set(node, row),
            None => self.container_query_inputs.clear(node),
        }
    }

    /// What a record says of its element as a query container: none for a record without its box
    /// group, nor, unless `any_element` asks for every element's, for one naming no container type
    /// or name.
    pub(super) fn container_query_input_row(
        &self,
        style_record: u64,
        any_element: bool,
    ) -> Option<tree::ContainerQueryInputRow> {
        let payloads = self
            .computed_group_sets
            .style_record_payloads(style_record)
            .filter(|payloads| payloads.len() > crate::css::computed_value_types::STYLE_GROUP_INDEX_BOX)?;
        let values =
            crate::css::computed_value_views::ComputedValuesView::new(SharedPayload::as_pointer_slice(payloads));
        let box_values = values.box_values();
        if !any_element
            && !box_values.is_size_container
            && !box_values.is_inline_size_container
            && !box_values.is_scroll_state_container
            && box_values.container_name.raws().is_empty()
        {
            return None;
        }
        let names = box_values
            .container_name
            .raws()
            .iter()
            .map(|raw| match unsafe { ak::utf16_string_units(raw) } {
                ak::Utf16StringUnits::Ascii(units) => units.iter().copied().map(u16::from).collect(),
                ak::Utf16StringUnits::Utf16(units) => units.to_vec(),
            })
            .collect();
        Some(tree::ContainerQueryInputRow {
            style_record,
            names,
            is_size_container: box_values.is_size_container,
            is_inline_size_container: box_values.is_inline_size_container,
            is_scroll_state_container: box_values.is_scroll_state_container,
            writing_mode: values.writing_mode(),
            direction: values.direction(),
        })
    }

    /// Record the CSS animations the host holds for one of an element's animation lists, in the
    /// order it holds them: each one's name, and the definition the last plan applied to it.
    pub fn set_element_css_defined_animations(
        &mut self,
        node: StyleNodeID,
        slot: animations::AnimationSlot,
        names: Box<[crate::css::css_string::CssString]>,
        definitions: &[super::bridge::FfiAppliedAnimationDefinition],
    ) {
        self.css_defined_animations.set(node, slot, names, definitions);
    }

    /// The CSS animations the host holds for one of an element's animation lists.
    #[must_use]
    pub(crate) fn element_css_defined_animations(
        &self,
        node: StyleNodeID,
        slot: animations::AnimationSlot,
    ) -> &[animations::CssDefinedAnimation] {
        self.css_defined_animations.list(node, slot)
    }

    /// The `@keyframes` the document's style scopes define.
    #[must_use]
    pub(crate) fn animation_keyframes(&self) -> &animations::AnimationKeyframes {
        &self.animation_keyframes
    }

    /// Where an element stands among its siblings, as a tree-counting function reads it: how many
    /// there are and its one-based index among them.
    #[must_use]
    pub(crate) fn element_sibling_position(&self, node: StyleNodeID) -> Option<(u32, u32)> {
        self.sibling_position(node)
            .map(|position| (position.count, position.index))
    }

    /// The registry of the custom properties the document registers.
    #[must_use]
    pub(crate) fn custom_property_registry(&self) -> &crate::css::custom_properties::CustomPropertyRegistry {
        self.document_style_computation_inputs.custom_property_registry()
    }

    /// The effects one of an element's animation lists holds, as the host described them.
    #[must_use]
    pub(crate) fn element_animation_effects(
        &self,
        node: StyleNodeID,
        slot: animations::AnimationSlot,
    ) -> &[super::effect_descriptions::PublishedEffect] {
        self.animation_effect_descriptions.effects(node, slot)
    }

    /// Keeps the timing the host samples one of an element's described effects with, and answers the
    /// key the effect samples its keyframes at (see
    /// [`super::effect_descriptions::AnimationEffectDescriptions::time_effect`]).
    ///
    /// # Safety
    /// The linear points `easing` names must be live.
    pub(crate) unsafe fn time_element_animation_effect(
        &mut self,
        node: StyleNodeID,
        slot: animations::AnimationSlot,
        identity: u64,
        timing: &crate::css::style_compute::FfiEffectTiming,
        easing: &crate::css::easing::FfiEasingDescriptor,
        host_key: f64,
    ) -> Option<f64> {
        unsafe {
            self.animation_effect_descriptions
                .time_effect(node, slot, identity, timing, easing, host_key)
        }
    }

    /// Record the custom properties an element declares or references. Also an index rather than an
    /// input, and for the same reason: it answers which elements an `@property` registration reaches.
    pub fn set_element_custom_property_names(
        &mut self,
        node: StyleNodeID,
        names: &[StyleAtomID],
        uses_unnamed: bool,
        uses_custom_functions: bool,
    ) {
        self.facts.set_custom_property_names(node, names, &mut self.memory);
        self.facts
            .set_uses_unnamed_custom_properties(node, uses_unnamed, &mut self.memory);
        self.facts
            .set_uses_custom_functions(node, uses_custom_functions, &mut self.memory);
    }

    pub fn set_tree_scope_root(&mut self, tree_scope: TreeScopeID, root: StyleNodeID) {
        if tree_scope == TreeScopeID::DOCUMENT {
            return;
        }
        let index = tree_scope.0 as usize;
        if let Some(previous_root) = self.scope_roots.get(index).copied().flatten()
            && previous_root != root
        {
            self.scope_by_root.remove(previous_root);
        }
        self.scope_roots.insert(index, Some(root));
        self.scope_by_root.insert(root, tree_scope);
        // The root is a node selectors reach and nothing publishes features for, so this is where it
        // gets a row of its own.
        self.facts.ensure_row(root);
    }

    pub(super) fn scope_root(&self, tree_scope: TreeScopeID) -> Option<StyleNodeID> {
        self.scope_roots.get(tree_scope.0 as usize).copied().flatten()
    }

    /// Everything a sheet attached to these scopes can decide.
    ///
    /// A scope-local sheet reaches the tree it is attached to, and out of it only through `:host`,
    /// `::slotted()` and `::part()` - which reach the host and the nodes slotted into it. So a sheet
    /// in a shadow root can be bounded even when its rules dispatch on nothing enumerable, where the
    /// document's own scope has no bound narrower than the document.
    /// Where a sheet decides, taking in both the scopes it is attached to now and the ones it was
    /// attached to when the transaction began.
    pub(super) fn scopes_of_sheet(
        &self,
        sheet: SheetID,
        departed_sheet_scopes: &[(SheetID, TreeScopeID)],
    ) -> Vec<TreeScopeID> {
        let mut scopes = self.program.sheet_scopes(sheet);
        for &(departed_sheet, scope) in departed_sheet_scopes {
            if departed_sheet == sheet && !scopes.contains(&scope) {
                scopes.push(scope);
            }
        }
        scopes
    }

    pub(super) fn regions_reachable_from_scopes(
        &self,
        scopes: &[TreeScopeID],
        subject_leaves_scope: bool,
        host_is_a_subject: bool,
    ) -> Option<Vec<ImpactRegion>> {
        if scopes.is_empty() {
            return None;
        }
        let mut regions = Vec::new();
        for &scope in scopes {
            let root = match self.scope_root(scope) {
                Some(root) => root,
                // The document's own scope records no root and has no bound narrower than itself,
                // which is a different thing from a shadow scope that has not named one yet.
                None if scope == TreeScopeID::DOCUMENT => return None,
                // A shadow scope whose root has never been named holds no element the engine knows
                // about: every element in a shadow tree takes its place in the style tree through
                // that root, so a scope that has not named one has nothing inside it for a sheet
                // attached there to decide. Attaching a sheet to a shadow root numbers its scope
                // without populating it, which is what `attachShadow` immediately followed by
                // `adoptedStyleSheets` does, and answering that with the document restyles the page
                // once per component.
                //
                // A program that leaves its scope reaches the host instead, so it needs the root to
                // find one, and a scope with no root names no host either.
                None if !subject_leaves_scope => continue,
                None => continue,
            };
            match subject_leaves_scope {
                // `:host` and `::slotted()` describe the host and what is slotted into it, which are
                // in the host's own tree and not in the one the sheet is attached to. A rule using
                // them says nothing about the shadow tree, so naming it would be the widest part of
                // the answer for the narrowest reason.
                // A root with no host is a shadow tree whose host has not taken its place in the
                // style tree, so the tree hangs off nothing the engine can reach and the rule
                // decides for no element that exists. The host computes its style from scratch when
                // it does arrive, which is what makes saying so safe rather than optimistic.
                true => match self.tree.host_of(root) {
                    Some(host) => {
                        if host_is_a_subject {
                            regions.push(ImpactRegion::Node(host));
                        }
                        regions.push(ImpactRegion::StrictSubtree(host));
                    }
                    None => continue,
                },
                false => regions.push(ImpactRegion::Subtree(root)),
            }
        }
        Some(regions)
    }

    /// Everything a named rule in these scopes can reach through its consumers.
    ///
    /// Unlike a selector program, a named rule has no one subject position: an `@keyframes` or
    /// `@property` rule can be referenced both inside its shadow tree and by a declaration matching
    /// `:host` or `::slotted()`. The consumer index identifies the exact nodes when it is complete;
    /// these regions bound that index, and are also the conservative answer when it is not.
    pub(super) fn regions_reachable_for_named_consumers(&self, scopes: &[TreeScopeID]) -> Option<Vec<ImpactRegion>> {
        let mut regions = self.regions_reachable_from_scopes(scopes, false, false)?;
        let outside_regions = self.regions_reachable_from_scopes(scopes, true, true)?;
        regions.extend(outside_regions);
        Some(regions)
    }

    /// Where an entry with no dispatch key can have subjects, when its own shape still says.
    ///
    /// Three things narrow an entry the dispatch buckets gave up on. `:root` matches the document's
    /// root element and nothing else, whatever else its compound tests. A child combinator names
    /// what the subject's parent must be, which does have postings to enumerate - one lookup per
    /// parent candidate reaches every subject the entry can have, where the alternative is every
    /// element in the document. And a descendant combinator says the same thing more weakly: the
    /// subject is somewhere under a named ancestor rather than directly beneath a named parent.
    ///
    /// A relative query the subject has to satisfy is the fourth: `:has(.error)` names no feature of
    /// its own, but it can only match an anchor of that query, and the witness compound does have a
    /// posting to enumerate.
    ///
    /// `None` means the entry says nothing, and the caller falls back to the scope. Every answer here
    /// is a superset of the true subject set, which is what a plan is allowed to be.
    pub(super) fn regions_from_subject_position(
        &self,
        compiled: &SelectorProgram,
        entry: usize,
        document_root: StyleNodeID,
        bounding_scopes: Option<&[TreeScopeID]>,
    ) -> Option<Vec<ImpactRegion>> {
        if compiled.subject_is_only_the_root(entry) {
            return Some(vec![ImpactRegion::Node(document_root)]);
        }

        // The sets below are disjunctions: the constrained relative is reachable by at least one of
        // the keys, so the union over all of them covers it. An empty set is no constraint at all.
        //
        // A parent is the tightest, so it is asked for first; an ancestor is consulted when there is
        // no child combinator, and a relative query only when the subject's own position says nothing.
        let (keys, region): (_, fn(StyleNodeID) -> ImpactRegion) = match compiled.subject_parent_dispatch(entry) {
            keys if !keys.is_empty() => (keys, ImpactRegion::Children),
            _ => match compiled.subject_ancestor_dispatch(entry) {
                keys if !keys.is_empty() => (keys, ImpactRegion::StrictSubtree),
                _ => match compiled.subject_relative_anchor(entry) {
                    Some((axis, keys)) if !keys.is_empty() => (keys, anchor_region_for(axis)?),
                    _ => return None,
                },
            },
        };
        if keys.is_empty() {
            return None;
        }
        let mut regions = Vec::new();
        for key in keys {
            if !key.has_selector_posting() {
                return None;
            }
            match self.facts.postings().lookup(key) {
                Lookup::Known(posting) => {
                    for relative in posting.candidates() {
                        if bounding_scopes
                            .is_some_and(|scopes| scopes.binary_search(&self.tree.tree_scope(relative)).is_err())
                        {
                            continue;
                        }
                        regions.push(region(relative));
                    }
                }
                Lookup::KnownAbsent => {}
                Lookup::Missing(_) => return None,
            }
        }
        Some(regions)
    }

    /// Record that a rule sits in a cascade layer.
    ///
    /// Said as the rule is compiled rather than as an input: which layer a rule is in is part of what
    /// the rule is, and a rule that moves between layers is recompiled.
    /// Record which longhand properties one of an element's own declarations covers.
    ///
    /// An element-attached declaration is a cascade component above layers: a style attribute beats
    /// every layered and unlayered rule in its context, whatever layer they are in.
    pub fn set_element_declared_properties(
        &mut self,
        node: StyleNodeID,
        kind: ElementDeclarationKind,
        declarations: Vec<(DeclaredProperty, RetainedStyleValueData)>,
        custom_declarations: Vec<(CustomDeclaration, RetainedStyleValueData)>,
    ) {
        debug_assert!(custom_declarations.is_empty() || kind == ElementDeclarationKind::InlineStyle);
        if matches!(
            kind,
            ElementDeclarationKind::PresentationalHint | ElementDeclarationKind::SvgPresentationAttribute
        ) {
            verify_cascade_winners(self, |_| {
                let mut properties: Vec<u16> = declarations.iter().map(|(declared, _)| declared.property).collect();
                properties.sort_unstable();
                assert!(
                    properties.windows(2).all(|pair| pair[0] != pair[1]),
                    "element-attached declarations repeat a property"
                );
            });
        }
        let current_declared = self.facts.element_declared_properties(node, kind);
        let current_custom_declarations = match kind {
            ElementDeclarationKind::InlineStyle => self.facts.element_custom_declarations(node),
            _ => &[],
        };
        if current_declared
            .iter()
            .eq(declarations.iter().map(|(declared, _)| declared))
            && current_custom_declarations
                .iter()
                .eq(custom_declarations.iter().map(|(declared, _)| declared))
        {
            return;
        }
        // Custom properties never reach the winner columns, so winners repaired from the
        // declarations alone hold only for an element whose style declared none.
        let repair_inputs = current_custom_declarations
            .is_empty()
            .then(|| {
                let previous = self
                    .current_winner_groups()
                    .token_for(WinnerGroupKey::current(node, self.program.version()))
                    .sparse()
                    .ok()
                    .map(|(_, state)| state)?;
                let retained = self.current_answer_identity(node)?;
                self.match_answers.answer(retained)?;
                Some((previous, retained, current_declared.to_vec()))
            })
            .flatten();
        self.facts.set_element_declared_properties(node, kind, declarations);
        if kind == ElementDeclarationKind::InlineStyle {
            self.facts.set_element_custom_declarations(node, custom_declarations);
        }
        let Some((previous, retained, previous_declared)) = repair_inputs else {
            return;
        };
        let declared = self.facts.element_declared_properties(node, kind);
        let mut changed_properties: Vec<u16> = previous_declared
            .iter()
            .chain(declared)
            .map(|declared| declared.property)
            .collect();
        changed_properties.sort_unstable();
        changed_properties.dedup();
        changed_properties.retain(|&property| {
            previous_declared.iter().find(|declared| declared.property == property)
                != declared.iter().find(|declared| declared.property == property)
        });
        if !changed_properties.is_empty() {
            self.apply_element_declaration_winner_updates(node, previous, retained, &changed_properties);
        }
    }

    /// Repair the exact properties whose element-attached declaration inventory changed.
    ///
    /// Presentational hints are published while the legacy cascade builds its input, after the
    /// transaction has already reused the selector answer. Re-reducing only their changed
    /// properties here keeps the retained top-1 relation current without matching the element.
    pub(super) fn apply_element_declaration_winner_updates(
        &mut self,
        node: StyleNodeID,
        previous: CascadeStateID,
        retained: MatchAnswerID,
        properties: &[u16],
    ) {
        let Some(retained) = self.match_answers.answer(retained) else {
            return;
        };
        let matches = retained
            .iter()
            .copied()
            .filter(|entry| {
                !self.program.declarations_are_complete_for(entry.rule)
                    || self
                        .program
                        .declared_properties_of(entry.rule)
                        .iter()
                        .any(|declared| properties.binary_search(&declared.property).is_ok())
            })
            .map(|entry| entry.materialize(node, &self.programs, 0))
            .collect::<Option<Vec<_>>>();
        let Some(matches) = matches else {
            return;
        };
        self.counters
            .add(Counter::ElementDeclarationRepairMatches, matches.len() as u64);
        let Some(updates) = self.exact_cascade_winner_updates_for_properties(node, &matches, None, properties) else {
            return;
        };
        let (state, _) =
            self.with_cascade_interning_counters(|groups| groups.apply_property_updates(previous, &updates));
        let published = if let Some(traversal) = self.batch_matching_traversal.as_mut() {
            traversal.answer_effects.winners.set(
                &mut self.winner_groups,
                node,
                state,
                self.program.version(),
                &mut self.memory,
            )
        } else {
            let mut effects = super::cascade::WinnerEffects::default();
            let published = effects.set(
                &mut self.winner_groups,
                node,
                state,
                self.program.version(),
                &mut self.memory,
            );
            self.install_winner_effects(effects);
            published
        };
        self.winner_groups.settle_memory(&mut self.memory);
        if published {
            self.counters.bump(Counter::CascadeNodeHandlesPublished);
        }
    }
}

impl StyleEngine {
    #[must_use]
    pub(crate) fn new() -> Self {
        let mut memory = MemoryController::new();
        let tree = StyleNodeTree::new(&mut memory);
        Self {
            retained: RetainedState {
                counters: Counters::new(),
                memory,
                admission: AdmissionFacts::default(),
                deferred_pseudo_elements: 0,
                tree: crate::fork::ForkShared::new(tree),
                program: crate::fork::ForkShared::new(StyleSheetProgram::new()),
                native_rules: Default::default(),
                declaration_block_version: Arc::new(std::sync::atomic::AtomicU32::new(1)),
                last_transaction_only_derived_child_reactions: false,
                sheets_excluded_from_routing: BitColumn::default(),
                routing_needs_detachment_sweep: false,
                match_workspace: MatchScratch::default(),
                exact_covered_scratch: Vec::new(),
                cascade_compaction_scratch: ordering::CascadeCompactionWorkspace::default(),
                cascade_compaction_scratch_memory: MemoryLease::new(MemoryCategory::BatchScratch),
                next_style_transaction_version: StyleTransactionVersion(1),
                document_style_computation_inputs: Default::default(),
                driven_viewport: (0.0, 0.0),
                document_resource_contexts: Default::default(),
                document_media: Default::default(),
                document_functions: Default::default(),
                custom_property_registry: None,
                monospace_font_family: RetainedStyleValueData::from_owned(
                    crate::css::parser::value_parser::value_list(
                        vec![StyleValueData::Keyword {
                            keyword: crate::css::style_compute::keyword::MONOSPACE,
                        }],
                        1,
                        true,
                    ),
                ),
                font_resolution: None,
                layer_topology_version: 0,
                sheet_order_version: 0,
                specified_values: crate::fork::ForkShared::new(SpecifiedValues::new()),
                winner_groups: crate::fork::ForkShared::new(WinnerGroups::new()),
                computed_group_sets: Default::default(),
                custom_property_environments: Default::default(),
                nodes_with_substituted_records: HashSet::default(),
                custom_declaration_reads: HashMap::default(),
                nodes_with_tree_counting_records: HashMap::default(),
                nodes_with_rolled_back_records: HashMap::default(),
                nodes_with_element_relative_substitutions: HashMap::default(),
                element_custom_property_data: Default::default(),
                pseudo_element_custom_property_data: Default::default(),
                environment_move_recompute_nodes: HashSet::default(),
                container_effects_for_host: Default::default(),
                published_container_verdicts: HashMap::default(),
                container_gates_unheld: HashSet::default(),
                row_inputs_moved: flush::RowInputsMoved::default(),
                container_query_inputs: Default::default(),
                layout_style_snapshots: HashMap::default(),
                size_container_queries: Default::default(),
                counter_style_environment_identities: HashMap::default(),
                held_style_records: Default::default(),
                tick_shown: Default::default(),
                backing_elements: HashMap::default(),
                children_explicitly_inherit_marks: HashSet::default(),
                css_defined_animations: Default::default(),
                animation_keyframes: Default::default(),
                animation_effect_descriptions: Default::default(),
                held_root_font_inputs: None,
                random_base_values: Default::default(),
                replaced_content_inputs: HashMap::default(),
                transition_baselines: Default::default(),
                custom_property_registrations_changed: false,
                engine_computed_records_pending: HashMap::default(),
                demand_records: HashMap::default(),
                flush_stamp: 0,
                engine_pseudo_record_cache: HashMap::default(),
                batch_answers_complete_but_for_custom_properties: HashMap::default(),
                batch_custom_property_matches: HashMap::default(),
                batch_backing_pseudo_matches: HashMap::default(),
                engine_cold_record_cache: Default::default(),
                engine_cold_record_donors: Default::default(),
                computed_group_set_memory: MemoryLease::new(MemoryCategory::ComputedGroupSet),
                custom_property_environment_memory: MemoryLease::new(MemoryCategory::CustomPropertyEnvironment),
                computed_fixed_metadata_memory: MemoryLease::new(MemoryCategory::ComputedFixedMetadata),
                computed_longhand_table_memory: MemoryLease::new(MemoryCategory::ComputedLonghandTable),
                style_record_memory: MemoryLease::new(MemoryCategory::StyleRecord),
                animation_overlay_memory: MemoryLease::new(MemoryCategory::AnimationOverlayRecord),
                computed_pseudo_assignment_memory: MemoryLease::new(MemoryCategory::ComputedPseudoAssignment),
                style_invalidation_cache: HashMap::default(),
                html_element_namespace: StyleAtomID::NONE,
                match_answers: Default::default(),
                selector_truth_sets: SelectorTruthSetCatalog::default(),
                retained_match_answers: Default::default(),
                retained_selector_incidences: RetainedSelectorIncidences::default(),
                selector_incidence_is_current: false,
                batch_matching_traversal: None,
                completion_exactness: CompletionExactness::Exact,
                route_pruning_states: crate::fork::ForkReset::new(Mutex::new(RoutePruningStateCache::default())),
                prefix_caches: std::sync::Arc::default(),
                #[cfg(test)]
                force_bounded_prefix_completion: false,
                prepared_batch_matching_traversal: None,
                published_match_answers: PublishedMatchAnswers::default(),
                transaction_fact_view: None,
                facts: crate::fork::ForkShared::new(ElementFactStore::new()),
                programs: SelectorPrograms::for_live_engine(),
                attribute_value_text_names: Arc::default(),
                attribute_value_text_requirements_version: 0,
                selector_programs_need_sweep: false,
                routing: Arc::new(RoutingRegistry::new()),
                selector_truth_changes: SelectorTruthChanges::default(),
                already_planned_selector_truth: DeltaBatch::default(),
                selector_truth_changes_active: false,
                relational_witnesses: RelationalWitnesses::default(),
                pending_witness_effects: Vec::new(),
                witness_effect_scratch: MemoryLease::new(MemoryCategory::BatchScratch),
                relational_witness_residency: MemoryLease::new(MemoryCategory::RetainedWitness),
                scope_roots: Column::default(),
                scope_by_root: Default::default(),
                scope_programs: intern_table::InternTable::default(),
                vacant_scope_programs: Vec::new(),
                scope_dispatch_templates: HashMap::default(),
                scope_cascade_templates: HashMap::default(),
                ancestor_dispatch_templates: Default::default(),
                scope_program_by_scope: Column::default(),
                atoms: crate::fork::ForkShared::new(DocumentAtoms::for_live_engine()),
                fold_id_and_class_name_case: false,
                #[cfg(test)]
                diagnostic_plan_capture: None,
            },
            host: HostState {
                suspended_style_pass: None,
                font_resolver: None,
                journal: NormalizationJournal::new(),
                deferred_geometry_journal: NormalizationJournal::new(),
                flushing_deferred_geometry_journal: false,
                deferred_element_style_inputs: Vec::new(),
                deferred_element_style_inputs_moved: false,
                deferred_element_style_inputs_are_pending: false,
                deferred_element_style_input_memory: MemoryLease::new(MemoryCategory::NormalizationJournal),
                initial_tree_batch_applied: false,
                initial_tree_bulk_load_is_pending: false,
                tree_staging: TreeRelationStaging::default(),
                tree_staging_memory: MemoryLease::new(MemoryCategory::NormalizationJournal),
                program_staging: ProgramStaging::default(),
                sheet_occurrences: HashMap::default(),
                sheet_occurrence_storage_bytes: 0,
                sheet_occurrence_memory: MemoryLease::new(MemoryCategory::RuleProgram),
                sheet_rule_replacement: None,
                reclaimed_style_atoms: Vec::new(),
                defers_atom_sweep: false,
            },
        }
    }

    /// Record a change to style inputs which are properties of the document environment rather
    /// than of an element or stylesheet rule.
    pub fn record_environment_change(&mut self) {
        self.discard_prepared_batch_matching_traversal();
        self.host.journal.record_complete_scope_action(
            InputKind::Environment,
            &mut self.retained.memory,
            &self.retained.counters,
        );
    }

    /// Whether the first tree batch can be installed directly.
    pub(crate) fn can_bulk_load_initial_tree(&self) -> bool {
        !self.host.initial_tree_batch_applied
    }

    /// Install a first batch of unique element arrivals without journalling one structural input
    /// per row. The document root is the transaction envelope, so planning will still choose the
    /// same exact whole-document result at first observation.
    pub(crate) fn bulk_load_initial_tree(
        &mut self,
        document_root: StyleNodeID,
        arrivals: &[(StyleNodeID, TreeRelations)],
    ) {
        debug_assert!(!self.host.initial_tree_batch_applied);
        debug_assert!(!arrivals.is_empty());
        debug_assert!(arrivals.iter().any(|&(node, _)| node == document_root));

        for &(node, relations) in arrivals {
            self.link(node, relations);
        }
        self.publish_budget_inputs();

        let root_relations = arrivals
            .iter()
            .find_map(|&(node, relations)| (node == document_root).then_some(relations))
            .unwrap();
        let recorded = self.record(
            InputKey::TreeRelations(document_root),
            InputValue::TreeRelations(None),
            InputValue::TreeRelations(Some(root_relations)),
        );
        debug_assert!(recorded);

        let folded_rows = arrivals.len() - 1;
        self.retained
            .counters
            .add(Counter::RawMutationRecords, folded_rows as u64);
        self.retained.counters.add(Counter::TreeDeltas, folded_rows as u64);
        self.retained.counters.bump(Counter::InitialBulkLoads);
        self.retained
            .counters
            .add(Counter::InitialBulkTreeRows, arrivals.len() as u64);
        self.host.initial_tree_batch_applied = true;
        self.host.initial_tree_bulk_load_is_pending = true;
    }

    #[must_use]
    pub fn has_pending_transaction(&self) -> bool {
        (self.host.deferred_element_style_inputs_are_pending && !self.host.deferred_element_style_inputs.is_empty())
            || !self.host.journal.is_empty()
            || (!self.host.flushing_deferred_geometry_journal && !self.host.deferred_geometry_journal.is_empty())
            || !self.host.tree_staging.is_empty()
            || self.host.program_staging.is_dirty()
            || self.host.sheet_rule_replacement.is_some()
            || self.host.suspended_style_pass.is_some()
    }

    /// Whether the host is installing a style pass wave by wave, between two of its waves.
    #[must_use]
    pub fn has_suspended_style_pass(&self) -> bool {
        self.host.suspended_style_pass.is_some()
    }

    #[must_use]
    pub fn has_deferred_geometry_transaction(&self) -> bool {
        !self.host.flushing_deferred_geometry_journal && !self.host.deferred_geometry_journal.is_empty()
    }

    /// Whether any element style input is still deferred, waiting for the first transaction with a
    /// document root. A rootless flush drains the journal but preserves these — so an engine that
    /// reports no pending transaction can still owe an element its recomputation.
    #[must_use]
    pub fn has_deferred_element_style_inputs(&self) -> bool {
        !self.host.deferred_element_style_inputs.is_empty()
    }

    /// Record that what a container query or a container-relative length read of the node's
    /// containers moved: a container's size, scroll state or style. The engine settles it where it
    /// can, deciding the node's gated rules again and publishing its winners anew from its
    /// retained answer where a verdict moved.
    pub fn record_container_query_input(&mut self, node: StyleNodeID) {
        self.retained.row_inputs_moved.note_containers_moved(node);
        self.record_derived_element_style_input(
            node,
            transaction::STYLE_REACTION_PUBLISHED_STYLE | transaction::STYLE_REACTION_RECOMPUTE_STYLE,
            0,
        );
    }

    /// Record a font input change for every element whose style, or the style of one of its
    /// pseudo-elements, uses a font that resolves differently now: its `font-family` names one of
    /// the families, packed as one buffer of code units with a length each, or its cascade is one of
    /// `font_lists`, which are compared by address only. Styles share font groups, so each distinct
    /// group is asked once.
    pub fn record_font_input_changes(
        &mut self,
        family_name_lengths: &[u32],
        family_name_units: &[u16],
        font_lists: &[u64],
    ) {
        use crate::css::computed_value_types::{FontValues, STYLE_GROUP_INDEX_FONT};
        use crate::css::custom_properties::Utf16SliceExt;
        use crate::css::style_value::StyleValueData;

        let families = || {
            family_name_lengths.iter().scan(0, |offset, &length| {
                let start = *offset;
                *offset += length as usize;
                Some(&family_name_units[start..*offset])
            })
        };
        let names_changed_family = |font: &FontValues| {
            let Some(StyleValueData::ValueList { values, .. }) = font.font_family.data() else {
                return false;
            };
            values.as_slice().iter().any(|value| {
                let name = match value.data() {
                    StyleValueData::String { string, .. } => string.units(),
                    StyleValueData::CustomIdent { custom_ident } => custom_ident.units(),
                    _ => return false,
                };
                families().any(|family| name.eq_ignore_ascii_case_utf16(family))
            })
        };
        let mut answers = HashMap::<*const FontValues, bool>::default();
        let mut uses_changed_font = |style_record: u64| {
            let Some(payloads) = self.computed_group_sets.style_record_payloads(style_record) else {
                return false;
            };
            let font =
                crate::css::computed_value_views::ComputedValuesView::new(SharedPayload::as_pointer_slice(payloads))
                    .font();
            *answers.entry(std::ptr::from_ref(font)).or_insert_with(|| {
                font_lists.contains(&(font.font_cascade_list.as_raw() as u64)) || names_changed_family(font)
            })
        };
        let users: Vec<StyleNodeID> = self
            .held_style_records
            .iter()
            .map(|(&node, &style_record)| (node, style_record))
            .chain(
                self.computed_group_sets
                    .pseudo_style_records()
                    .map(|(node, style_record)| (node, style_record.raw())),
            )
            .filter_map(|(node, style_record)| uses_changed_font(style_record).then_some(node))
            .collect();
        // An element and its pseudo-elements may both be users: what it owes merges.
        for node in users {
            self.record_derived_element_style_input(
                node,
                transaction::STYLE_REACTION_PUBLISHED_STYLE
                    | transaction::STYLE_REACTION_RECOMPUTE_STYLE
                    | transaction::STYLE_REACTION_FONT_INPUTS_CHANGED,
                1 << STYLE_GROUP_INDEX_FONT,
            );
        }
    }

    /// Record a style reaction for one element, which the engine derived itself or C++ derived
    /// from what it saw move, merged with what the element already owes. It joins the next
    /// transaction, and the engine settles it where it can.
    pub fn record_derived_element_style_input(&mut self, node: StyleNodeID, reaction: u8, inherited_style_groups: u8) {
        if reaction == 0 {
            return;
        }
        self.defer_element_style_input(node, reaction, inherited_style_groups);
        self.host.deferred_element_style_inputs_are_pending = true;
    }

    /// Fold the style input an element owes into the reaction C++ is about to apply to it, when
    /// that reaction covers it: a materialization covers anything, while a record delta covers
    /// what it already carries and a descendant recompute. The folded input is consumed; one not
    /// covered stays owed to the next transaction. Returns the merged reaction in the low byte and
    /// the merged inherited style groups in the next, or zero when nothing was folded.
    pub fn absorb_element_style_input(
        &mut self,
        node: StyleNodeID,
        reaction: u8,
        inherited_style_groups: u8,
        absorbs_any: bool,
    ) -> u32 {
        let Ok(index) = self
            .host
            .deferred_element_style_inputs
            .binary_search_by_key(&InputKey::ElementStyleInput(node), |pending| pending.key)
        else {
            return 0;
        };
        let InputValue::ElementStyleInput {
            reaction: pending_reaction,
            inherited_style_groups: pending_inherited_style_groups,
        } = self.host.deferred_element_style_inputs[index].new
        else {
            unreachable!();
        };
        // A descendant recompute asks nothing of the element its own reaction does not answer, and
        // the applied reaction carries what it asks of the descendants to the element's children.
        if !absorbs_any
            && (pending_reaction & !reaction & !transaction::STYLE_REACTION_RECOMPUTE_DESCENDANT_STYLES != 0
                || pending_inherited_style_groups & !inherited_style_groups != 0)
        {
            return 0;
        }
        self.host.deferred_element_style_inputs.remove(index);
        self.host.deferred_element_style_inputs_moved = true;
        u32::from(reaction | pending_reaction)
            | (u32::from(inherited_style_groups | pending_inherited_style_groups) << 8)
    }

    /// The element style inputs the engine defers, by element, where they moved since the last time they were taken.
    pub(crate) fn take_moved_deferred_element_style_inputs(
        &mut self,
    ) -> Option<Vec<crate::css::style::engine_calls::DeferredInput>> {
        if !std::mem::take(&mut self.host.deferred_element_style_inputs_moved) {
            return None;
        }
        Some(
            self.host
                .deferred_element_style_inputs
                .iter()
                .filter_map(|input| match (input.key, input.new) {
                    (
                        InputKey::ElementStyleInput(node),
                        InputValue::ElementStyleInput {
                            reaction,
                            inherited_style_groups,
                        },
                    ) => Some((node, reaction, inherited_style_groups)),
                    _ => None,
                })
                .collect(),
        )
    }

    /// Drop the style input an element owes: C++ computed the element's style, which answers it.
    pub fn consume_element_style_input(&mut self, node: StyleNodeID) {
        if let Ok(index) = self
            .host
            .deferred_element_style_inputs
            .binary_search_by_key(&InputKey::ElementStyleInput(node), |pending| pending.key)
        {
            self.host.deferred_element_style_inputs.remove(index);
            self.host.deferred_element_style_inputs_moved = true;
        }
    }

    /// Drop the style input an element owes but the descendant recompute it carries: a demand
    /// answers the element's own style, which reaches none of its descendants.
    pub(super) fn consume_element_style_input_but_descendants(&mut self, node: StyleNodeID) {
        let Ok(index) = self
            .host
            .deferred_element_style_inputs
            .binary_search_by_key(&InputKey::ElementStyleInput(node), |pending| pending.key)
        else {
            return;
        };
        self.host.deferred_element_style_inputs_moved = true;
        let InputValue::ElementStyleInput {
            reaction,
            inherited_style_groups,
        } = &mut self.host.deferred_element_style_inputs[index].new
        else {
            unreachable!();
        };
        if *reaction & transaction::STYLE_REACTION_RECOMPUTE_DESCENDANT_STYLES != 0 {
            *reaction = transaction::STYLE_REACTION_RECOMPUTE_DESCENDANT_STYLES;
            *inherited_style_groups = 0;
        } else {
            self.host.deferred_element_style_inputs.remove(index);
        }
    }

    /// Whether one element still owes a deferred style input, asked per node the way the recorded
    /// batch is.
    #[must_use]
    pub fn has_deferred_element_style_input(&self, node: StyleNodeID) -> bool {
        self.host.owes_element_style_input(node)
    }

    /// Whether settling the pending selector inputs can change geometry derived from the committed
    /// layout. This is deliberately a proof of independence rather than a list of properties which
    /// usually avoid layout: anything not explicitly known to preserve geometry remains observable.
    #[must_use]
    pub fn pending_transaction_may_affect_layout_geometry(&self) -> bool {
        if self.host.journal.is_empty() {
            return !self.host.tree_staging.is_empty()
                || self.host.program_staging.is_dirty()
                || self.host.sheet_rule_replacement.is_some()
                || !self.host.deferred_element_style_inputs.is_empty()
                || self.host.initial_tree_bulk_load_is_pending;
        }
        if !self.host.journal.markers().is_empty()
            || !self.host.tree_staging.is_empty()
            || self.host.program_staging.is_dirty()
            || self.host.sheet_rule_replacement.is_some()
            || !self.host.deferred_element_style_inputs.is_empty()
            || self.host.initial_tree_bulk_load_is_pending
        {
            return true;
        }

        let mut checked_keys = HashSet::default();
        self.host.journal.inputs().any(|input| {
            let keys = match input.key {
                InputKey::LocalFeature(_, LocalFeatureKey::PartExposure | LocalFeatureKey::ArrivingFacts) => {
                    return true;
                }
                InputKey::LocalFeature(_, LocalFeatureKey::Attribute(name)) => {
                    let mut keys = routing_keys_for_input(&input);
                    for other in self.retained.facts.attribute_name_keys(name) {
                        if other != name {
                            keys.push(RoutingKey::AttributeName(other));
                        }
                    }
                    keys
                }
                InputKey::LocalFeature(..) | InputKey::State(..) => routing_keys_for_input(&input),
                _ => return true,
            };
            keys.into_iter().any(|key| {
                // Geometry independence depends on the routing key's rules, not the node.
                // Reuse that proof when several journal inputs reach the same key.
                checked_keys.insert(key)
                    && self.retained.routing.key_may_affect_layout_geometry(
                        key,
                        &self.retained.program,
                        &self.retained.programs,
                    )
            })
        })
    }

    /// Preserve the pending paint-only selector facts as the style change event established by a
    /// geometry read. Repeated reads advance the same boundary to the latest observed facts.
    /// Returning false means exact journalling coarsened while combining the facts, so the caller
    /// must settle style instead of reusing layout.
    pub fn defer_pending_transaction_for_geometry_read(&mut self) -> bool {
        debug_assert!(!self.host.flushing_deferred_geometry_journal);
        debug_assert!(self.host.tree_staging.is_empty());
        debug_assert!(!self.host.program_staging.is_dirty());
        debug_assert!(self.host.sheet_rule_replacement.is_none());
        debug_assert!(self.host.deferred_element_style_inputs.is_empty());
        debug_assert!(!self.host.initial_tree_bulk_load_is_pending);
        debug_assert!(self.host.journal.markers().is_empty());
        debug_assert!(
            self.host
                .journal
                .inputs()
                .all(|input| matches!(input.key, InputKey::LocalFeature(..) | InputKey::State(..)))
        );

        if self.host.journal.is_empty() {
            return true;
        }
        if self.host.deferred_geometry_journal.is_empty() {
            std::mem::swap(&mut self.host.journal, &mut self.host.deferred_geometry_journal);
        } else {
            self.host.deferred_geometry_journal.absorb_newer(
                &mut self.host.journal,
                &mut self.retained.memory,
                &self.retained.counters,
            );
        }
        self.host.deferred_geometry_journal.markers().is_empty()
    }

    /// Make the style transaction sealed by a geometry read current while preserving local facts
    /// recorded after it for the following style change event.
    pub fn begin_deferred_geometry_transaction_flush(&mut self) -> bool {
        debug_assert!(!self.host.flushing_deferred_geometry_journal);
        if self.host.deferred_geometry_journal.is_empty()
            || !self.host.journal.markers().is_empty()
            || !self.host.tree_staging.is_empty()
            || self.host.program_staging.is_dirty()
            || self.host.sheet_rule_replacement.is_some()
            || !self.host.deferred_element_style_inputs.is_empty()
            || self.host.initial_tree_bulk_load_is_pending
            || !self
                .host
                .journal
                .inputs()
                .all(|input| matches!(input.key, InputKey::LocalFeature(..) | InputKey::State(..)))
        {
            return false;
        }

        let later_inputs: Vec<NormalizedInput> = self.host.journal.inputs().collect();
        for input in &later_inputs {
            self.apply_to_facts_without_settling(input.key, input.old);
        }
        std::mem::swap(&mut self.host.journal, &mut self.host.deferred_geometry_journal);
        self.host.flushing_deferred_geometry_journal = true;
        true
    }

    /// Restore the local facts recorded after the geometry boundary once its transaction has been
    /// consumed.
    pub fn end_deferred_geometry_transaction_flush(&mut self) {
        assert!(self.host.flushing_deferred_geometry_journal);
        assert!(self.host.journal.is_empty());
        assert!(self.host.tree_staging.is_empty());
        assert!(!self.host.program_staging.is_dirty());
        assert!(self.host.sheet_rule_replacement.is_none());
        assert!(self.host.deferred_element_style_inputs.is_empty());

        std::mem::swap(&mut self.host.journal, &mut self.host.deferred_geometry_journal);
        let later_inputs: Vec<NormalizedInput> = self.host.journal.inputs().collect();
        for input in later_inputs {
            self.apply_to_facts_without_settling(input.key, input.new);
        }
        self.host.flushing_deferred_geometry_journal = false;
    }

    pub(super) fn merge_deferred_geometry_transaction(&mut self) {
        if self.host.flushing_deferred_geometry_journal || self.host.deferred_geometry_journal.is_empty() {
            return;
        }
        if self.host.journal.is_empty() {
            std::mem::swap(&mut self.host.journal, &mut self.host.deferred_geometry_journal);
            return;
        }
        self.host.deferred_geometry_journal.absorb_newer(
            &mut self.host.journal,
            &mut self.retained.memory,
            &self.retained.counters,
        );
        std::mem::swap(&mut self.host.journal, &mut self.host.deferred_geometry_journal);
    }

    pub(crate) fn settle_batched_inputs(&mut self) {
        self.install_pending_matching_context();
        if !self.host.journal.contains_only_element_style_inputs() {
            self.discard_prepared_batch_matching_traversal();
        }
        self.discard_published_match_answers();
    }

    #[must_use]
    pub(crate) fn node_arrival_is_pending(&self, node: StyleNodeID) -> bool {
        self.host.journal.pending_old(InputKey::TreeRelations(node)) == Some(InputValue::TreeRelations(None))
    }

    /// Returns whether the change joined the current transaction.
    pub(super) fn record(&mut self, key: InputKey, old: InputValue, new: InputValue) -> bool {
        self.discard_prepared_batch_matching_traversal();
        if let Some(node) = self.node_whose_arrival_carries(key) {
            // Every fact of an arriving element folds onto one key, so the journal holds one entry
            // per element rather than one per fact. Routing reads the facts back off the element.
            self.retained.counters.bump(Counter::ArrivingNodeFactsFolded);
            self.host.journal.record(
                InputKey::LocalFeature(node, LocalFeatureKey::ArrivingFacts),
                InputValue::Feature(FeatureValue::Absent),
                InputValue::Feature(FeatureValue::Present),
                &mut self.retained.memory,
                &self.retained.counters,
            );
            return true;
        }
        self.host
            .journal
            .record(key, old, new, &mut self.retained.memory, &self.retained.counters);
        true
    }

    /// Whether a fact about an element is already carried by that element arriving.
    ///
    /// An element that connects in this transaction has its whole subtree put in the plan by its own
    /// tree delta, and the elements around it - the anchors of a relational query, the neighbours a
    /// sibling selector constrains, the parent whose emptiness moved - are reached from that delta
    /// too, not from anything the arriving element publishes about itself. So each of the facts it
    /// announces on the way in says nothing the arrival does not already say.
    ///
    /// What journalling them costs is the transaction's scratch budget: a page of a few thousand
    /// elements announces tens of thousands of them, and a journal that has to coarsen no longer
    /// knows which keys moved - which costs a restyle of the whole document, for elements whose
    /// arrival was already accounted for.
    ///
    /// The facts themselves are still applied: what is skipped is the journal entry, not the state.
    #[must_use]
    pub(super) fn node_whose_arrival_carries(&self, key: InputKey) -> Option<StyleNodeID> {
        let node = match key {
            InputKey::LocalFeature(node, feature) if feature != LocalFeatureKey::ArrivingFacts => node,
            InputKey::State(node, _) => node,
            _ => return None,
        };
        (self.host.journal.pending_old(InputKey::TreeRelations(node)) == Some(InputValue::TreeRelations(None)))
            .then_some(node)
    }

    #[inline]
    pub(super) fn stage_tree_row(
        &mut self,
        node: StyleNodeID,
        old_if_unstaged: Option<TreeRelations>,
        new: Option<TreeRelations>,
    ) {
        let old = self.host.tree_staging.current_row(node, old_if_unstaged);
        if old == new {
            return;
        }
        self.record(
            InputKey::TreeRelations(node),
            InputValue::TreeRelations(old),
            InputValue::TreeRelations(new),
        );
        if old.is_some() && new.is_none() {
            self.retained.counters.bump(Counter::TreeDepartureDeltas);
        }
        self.host.tree_staging.stage_row(node, old_if_unstaged, new);
    }

    pub(super) fn stage_connected_tree_row(&mut self, node: StyleNodeID, update: impl FnOnce(&mut TreeRelations)) {
        let old = self
            .host
            .tree_staging
            .current_row(node, Some(self.settled_tree_relations(node)));
        let mut new = old.expect("a pending neighbour must remain connected");
        // A subtree can preallocate sibling identities before publishing their insertions. Such
        // a neighbour has no parent yet; its own insertion will supply its links. Synthesizing a
        // connected before-row here would hide that arrival from transaction normalization.
        if new.parent.is_none() {
            return;
        }
        update(&mut new);
        self.stage_tree_row(node, old, Some(new));
    }

    pub(super) fn stage_first_child(&mut self, parent: StyleNodeID, child: Option<StyleNodeID>) {
        self.host
            .tree_staging
            .stage_first_child(parent, self.retained.tree.first_element_child(parent), child);
    }

    pub(super) fn settle_tree_staging_memory(&mut self) {
        let bytes = self.host.tree_staging.capacity_bytes();
        self.host
            .tree_staging_memory
            .resize_required_to(&mut self.retained.memory, bytes);
    }

    /// Install final staged relation rows at the transaction barrier.
    pub(super) fn apply_staged_tree_deltas(&mut self) {
        if self.host.tree_staging.is_empty() || self.host.tree_staging.is_applied() {
            return;
        }
        let staged_rows = self.host.tree_staging.dirty_rows();
        // Depth changes only for arrivals and for nodes whose parent differs from the resident one,
        // read before installation: the frozen before-side parent misses a move that a mid-transaction
        // application already installed, and a sibling-only row must not count as a moved parent.
        // A moved parent's subtree walk covers its moved descendants, so those are skipped below.
        let depth_recompute_nodes = self.depth_recompute_nodes(&staged_rows);

        for &(node, _, relations) in &staged_rows {
            let Some(relations) = relations else {
                self.retained.tree.set_parent_without_updating_depth(node, None);
                self.retained.tree.set_next_element_sibling(node, None);
                self.retained.tree.set_previous_element_sibling(node, None);
                self.retained
                    .tree
                    .set_assigned_slot(node, None, &mut self.retained.memory);
                continue;
            };
            self.retained
                .tree
                .set_parent_without_updating_depth(node, relations.parent);
            self.retained
                .tree
                .set_next_element_sibling(node, relations.next_element_sibling);
            self.retained
                .tree
                .set_previous_element_sibling(node, relations.previous_element_sibling);
            if relations.tree_scope != TreeScopeID::DOCUMENT {
                self.retained.tree.enable_tree_scopes(&mut self.retained.memory);
            }
            if self.retained.tree.has_tree_scopes() {
                self.retained.tree.set_tree_scope(node, relations.tree_scope);
            }
            self.retained
                .tree
                .set_assigned_slot(node, relations.assigned_slot, &mut self.retained.memory);
        }
        for (parent, _, child) in self.host.tree_staging.dirty_first_children() {
            self.retained.tree.set_first_element_child(parent, child);
        }
        for &(node, _, _) in &staged_rows {
            if !depth_recompute_nodes.contains(&node) {
                continue;
            }
            let parent_is_recomputed = self
                .tree
                .parent(node)
                .is_some_and(|parent| depth_recompute_nodes.contains(&parent));
            if !parent_is_recomputed {
                self.retained.tree.recompute_subtree_depth(node);
            }
        }
        let live_animation_overlays_before = self.retained.computed_group_sets.live_animation_overlay_records();
        let mut retired_nodes: Vec<StyleNodeID> = Vec::new();
        for &(node, _, relations) in &staged_rows {
            if relations.is_some() || !self.retained.tree.is_live(node) {
                continue;
            }
            self.retained.retire_node_state(node);
            retired_nodes.push(node);
        }
        if !retired_nodes.is_empty() {
            self.retained
                .tree
                .retire_elements(&retired_nodes, &mut self.retained.memory);
            let live_animation_overlays_after = self.retained.computed_group_sets.live_animation_overlay_records();
            self.settle_computed_memory();
            self.retained.counters.add(
                Counter::AnimationOverlaySlotsReleased,
                (live_animation_overlays_before - live_animation_overlays_after) as u64,
            );
            self.retained.counters.set(
                Counter::LiveAnimationOverlayRecords,
                live_animation_overlays_after as u64,
            );
            self.retained
                .counters
                .add(Counter::StyleNodesRetired, retired_nodes.len() as u64);
        }
        self.host.tree_staging.mark_applied();
        self.publish_budget_inputs();
    }

    /// Name the node a style scope belongs to. A shadow root is a scope and a subtree at once, which
    /// is what lets a sheet attached there be bounded by the tree it decides in.
    /// Record that a tree scope decides with the document's author sheets as well as its own.
    pub fn set_tree_scope_uses_document_sheets(&mut self, tree_scope: TreeScopeID) {
        let previous = self
            .host
            .program_staging
            .scopes_using_document_sheets
            .current(tree_scope, || {
                self.retained.program.scope_uses_document_sheets(tree_scope)
            });
        if previous {
            return;
        }
        self.host.program_staging.scopes_using_document_sheets.stage(
            tree_scope,
            || self.retained.program.scope_uses_document_sheets(tree_scope),
            true,
        );
        self.host
            .program_staging
            .base_version
            .get_or_insert(self.retained.program.version());
        self.invalidate_scope_program(tree_scope);
    }
}

impl StyleEngine {
    /// Add a style rule that only applies inside a scope, naming the scope's root selectors.
    ///
    /// `before` places the rule immediately ahead of an existing one instead of at the end. A rule
    /// arriving in the middle of a sheet takes an order token between its neighbours: nothing else
    /// is renumbered, and no other rule's identity or compiled program is touched.
    pub fn add_style_rule_in_scope(
        &mut self,
        sheet: SheetID,
        before: Option<RuleID>,
        selectors: &[&CompiledSelector],
        namespaces: NamespaceScope,
        scope: &ScopeChain<'_>,
    ) -> RuleID {
        self.add_style_rule_with(sheet, before, |engine, previous_program| {
            engine.compile_selectors(selectors, namespaces, scope, previous_program)
        })
    }

    /// Add a style rule whose selector program `attach` gives it, from the program of the rule it replaces, if any.
    pub(super) fn add_style_rule_with(
        &mut self,
        sheet: SheetID,
        before: Option<RuleID>,
        attach: impl FnOnce(&mut Self, Option<SelectorProgramID>) -> SelectorProgramID,
    ) -> RuleID {
        let rule = match before {
            Some(before) => self.insert_rule_before(before, RuleKind::Style),
            None => self
                .reuse_replaced_style_rule(sheet)
                .unwrap_or_else(|| self.append_rule(sheet, None, RuleKind::Style)),
        };
        let previous_program = self
            .replacement_rule(rule)
            .and_then(|replacement| replacement.version.selector_program);
        let program = attach(self, previous_program);
        if previous_program != Some(program) {
            self.add_routing_rule(rule, program);
        }

        let mut version = self.current_rule_version(rule);
        version.selector_program = Some(program);
        self.replace_rule_version(rule, version);
        self.retained.counters.bump(Counter::StyleRulesCompiled);
        rule
    }

    /// Record that a sheet declared or gave up a cascade layer.
    ///
    /// The declaration contributes no declarations and matches nothing: what it does is fix the order
    /// of the layers every rule referencing them sits in. A layer name belongs to the tree scope the
    /// sheet is attached to, so what moves is that scope's layer order - one input per scope the sheet
    /// decides in.
    #[cfg(test)]
    pub fn record_layer_statement(&mut self, sheet: SheetID) {
        for scope in self.retained.program.sheet_scopes(sheet) {
            self.record_layer_topology_change(scope);
        }
    }

    pub(super) fn add_named_rule(
        &mut self,
        sheet: SheetID,
        before: Option<RuleID>,
        kind: RuleKind,
        name: StyleAtomID,
    ) -> RuleID {
        let rule = match before {
            Some(before) => self.insert_rule_before(before, kind),
            None => self.append_rule(sheet, None, kind),
        };
        let mut version = self.current_rule_version(rule);
        version.declared_name = Some(name);
        self.replace_rule_version(rule, version);
        rule
    }

    /// Give an existing rule a new selector list, keeping its identity and its position.
    ///
    /// Editing `selectorText` is one rule changing, not the sheet being rebuilt. The rule keeps its
    /// order token, so nothing around it is renumbered, and the journal sees exactly one selector
    /// field move.
    pub fn replace_style_rule_selectors(
        &mut self,
        rule: RuleID,
        selectors: &[&CompiledSelector],
        namespaces: NamespaceScope,
        scope: &ScopeChain<'_>,
    ) {
        let program = self.compile_selectors(selectors, namespaces, scope, None);
        self.add_routing_rule(rule, program);

        let mut version = self.current_rule_version(rule);
        version.selector_program = Some(program);
        self.replace_rule_version(rule, version);
        self.settle_program();
        self.retained.counters.bump(Counter::StyleRulesCompiled);
    }

    /// Report that a rule's declarations moved, without touching anything else about it.
    ///
    /// The block's contents changed even where the CSSOM object did not, so what makes this a
    /// change is a version rather than the object's address. The rule keeps its identity, its
    /// position, and its selector program, so the journal sees one field move and routing reaches
    /// exactly the elements that rule matches.
    pub fn record_rule_declarations_changed(&mut self, rule: RuleID, block_version: u32) {
        let mut version = self.current_rule_version(rule);
        version.declaration_block = Some(DeclarationBlockID(block_version));
        self.replace_rule_version(rule, version);
        self.settle_program();
    }

    /// Make the identities the host minted live, ahead of anything the host records about their
    /// nodes. Mints are batched because a call per node is exactly the boundary shape this design
    /// rules out.
    pub fn mint_style_nodes(&mut self, nodes: &[u32]) {
        let mut minted_an_element = false;
        for node in nodes.iter().filter_map(|&raw| StyleNodeID::from_raw(raw)) {
            if node.text_index().is_some() {
                self.retained.tree.mint_text(node, &mut self.retained.memory);
            } else {
                self.retained.tree.mint_element(node, &mut self.retained.memory);
                self.retained.counters.bump(Counter::StyleNodesAllocated);
                minted_an_element = true;
            }
        }
        if minted_an_element {
            self.publish_budget_inputs();
        }
    }

    pub fn record_input(&mut self, key: InputKey, old: InputValue, new: InputValue) {
        if self.record(key, old, new) {
            self.apply_to_facts_without_settling(key, new);
        }
    }

    /// Record an exact style reaction for every flat-tree descendant of a node.
    ///
    /// The reaction is one C++ derived from a reaction it applied to `root`, not a fact only C++
    /// holds: a descendant is here because what it inherits moved, which the engine settles itself
    /// wherever its record computation admits it.
    pub fn record_flat_tree_descendant_style_inputs(
        &mut self,
        root: StyleNodeID,
        reaction: u8,
        inherited_style_groups: u8,
    ) {
        if reaction == 0 {
            return;
        }
        let mut descendants = Vec::new();
        self.for_each_flat_tree_descendant(root, |node| descendants.push(node));
        for node in descendants {
            self.record_derived_element_style_input(node, reaction, inherited_style_groups);
        }
    }

    /// Record one member of a flat FFI batch without repeatedly settling the fact-store capacity.
    ///
    /// The bridge has already applied every tree delta before it publishes facts. It can therefore
    /// identify facts carried by a node's arrival once per node instead of probing and replacing
    /// the same journal key once per fact.
    pub(crate) fn record_batched_input(
        &mut self,
        key: InputKey,
        old: InputValue,
        new: InputValue,
        arriving_node: bool,
    ) {
        if arriving_node {
            debug_assert!(matches!(key, InputKey::LocalFeature(..) | InputKey::State(..)));
            self.retained.counters.bump(Counter::ArrivingNodeFactsFolded);
            self.retained.counters.bump(Counter::RawMutationRecords);
            self.retained.counters.bump(Counter::LocalFeatureDeltas);
            self.apply_to_facts_without_settling(key, new);
        } else if self.record(key, old, new) {
            self.apply_to_facts_without_settling(key, new);
        }
    }

    /// Stage one structural delta and the neighbour rows it derives.
    pub(super) fn stage_tree_delta(
        &mut self,
        node: StyleNodeID,
        old: Option<TreeRelations>,
        new: Option<TreeRelations>,
    ) {
        if let Some(old) = old {
            if let Some(previous) = old.previous_element_sibling {
                self.stage_connected_tree_row(previous, |relations| {
                    relations.next_element_sibling = old.next_element_sibling;
                });
            } else if let Some(parent) = old.parent {
                self.stage_first_child(parent, old.next_element_sibling);
            }
            if let Some(next) = old.next_element_sibling {
                self.stage_connected_tree_row(next, |relations| {
                    relations.previous_element_sibling = old.previous_element_sibling;
                });
            }
        }
        if let Some(new) = new {
            if let Some(previous) = new.previous_element_sibling {
                self.stage_connected_tree_row(previous, |relations| {
                    relations.next_element_sibling = Some(node);
                });
            } else if let Some(parent) = new.parent {
                self.stage_first_child(parent, Some(node));
            }
            if let Some(next) = new.next_element_sibling {
                self.stage_connected_tree_row(next, |relations| {
                    relations.previous_element_sibling = Some(node);
                });
            }
        }
        self.stage_tree_row(node, old, new);
        self.settle_tree_staging_memory();
    }

    /// Attach a compiled program at the end of a scope's sheet order.
    pub(super) fn attach_sheet(&mut self, sheet: SheetID, tree_scope: TreeScopeID) {
        self.restore_routing_for_reattached_sheet(sheet);
        let mut sheets = self.current_sheets_in_scope(tree_scope).to_vec();
        let previous_position = sheets.iter().position(|&candidate| candidate == sheet);
        let was_attached = previous_position.is_some();
        sheets.retain(|&candidate| candidate != sheet);
        sheets.push(sheet);
        let order_changed = previous_position.is_some_and(|previous| previous != sheets.len() - 1);
        self.stage_sheets_in_scope(tree_scope, sheets);
        self.record_attachment(sheet, tree_scope, was_attached, true);
        if order_changed {
            self.record_sheet_order_change(tree_scope);
        }
    }

    /// Attach at a position established before the sheet finished loading. Network completion order
    /// does not determine cascade order.
    pub(super) fn attach_sheet_before(&mut self, sheet: SheetID, before: SheetID, tree_scope: TreeScopeID) {
        self.restore_routing_for_reattached_sheet(sheet);
        let mut sheets = self.current_sheets_in_scope(tree_scope).to_vec();
        let previous_position = sheets.iter().position(|&candidate| candidate == sheet);
        let was_attached = previous_position.is_some();
        sheets.retain(|&candidate| candidate != sheet);
        let position = sheets
            .iter()
            .position(|&candidate| candidate == before)
            .unwrap_or(sheets.len());
        sheets.insert(position, sheet);
        let order_changed = previous_position.is_some_and(|previous| previous != position);
        self.stage_sheets_in_scope(tree_scope, sheets);
        // A sheet arriving in the middle is not the scope's order changing. Order is kept as tokens
        // precisely so an insertion writes one label and renumbers nothing, so every sheet already
        // attached keeps the priority it had against every other. What is new is one more competitor
        // for the declarations it makes, and the elements that competition can reach are exactly the
        // ones its own rules match, which the attachment recorded above already names. A sheet that
        // was attached a moment ago and is arriving again can be a move, whose order delta is
        // recorded below.
        self.record_attachment(sheet, tree_scope, was_attached, true);
        if order_changed {
            self.record_sheet_order_change(tree_scope);
        }
    }

    /// Attach a sheet immediately before another sheet in the same scope, or at the end when that
    /// sheet is not attached there. Order tokens stay inside the engine: callers name neighbours,
    /// never positions.
    pub fn attach_sheet_before_sheet(&mut self, sheet: SheetID, before: Option<SheetID>, tree_scope: TreeScopeID) {
        let before = before.filter(|&before| self.current_sheets_in_scope(tree_scope).contains(&before));
        match before {
            Some(before) => self.attach_sheet_before(sheet, before, tree_scope),
            None => self.attach_sheet(sheet, tree_scope),
        }
    }

    pub fn detach_sheet(&mut self, sheet: SheetID, tree_scope: TreeScopeID) {
        let sheets = self.current_sheets_in_scope(tree_scope);
        let Some(position) = sheets.iter().position(|&candidate| candidate == sheet) else {
            return;
        };
        let mut sheets = sheets.to_vec();
        sheets.remove(position);
        self.stage_sheets_in_scope(tree_scope, sheets);
        self.record_attachment(sheet, tree_scope, true, false);
        self.retained.routing_needs_detachment_sweep = true;
    }
}

impl StyleEngine {
    /// Add a `@keyframes` rule, which matches no element and is found by the name it declares.
    ///
    /// It has to be in the program at all for a change to it to be an input, and it has to carry its
    /// name for that input to reach the animations referencing it.
    pub fn add_keyframes_rule(&mut self, sheet: SheetID, before: Option<RuleID>, name: StyleAtomID) -> RuleID {
        self.add_named_rule(sheet, before, RuleKind::Keyframes, name)
    }

    /// Add an `@property` rule, which registers the custom property it names. Registering one changes
    /// how every element that declares or references it computes, which the custom-property index
    /// knows and selector matching cannot say.
    pub fn add_property_rule(&mut self, sheet: SheetID, before: Option<RuleID>, name: StyleAtomID) -> RuleID {
        self.add_named_rule(sheet, before, RuleKind::Property, name)
    }

    /// Add a rule that matches no element and is not found by name either, so that a change to it is
    /// an input at all. What it reaches is decided by its kind.
    pub fn add_non_matching_rule(&mut self, sheet: SheetID, before: Option<RuleID>, kind: RuleKind) -> RuleID {
        match before {
            Some(before) => self.insert_rule_before(before, kind),
            None => self.append_rule(sheet, None, kind),
        }
    }

    /// Stage a structural change. The normalized transaction installs the final relation rows at
    /// the next observation boundary.
    pub fn record_tree_delta(&mut self, node: StyleNodeID, old: Option<TreeRelations>, new: Option<TreeRelations>) {
        if old != new {
            self.stage_tree_delta(node, old, new);
        }
    }

    /// Record a registration made through `CSS.registerProperty()`. Stylesheet registrations are
    /// already represented by their `@property` rule's program input.
    pub fn record_custom_property_registration_change(&mut self, name: StyleAtomID) {
        self.record_input(
            InputKey::CustomPropertyRegistration(name),
            InputValue::Flag(false),
            InputValue::Flag(true),
        );
    }

    /// Record a state publication against the settled value StyleEngine already owns.
    ///
    /// State invalidators may conservatively republish a related group of pseudo-classes when one
    /// member changes. The settled fact is therefore the old side; assuming every publication is
    /// a Boolean toggle would invent transitions for the unchanged members of that group.
    pub(crate) fn record_batched_state(
        &mut self,
        node: StyleNodeID,
        fact: StateFact,
        new_value: bool,
        arriving_node: bool,
    ) {
        let old_value = self.retained.facts.states_of_node(node).contains(fact);
        self.record_batched_input(
            InputKey::State(node, fact),
            InputValue::State(old_value),
            InputValue::State(new_value),
            arriving_node,
        );
    }

    /// Install the fixed facts carried by one element arrival. The tree row already routes the
    /// arriving element, so facts which can change later update their columns without adding one
    /// journal entry apiece.
    pub(crate) fn record_element_arrival(
        &mut self,
        node: StyleNodeID,
        arrival: &super::bridge::FfiElementArrival,
        custom_states: &[StyleAtomID],
        arriving_node: bool,
    ) {
        debug_assert!(arriving_node);
        self.retained
            .facts
            .set_namespace(node, StyleAtomID(arrival.namespace_atom));
        self.retained.facts.set_is_slot(node, arrival.is_slot);
        let mut publish_feature = |feature, value| {
            self.record_batched_input(
                InputKey::LocalFeature(node, feature),
                InputValue::Feature(FeatureValue::Absent),
                InputValue::Feature(value),
                arriving_node,
            );
        };
        if arrival.language_atom != 0 {
            publish_feature(
                LocalFeatureKey::Language,
                FeatureValue::Atom(StyleAtomID(arrival.language_atom)),
            );
        }
        if arrival.directionality_atom != 0 {
            publish_feature(
                LocalFeatureKey::Directionality,
                FeatureValue::Atom(StyleAtomID(arrival.directionality_atom)),
            );
        }
        if arrival.heading_level != 0 {
            publish_feature(
                LocalFeatureKey::HeadingLevel,
                FeatureValue::Number(u32::from(arrival.heading_level)),
            );
        }
        self.retained
            .computed_group_sets
            .set_adjustment_facts(node, arrival.adjustment_facts);
        self.retained
            .computed_group_sets
            .set_construction_facts(node, arrival.construction_facts);
        self.retained
            .computed_group_sets
            .set_box_kind(node, super::bridge::ElementBoxKind::from_raw(arrival.box_kind));
        self.retained
            .set_element_associated_pseudo_kind(node, arrival.associated_pseudo_kind_plus_one);
        for &state in custom_states {
            self.record_batched_input(
                InputKey::LocalFeature(node, LocalFeatureKey::CustomState(state)),
                InputValue::Feature(FeatureValue::Absent),
                InputValue::Feature(FeatureValue::Present),
                arriving_node,
            );
        }
        self.retained
            .facts
            .set_custom_states(node, custom_states, &mut self.retained.memory);
    }

    /// Record the shadow parts an element exposes.
    ///
    /// A part is a fact about the element like a class is: posted so `::part()` rules can be
    /// enumerated from it, and journalled so a change to the `part` attribute routes. The plain
    /// name set is derived here from the name-to-host pairs exact matching needs, so the two views
    /// cannot disagree.
    pub fn set_element_parts(&mut self, node: StyleNodeID, pairs: &[(StyleAtomID, StyleNodeID)]) {
        let mut parts = Vec::new();
        for &(part, _) in pairs {
            if !parts.contains(&part) {
                parts.push(part);
            }
        }
        if self.retained.facts.parts_of(node) != parts {
            let previous: Vec<StyleAtomID> = self.retained.facts.parts_of(node).to_vec();
            for part in previous.iter().filter(|part| !parts.contains(part)) {
                self.record_input(
                    InputKey::LocalFeature(node, LocalFeatureKey::Part(*part)),
                    InputValue::Feature(FeatureValue::Present),
                    InputValue::Feature(FeatureValue::Absent),
                );
            }
            for part in parts.iter().filter(|part| !previous.contains(part)) {
                self.record_input(
                    InputKey::LocalFeature(node, LocalFeatureKey::Part(*part)),
                    InputValue::Feature(FeatureValue::Absent),
                    InputValue::Feature(FeatureValue::Present),
                );
            }
            self.retained.facts.set_parts(node, &parts, &mut self.retained.memory);
        }
        self.retained
            .tree
            .set_part_hosts(node, pairs, &mut self.retained.memory);
    }

    /// Report the outermost host a `::part()` rule can address this element from.
    ///
    /// What an `exportparts` change moves is which scopes can name an element, not which names it
    /// carries - the forwarded name is usually the one it already had. So the exposure is the fact,
    /// and an element whose reach did not move says nothing.
    pub fn set_element_part_exposure(&mut self, node: StyleNodeID, exposure: StyleAtomID) {
        let previous = self.retained.facts.part_exposure_of(node);
        if previous == exposure {
            return;
        }
        self.record_input(
            InputKey::LocalFeature(node, LocalFeatureKey::PartExposure),
            InputValue::Feature(RetainedState::atom_or_absent(previous)),
            InputValue::Feature(RetainedState::atom_or_absent(exposure)),
        );
    }

    pub fn set_element_heading_level(&mut self, node: StyleNodeID, level: u8) {
        let previous = self.retained.facts.heading_level_of(node);
        if previous == level {
            return;
        }
        self.record_input(
            InputKey::LocalFeature(node, LocalFeatureKey::HeadingLevel),
            InputValue::Feature(FeatureValue::Number(u32::from(previous))),
            InputValue::Feature(FeatureValue::Number(u32::from(level))),
        );
    }

    pub fn set_element_language(&mut self, node: StyleNodeID, language: StyleAtomID) {
        let previous = self.retained.facts.language_of(node);
        if previous == language {
            return;
        }
        self.record_input(
            InputKey::LocalFeature(node, LocalFeatureKey::Language),
            InputValue::Feature(RetainedState::atom_or_absent(previous)),
            InputValue::Feature(RetainedState::atom_or_absent(language)),
        );
    }

    /// Report the element's resolved directionality, which `:dir()` tests.
    pub fn set_element_directionality(&mut self, node: StyleNodeID, directionality: StyleAtomID) {
        let previous = self.retained.facts.directionality_of(node);
        if previous == directionality {
            return;
        }
        self.record_input(
            InputKey::LocalFeature(node, LocalFeatureKey::Directionality),
            InputValue::Feature(RetainedState::atom_or_absent(previous)),
            InputValue::Feature(RetainedState::atom_or_absent(directionality)),
        );
    }

    /// Replace the custom states an element is in.
    ///
    /// A custom state is a named fact about one element, exactly like a class, so it is published as
    /// one: the names that arrived and the names that left are each a local feature moving, and
    /// `:state()` reaches its subjects through the same postings every other name does.
    pub fn set_element_custom_states(&mut self, node: StyleNodeID, states: &[StyleAtomID]) {
        if self.retained.facts.custom_states_of(node) == states {
            return;
        }
        let previous: Vec<StyleAtomID> = self.retained.facts.custom_states_of(node).to_vec();
        for state in previous.iter().filter(|state| !states.contains(state)) {
            self.record_input(
                InputKey::LocalFeature(node, LocalFeatureKey::CustomState(*state)),
                InputValue::Feature(FeatureValue::Present),
                InputValue::Feature(FeatureValue::Absent),
            );
        }
        for state in states.iter().filter(|state| !previous.contains(state)) {
            self.record_input(
                InputKey::LocalFeature(node, LocalFeatureKey::CustomState(*state)),
                InputValue::Feature(FeatureValue::Absent),
                InputValue::Feature(FeatureValue::Present),
            );
        }
        self.retained
            .facts
            .set_custom_states(node, states, &mut self.retained.memory);
    }
}

impl StyleEngine {}

impl RetainedState {
    /// Drop what the engine retains for an element whose identity retires. Identities are handed
    /// out again once released, so a row left behind would describe whatever element is given the
    /// identity next. Every field is named here, so a new field fails to compile until it says
    /// what it does with the node: a table this retires a row of is bound and cleared below, and
    /// `_` is either not keyed by style node or says where its rows go instead.
    pub(super) fn retire_node_state(&mut self, node: StyleNodeID) {
        let Self {
            counters: _,
            memory: _,
            admission: _,
            deferred_pseudo_elements: _,
            // Retires the whole batch at once, in `retire_elements`.
            tree: _,
            program: _,
            native_rules: _,
            declaration_block_version: _,
            last_transaction_only_derived_child_reactions: _,
            sheets_excluded_from_routing: _,
            routing_needs_detachment_sweep: _,
            match_workspace: _,
            // Scratch of one candidate evaluation.
            exact_covered_scratch: _,
            cascade_compaction_scratch: _,
            cascade_compaction_scratch_memory: _,
            next_style_transaction_version: _,
            document_style_computation_inputs: _,
            driven_viewport: _,
            document_resource_contexts: _,
            document_media: _,
            document_functions: _,
            custom_property_registry: _,
            font_resolution: _,
            monospace_font_family: _,
            layer_topology_version: _,
            sheet_order_version: _,
            specified_values: _,
            winner_groups,
            computed_group_sets,
            custom_property_environments: _,
            nodes_with_substituted_records,
            custom_declaration_reads,
            nodes_with_tree_counting_records,
            nodes_with_rolled_back_records,
            nodes_with_element_relative_substitutions,
            element_custom_property_data,
            pseudo_element_custom_property_data,
            environment_move_recompute_nodes,
            container_effects_for_host,
            published_container_verdicts,
            container_gates_unheld,
            row_inputs_moved,
            container_query_inputs,
            layout_style_snapshots,
            size_container_queries,
            counter_style_environment_identities: _,
            held_style_records,
            // Empty but while a clock frame runs, which no retirement reaches.
            tick_shown: _,
            backing_elements,
            children_explicitly_inherit_marks,
            css_defined_animations,
            animation_keyframes: _,
            animation_effect_descriptions,
            // Like the host, a `rem` keeps reading the last document element's font until another
            // takes its place.
            held_root_font_inputs: _,
            random_base_values,
            replaced_content_inputs,
            transition_baselines,
            custom_property_registrations_changed: _,
            // Settled or reverted when the transaction's outputs are discarded, before identities are
            // released.
            engine_computed_records_pending: _,
            demand_records,
            flush_stamp: _,
            engine_pseudo_record_cache: _,
            // Filled and cleared within one transaction's record loop.
            batch_answers_complete_but_for_custom_properties: _,
            batch_custom_property_matches: _,
            batch_backing_pseudo_matches: _,
            engine_cold_record_cache: _,
            engine_cold_record_donors: _,
            computed_group_set_memory: _,
            custom_property_environment_memory: _,
            computed_fixed_metadata_memory: _,
            computed_longhand_table_memory: _,
            style_record_memory: _,
            animation_overlay_memory: _,
            computed_pseudo_assignment_memory: _,
            style_invalidation_cache: _,
            match_answers: _,
            selector_truth_sets: _,
            // Forgotten for departed elements in `forget_departed_elements`.
            retained_match_answers: _,
            // Cleared by every transaction with a tree input, which a departure is.
            retained_selector_incidences: _,
            selector_incidence_is_current: _,
            // Scratch of one traversal.
            batch_matching_traversal: _,
            route_pruning_states: _,
            completion_exactness: _,
            // Dropped in `forget_departed_elements` when a relation holds a departed element.
            prefix_caches: _,
            #[cfg(test)]
                force_bounded_prefix_completion: _,
            // Scratch of one transaction.
            prepared_batch_matching_traversal: _,
            published_match_answers: _,
            transaction_fact_view: _,
            // Forgotten for departed elements in `forget_departed_elements`.
            facts: _,
            programs: _,
            attribute_value_text_names: _,
            attribute_value_text_requirements_version: _,
            selector_programs_need_sweep: _,
            routing: _,
            // Scratch of one transaction.
            selector_truth_changes: _,
            already_planned_selector_truth: _,
            selector_truth_changes_active: _,
            // A witness is checked against the live tree before it is used.
            relational_witnesses: _,
            // Scratch of one transaction.
            pending_witness_effects: _,
            witness_effect_scratch: _,
            relational_witness_residency: _,
            // Forgotten for departed scope roots in `forget_departed_elements`.
            scope_roots: _,
            scope_by_root: _,
            scope_programs: _,
            vacant_scope_programs: _,
            scope_dispatch_templates: _,
            scope_cascade_templates: _,
            ancestor_dispatch_templates: _,
            scope_program_by_scope: _,
            atoms: _,
            html_element_namespace: _,
            fold_id_and_class_name_case: _,
            #[cfg(test)]
                diagnostic_plan_capture: _,
        } = self;
        winner_groups.remove(node);
        computed_group_sets.remove(node);
        nodes_with_substituted_records.remove(&node);
        custom_declaration_reads.remove(&node);
        nodes_with_tree_counting_records.remove(&node);
        nodes_with_rolled_back_records.remove(&node);
        nodes_with_element_relative_substitutions.remove(&node);
        element_custom_property_data.remove(&node);
        pseudo_element_custom_property_data.remove(&node);
        environment_move_recompute_nodes.remove(&node);
        container_effects_for_host.set(node, None);
        published_container_verdicts.remove(&node);
        container_gates_unheld.remove(&node);
        row_inputs_moved.forget(node);
        container_query_inputs.clear(node);
        layout_style_snapshots.remove(&node);
        demand_records.retain(|target, record| {
            let retired = target.node() == node;
            if retired {
                computed_group_sets.unpin_style_record(record.raw());
            }
            !retired
        });
        size_container_queries.retire(node);
        backing_elements.remove(&node);
        if let Some(style_record) = held_style_records.remove(&node) {
            computed_group_sets.unpin_style_record(style_record);
        }
        children_explicitly_inherit_marks.remove(&node);
        css_defined_animations.retire(node);
        animation_effect_descriptions.retire(node);
        random_base_values.retire(node);
        replaced_content_inputs.remove(&node);
        // A retired identity can name another element before the epoch commits.
        for style_record in transition_baselines.remove(node) {
            computed_group_sets.unpin_style_record(style_record);
        }
    }
}

impl HostState {
    /// Whether `node` still owes a deferred style input. The deferred inputs are kept sorted by
    /// key, so this is a binary search.
    pub(super) fn owes_element_style_input(&self, node: StyleNodeID) -> bool {
        self.deferred_element_style_inputs
            .binary_search_by_key(&InputKey::ElementStyleInput(node), |pending| pending.key)
            .is_ok()
    }
}

/// Mints the next declaration block version of `versions`.
pub(crate) fn next_declaration_block_version(versions: &std::sync::atomic::AtomicU32) -> u32 {
    versions
        .fetch_add(1, std::sync::atomic::Ordering::Relaxed)
        .checked_add(1)
        .expect("declaration revision overflow")
}

/// The attribute value text requirements version of an engine whose selectors' requirements are at `selector_version`:
/// it moves with every name an `attr()` anywhere in the process can newly read, too.
pub(crate) fn with_attr_names_read(selector_version: u64) -> u64 {
    selector_version.wrapping_add(crate::css::parser::arbitrary_substitution::attr_names_read_generation())
}
