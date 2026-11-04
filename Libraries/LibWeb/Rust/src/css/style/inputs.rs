/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::css::selector::SelectorListRef;
#[cfg(test)]
use projection_writes::ProjectionWrites;

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
    pub opens_or_closes_quotes: bool,
}

/// The custom-property environment one element holds: the store the host holds as the environment,
/// which the engine keeps a reference to. The element keeps no copy of its own.
#[derive(Clone)]
pub(crate) struct RetainedCustomPropertyData {
    store: std::sync::Arc<crate::css::custom_properties::CustomPropertyStore>,
}

impl RetainedCustomPropertyData {
    /// # Safety
    /// `data` must be a live `Web::CSS::CustomPropertyData`, which is a store the host holds a
    /// reference to.
    pub(super) unsafe fn retain(data: *const std::ffi::c_void) -> Self {
        let store = data.cast::<crate::css::custom_properties::CustomPropertyStore>();
        // SAFETY: Guaranteed by the caller: the store lives in an Arc.
        unsafe { std::sync::Arc::increment_strong_count(store) };
        Self {
            store: unsafe { std::sync::Arc::from_raw(store) },
        }
    }

    pub(crate) fn data(&self) -> *const std::ffi::c_void {
        std::sync::Arc::as_ptr(&self.store).cast()
    }
}

/// The style record each element holds, by element index, as the host reports every record it
/// installs or clears: what the host holds, which lags what the engine assigned until the host
/// takes an installation, and which it clears under `display: none`. The column holds a reference
/// to each, which keeps the record, and the identity naming it, until the host reports another.
/// Nearly every element holds one, so a dense column takes less than a map would.
///
/// The column is copied on write in chunks: a fork shares it whole, and a host write while a fork
/// lives copies only the chunk it lands in.
#[derive(Clone, Default)]
pub(crate) struct HeldStyleRecords {
    by_element: crate::cow_column::CowColumn<HeldRecord, HELD_STYLE_RECORDS_PER_CHUNK>,
}

/// A row of [`HeldStyleRecords`], which compares by the record it holds.
#[derive(Clone, Default)]
struct HeldRecord(Option<std::sync::Arc<super::pipeline::Record>>);

impl PartialEq for HeldRecord {
    fn eq(&self, other: &Self) -> bool {
        match (&self.0, &other.0) {
            (Some(record), Some(other)) => std::sync::Arc::ptr_eq(record, other),
            (None, None) => true,
            _ => false,
        }
    }
}

/// How many elements' records a chunk of [`HeldStyleRecords`] holds: a host write while a fork
/// lives copies this many.
const HELD_STYLE_RECORDS_PER_CHUNK: usize = 64;

impl HeldStyleRecords {
    /// The record `node` holds.
    #[must_use]
    pub(crate) fn get(&self, node: StyleNodeID) -> Option<&std::sync::Arc<super::pipeline::Record>> {
        self.by_element.get(node.element_index()? as usize)?.0.as_ref()
    }

    #[must_use]
    pub(crate) fn contains(&self, node: StyleNodeID) -> bool {
        self.get(node).is_some()
    }

    /// Note that `node` holds `record`, or none, answering the one it held.
    pub(crate) fn set(
        &mut self,
        node: StyleNodeID,
        record: Option<std::sync::Arc<super::pipeline::Record>>,
    ) -> Option<std::sync::Arc<super::pipeline::Record>> {
        let index = node.element_index().expect("only elements hold style records") as usize;
        if self.by_element.len() <= index {
            record.as_ref()?;
            self.by_element.grow_to(index + 1);
        }
        self.by_element
            .replace(index, HeldRecord(record))
            .expect("the column holds every element below its length")
            .0
    }

    /// Every element holding a record, with the record.
    pub(crate) fn iter(&self) -> impl Iterator<Item = (StyleNodeID, &std::sync::Arc<super::pipeline::Record>)> + '_ {
        (0..self.by_element.len()).filter_map(|index| {
            let record = self.by_element.get(index)?.0.as_ref()?;
            Some((
                StyleNodeID::element(u32::try_from(index).expect("element index exceeds u32")),
                record,
            ))
        })
    }

    pub(crate) fn heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        self.by_element.heap_size(sizer, |_, _| 0)
    }
}

/// The custom-property environment an element or one of its synthetic pseudo-elements holds.
#[derive(Clone)]
pub(crate) struct HeldCustomPropertyEnvironment {
    /// The identity the host's object names the environment by.
    pub(crate) identity: u64,
    /// For the element's animation overlay, the environment its style resolved to, which its
    /// animations sampled their custom properties over.
    pub(crate) sampled_over: Option<u64>,
    pub(crate) data: RetainedCustomPropertyData,
}

impl RetainedState {
    /// Says whether the document matches id and class selectors ASCII case-insensitively.
    ///
    /// A document only learns its mode while it is still empty - a parser reads it from the doctype
    /// before the first element arrives, and `document.open()` has already removed every element and
    /// every sheet by the time it resets it - so no rule compiled against the other folding and no
    /// fact published under it can outlive the change.
    pub fn set_fold_id_and_class_name_case(&mut self, fold: bool) {
        self.fold_id_and_class_name_case = fold;
    }

    /// What matching selectors in the document needs to know of it.
    pub(super) fn document_matching(&self) -> super::component_matching::DocumentMatching {
        super::component_matching::DocumentMatching {
            html_namespace: self.html_element_namespace,
            fold_ids_and_classes: self.fold_id_and_class_name_case,
        }
    }

    /// The HTML namespace, for an HTML document, or none for any other kind. It is what decides
    /// whether an attribute name from the legacy list compares its value case-insensitively.
    pub fn set_html_element_namespace(&mut self, namespace: StyleAtomID) {
        self.html_element_namespace = namespace;
    }

    #[must_use]
    pub fn tree(&self) -> &HostTree {
        &self.tree
    }

    #[must_use]
    pub fn connected_element_count(&self) -> u32 {
        self.tree.connected_element_count()
    }

    #[must_use]
    #[cfg(test)]
    pub(super) fn rules(&self) -> &RuleStore {
        &self.rules
    }

    /// The process-global atom for one interned name identity.
    ///
    /// Selector names and DOM facts intern through here and nowhere else. Two tables keyed by the
    /// same word but assigning their own sequences would compare unequal for the same name, which
    /// is a silent failure to match rather than a loud one.
    pub fn intern_atom(&mut self, raw: usize) -> StyleAtomID {
        self.atoms.intern_raw(raw)
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
    pub unsafe fn note_custom_property_name(&mut self, name: StyleAtomID, raw: usize) {
        if unsafe { self.custom_property_environments.note_name(name, raw) } {
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

    /// Record the attribute names `selectors`, inside the `@scope` levels `scope`, test, those whose values they read
    /// as text, and whether they test `:local-link`, which is all the host asks of an element's attributes that a
    /// selector may not read, and what an arrival has to carry to reach the anchor of one of their `:has()`.
    pub(super) fn note_selectors(
        &mut self,
        selectors: &SelectorListRef,
        scope: Option<&super::bridge::BoundScopeChain>,
    ) {
        self.note_selector_summary(super::selector_analysis::SelectorSummary::of(selectors.view(), scope));
    }

    /// What the selectors the engine noted test, summed up.
    pub(super) fn selector_summary(&self) -> super::selector_analysis::SelectorSummary {
        super::selector_analysis::SelectorSummary {
            tested_attribute_names: self.tested_attribute_names.iter().copied().collect(),
            attribute_value_text_names: self.attribute_value_text_names.iter().copied().collect(),
            tests_local_link: self.selectors_test_local_link,
            tests_validity: self.selectors_test_validity,
            relational_arrival_keys: self.unresolved_relational_arrival_keys.iter().copied().collect(),
        }
    }

    /// Note what the selectors `summary` sums up test, which the engine's fact projection reads.
    pub(super) fn note_selector_summary(&mut self, summary: super::selector_analysis::SelectorSummary) {
        self.selectors_test_local_link |= summary.tests_local_link;
        self.selectors_test_validity |= summary.tests_validity;
        if !summary
            .tested_attribute_names
            .iter()
            .all(|name| self.tested_attribute_names.contains(name))
        {
            Arc::make_mut(&mut self.tested_attribute_names).extend(summary.tested_attribute_names.iter().copied());
        }
        for key in summary.relational_arrival_keys {
            if !self.relational_arrival_keys.keys.contains(&key) {
                self.unresolved_relational_arrival_keys.insert(key);
            }
        }
        if summary
            .attribute_value_text_names
            .iter()
            .all(|name| self.attribute_value_text_names.contains(name))
        {
            return;
        }
        Arc::make_mut(&mut self.attribute_value_text_names).extend(summary.attribute_value_text_names);
        self.attribute_value_text_requirements_version += 1;
    }

    /// Starts the keys over from `keys`, as a sweep that reclaimed some of their atoms leaves them.
    pub(super) fn replace_relational_arrival_keys(&mut self, keys: HashSet<DispatchKey>) {
        self.relational_arrival_keys = Arc::default();
        self.unresolved_relational_arrival_keys = keys;
        self.relational_arrival_keys_version += 1;
    }

    /// Resolves the keys rules added since the host last learned them to what it compares an element's names and
    /// states with. The atom table is scanned once for all of them, however many rules added one.
    pub(crate) fn resolve_relational_arrival_keys(&mut self) {
        if self.unresolved_relational_arrival_keys.is_empty() {
            return;
        }
        let mut resolved = (*self.relational_arrival_keys).clone();
        let mut name_atoms = Vec::new();
        for key in std::mem::take(&mut self.unresolved_relational_arrival_keys) {
            if !resolved.keys.insert(key) {
                continue;
            }
            match key {
                DispatchKey::TagName(atom)
                | DispatchKey::Id(atom)
                | DispatchKey::Class(atom)
                | DispatchKey::AttributeName(atom) => name_atoms.push(atom),
                DispatchKey::State(fact) => resolved.states.push(fact as u8),
                _ => resolved.reached_by_any_arrival = true,
            }
        }
        match self.atoms.raw_identities_of(name_atoms) {
            Some(raws) => resolved.names.extend(raws),
            None => resolved.reached_by_any_arrival = true,
        }
        self.relational_arrival_keys = Arc::new(resolved);
        self.relational_arrival_keys_version += 1;
    }

    /// Moves whenever what an arrival has to carry to reach a `:has()` anchor does, once resolved.
    pub(crate) fn relational_arrival_keys_version(&self) -> u64 {
        self.relational_arrival_keys_version
    }

    /// What an arrival has to carry to reach a `:has()` anchor, as last resolved.
    pub(crate) fn relational_arrival_keys(&self) -> &Arc<RelationalArrivalKeys> {
        &self.relational_arrival_keys
    }

    /// Moves whenever an attribute name comes to require its value text for a selector here.
    pub fn selector_attribute_value_text_requirements_version(&self) -> u64 {
        self.attribute_value_text_requirements_version
    }

    /// The attribute names whose value text a selector here reads.
    pub(crate) fn selector_attribute_value_text_names(&self) -> &SelectorAttributeNames {
        &self.attribute_value_text_names
    }

    /// Whether a selector here tests `:local-link`.
    pub(crate) fn selectors_test_local_link(&self) -> bool {
        self.selectors_test_local_link
    }

    pub(crate) fn selectors_test_validity(&self) -> bool {
        self.selectors_test_validity
    }

    /// The attribute names a selector here tests in any way.
    pub(crate) fn selector_tested_attribute_names(&self) -> &SelectorAttributeNames {
        &self.tested_attribute_names
    }

    /// Apply an accepted input to the authoritative fact arrangement.
    ///
    /// A fact exists in the store exactly because a mutation published it, which is what lets the
    /// evaluator distinguish "this element has no such class" from "nothing ever told me about this
    /// element".
    pub(super) fn apply_to_facts_without_settling(&mut self, key: InputKey, new: InputValue) {
        match (key, new) {
            #[cfg(test)]
            (InputKey::LocalFeature(node, feature), InputValue::Feature(value)) => {
                self.write_projection_for_test(|writes| writes.apply_feature(node, feature, value));
            }
            _ => {}
        }
    }

    /// Write the projection as a test's host, publishing the writes to it at once and counting them.
    #[cfg(test)]
    pub(super) fn write_projection_for_test<R>(&mut self, write: impl FnOnce(&mut ProjectionWrites<'_>) -> R) -> R {
        let (answer, counts) = self.projection.write_for_test(|writer| write(&mut writer.writes()));
        for (counter, count) in counts {
            self.counters.add(counter, count);
        }
        answer
    }

    /// How `node` stands in the tree as the last transaction committed it: detached in its tree
    /// scope where it stood in no tree then.
    pub(super) fn settled_tree_relations(&self, node: StyleNodeID) -> TreeRelations {
        self.projection
            .committed_relations(node)
            .unwrap_or_else(|| TreeRelations {
                parent: None,
                previous_element_sibling: None,
                next_element_sibling: None,
                tree_scope: self.projection.tree_scope(node),
                assigned_slot: None,
            })
    }

    /// Of the elements whose links were written, those under which the deepest element may have deepened: those that
    /// arrived, and those standing under another parent than the last transaction committed.
    pub(super) fn depth_recompute_nodes(&self, nodes: &[StyleNodeID]) -> HashSet<StyleNodeID> {
        nodes
            .iter()
            .copied()
            .filter(|&node| {
                self.projection.written_relations(node).is_some_and(|relations| {
                    self.projection
                        .committed_relations(node)
                        .is_none_or(|committed| committed.parent != relations.parent)
                })
            })
            .collect()
    }

    /// The depth of `node` in the tree as its links were written, which walks its ancestors.
    #[must_use]
    pub(super) fn element_depth(&self, node: StyleNodeID) -> u32 {
        let written_parent = |node| {
            self.projection
                .written_relations(node)
                .and_then(|relations| relations.parent)
        };
        let mut depth = 0;
        let mut ancestor = written_parent(node);
        while let Some(parent) = ancestor {
            depth += 1;
            ancestor = written_parent(parent);
        }
        depth
    }

    /// Report the element's resolved language tag.
    /// Record the element's namespace.
    ///
    /// An element's namespace is fixed when it is created, so this is a fact the projection holds
    /// rather than an input that moves: nothing routes from it, and no transaction notes it.
    #[cfg(test)]
    pub fn set_element_namespace(&mut self, node: StyleNodeID, namespace: StyleAtomID) {
        self.projection.set_namespace(node, namespace);
    }

    /// The element facts the host published, as `bridge::element_adjustment_fact` names them. A text node and an
    /// identity minted again hold none.
    #[must_use]
    pub fn element_adjustment_facts(&self, node: StyleNodeID) -> u32 {
        self.projection.adjustment_facts(node)
    }

    /// Record what the element gives the natural size of its replaced content.
    pub fn set_element_replaced_content_input(&mut self, node: StyleNodeID, input: ReplacedContentInput) {
        // NB: A fork shares the table until either side writes it, so an input that moves nothing writes nothing.
        if self.element_replaced_content_input(node) == input {
            return;
        }
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

    /// The identity of the record the host holds for the element `node`.
    #[must_use]
    pub(crate) fn held_style_record(&self, node: StyleNodeID) -> Option<u64> {
        let identity = bridge::host_record_identity(self.held_style_records.get(node)?);
        (identity != 0).then_some(identity)
    }

    /// The element asks for another principal box, as its published host facts say.
    pub(crate) fn note_box_kind_change(&mut self, node: StyleNodeID) {
        if let Some(style_record) = self.held_style_record(node) {
            self.set_element_container_query_inputs(node, style_record);
        }
    }

    /// Which principal box the element asks for. A text node and a retired identity ask for
    /// nothing in particular.
    #[must_use]
    pub(crate) fn element_box_kind(&self, node: StyleNodeID) -> super::bridge::ElementBoxKind {
        self.projection.box_kind(node)
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
        self.projection.construction_facts(node)
    }

    /// The record the element published, which its box is built from: the one a clock frame shows in its place, the
    /// record the element holds, or the one the engine assigned it before it holds one. `None` while the element has
    /// no record: a text node, a retired identity, or an element style has not reached yet.
    #[must_use]
    pub fn element_published_style_record(&self, node: StyleNodeID) -> Option<u64> {
        let shown = self.tick_shown.element(node);
        shown
            .map(computed::FinalStyleRecordID::raw)
            .or_else(|| self.held_style_record(node))
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
            opens_or_closes_quotes: view.content_opens_or_closes_quotes(),
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
    /// over a node that is not an element. A slottable is below the slot it is assigned to.
    #[must_use]
    pub(crate) fn has_inclusive_ancestor_with_display_none_ignoring_animations(&self, node: StyleNodeID) -> bool {
        std::iter::successors(Some(node), |&current| {
            self.projection
                .assigned_slot_of(current)
                .or_else(|| self.projection.parent_or_shadow_host(current))
        })
        .any(|ancestor| {
            self.is_child_of_shadow_host_no_slot_takes(ancestor)
                || self
                    .computed_group_sets
                    .assigned_style_record(ancestor)
                    .and_then(|record| self.computed_group_sets.style_record_view(record.raw()))
                    .is_some_and(|view| {
                        crate::css::computed_value_views::ComputedValuesView::of(view.base_payloads)
                            .display()
                            .is_none()
                            && !view.longhands().display_none_is_animated()
                    })
        })
    }

    /// Whether `node` is a child of a shadow host that no slot takes. Such an element is not in the flat tree, so it
    /// generates no box, as if it were below a `display: none` element.
    pub(crate) fn is_child_of_shadow_host_no_slot_takes(&self, node: StyleNodeID) -> bool {
        if self.projection.assigned_slot_of(node).is_some() {
            return false;
        }
        self.projection
            .parent(node)
            .is_some_and(|parent| self.projection.shadow_root_of(parent).is_some())
    }

    /// Whether the element is a `<slot>`, whose children the flat tree takes elsewhere.
    #[must_use]
    pub fn element_is_slot(&self, node: StyleNodeID) -> bool {
        self.projection.written_facts(node).is_some_and(|facts| facts.is_slot())
    }

    fn published_box_facts(&self, style_record: Option<computed::FinalStyleRecordID>) -> Option<PublishedBoxFacts> {
        self.style_record_box_facts(style_record?.raw())
    }

    /// The box facts a style record holds. `None` for a record that holds no payloads.
    #[must_use]
    pub fn style_record_box_facts(&self, style_record: u64) -> Option<PublishedBoxFacts> {
        let payloads = self.computed_group_sets.style_record_payloads(style_record)?;
        let view = crate::css::computed_value_views::ComputedValuesView::of(payloads);
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
            .projection
            .assigned_slot_of(node)
            .or_else(|| self.tree.text_parent(node))
            .filter(|parent| self.projection.host_of(*parent).is_none() && !self.tree.is_relation_only(*parent));
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

    /// Whether the text node collapses its white space, as the element it inherits from publishes: its slot, its
    /// parent, or the host of the shadow root it is a child of. `None` where no element publishes a style for it.
    #[must_use]
    pub fn text_collapses_white_space(&self, node: StyleNodeID) -> Option<bool> {
        let parent = self
            .inheritance_parent(node)
            .filter(|parent| !self.tree.is_relation_only(*parent))?;
        let view = self.published_style_record_view(self.published_element_record(parent))?;
        Some(view.white_space_collapse() == crate::css::css_enums::white_space_collapse::COLLAPSE)
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
        self.projection.language_tag_of(node)
    }

    /// The language tag a text node's transform reads: the one its DOM parent element resolves to.
    /// A text node under a shadow root or the document has no element above it and reads none.
    pub(crate) fn text_language_tag(&self, node: StyleNodeID) -> &[u16] {
        self.tree
            .text_parent(node)
            .filter(|&parent| self.projection.host_of(parent).is_none() && !self.tree.is_relation_only(parent))
            .map_or(&[], |parent| self.projection.language_tag_of(parent))
    }

    /// Text directly under a shadow root uses its host's language for line breaking.
    pub(crate) fn text_line_break_language_tag(&self, node: StyleNodeID) -> &[u16] {
        self.tree
            .text_parent(node)
            .map(|parent| self.projection.host_of(parent).unwrap_or(parent))
            .filter(|&parent| !self.tree.is_relation_only(parent))
            .map_or(&[], |parent| self.projection.language_tag_of(parent))
    }

    /// Record the text node's whitespace-only state, as its data now spells it.
    pub fn set_text_is_ascii_whitespace(&mut self, node: StyleNodeID, value: bool) {
        self.tree.set_text_is_ascii_whitespace(node, value);
    }

    /// Record the unique node id the document names the element by. It arrives with the identity
    /// and never changes while the element holds it.
    pub fn set_element_unique_node_id(&mut self, node: StyleNodeID, unique_node_id: i64) {
        self.tree.set_unique_node_id(node, unique_node_id);
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
        self.tree.set_dom_paint_facts(node, facts);
    }

    /// The spans a row built for the element takes from its attributes.
    #[must_use]
    pub fn element_table_spans(&self, node: StyleNodeID) -> super::tree::TableSpans {
        self.tree.table_spans(node)
    }

    /// Record the spans a row built for the element takes from its attributes.
    pub fn set_element_table_spans(&mut self, node: StyleNodeID, spans: super::tree::TableSpans) {
        self.tree.set_table_spans(node, spans);
    }

    /// Record whether the text node holds the value of a password input.
    pub fn set_text_is_password_input(&mut self, node: StyleNodeID, value: bool) {
        self.tree.set_text_is_password_input(node, value);
    }

    /// Record the characters the text node now holds.
    pub fn set_text_data(&mut self, node: StyleNodeID, data: ak::Utf16String) {
        self.tree.set_text_data(node, data);
    }

    /// Record which kind of tree the text node arrived in.
    pub fn set_text_is_in_user_agent_shadow_tree(&mut self, node: StyleNodeID, value: bool) {
        self.tree.set_text_is_in_user_agent_shadow_tree(node, value);
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
        let tree_scope = self.projection.tree_scope(node);
        if self.rules.scope_uses_document_sheets(tree_scope) {
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

    /// The longhands of the published style record `raw_style_record` names, which keep what the
    /// record's values were specified as. `None` for zero, which names no record.
    #[must_use]
    pub(crate) fn published_record_longhands(&self, raw_style_record: u64) -> Option<computed::ViewLonghands<'_>> {
        let style_record = computed::FinalStyleRecordID::from_raw(raw_style_record)?;
        let view = self.computed_group_sets.style_record_view(style_record.raw())?;
        Some(view.longhands())
    }

    pub(super) fn published_style_record_view(
        &self,
        style_record: Option<computed::FinalStyleRecordID>,
    ) -> Option<crate::css::computed_value_views::ComputedValuesView<'_>> {
        let payloads = self.computed_group_sets.style_record_payloads(style_record?.raw())?;
        Some(crate::css::computed_value_views::ComputedValuesView::of(payloads))
    }

    /// Record what a language atom spells, so `:lang()` can compare its ranges against the tag.
    #[cfg(test)]
    pub fn set_element_language_text(&mut self, language: StyleAtomID, text: &[u16]) {
        self.write_projection_for_test(|writes| writes.set_language_text(language, text));
    }

    /// See [`pipeline::projection::Projection::note_attribute_name_forms`].
    #[cfg(test)]
    pub fn note_attribute_name_forms(&mut self, name: StyleAtomID, forms: index::AttributeNameForms) {
        self.write_projection_for_test(|writes| writes.projection.note_attribute_name_forms(name, forms));
    }

    /// Record the id an element answers to, or clear it with atom zero.
    pub fn set_element_id_name(&mut self, node: StyleNodeID, name: StyleAtomID) {
        self.tree.set_element_id_name(node, name);
    }

    /// The first element in tree order that answers to `name` inside `tree_scope`.
    #[must_use]
    pub fn element_by_id(&self, tree_scope: TreeScopeID, name: StyleAtomID) -> Option<StyleNodeID> {
        self.tree.element_by_id(&self.projection, tree_scope, name)
    }

    /// The computed style of the element's published record, for a reader that reaches an element
    /// by identity rather than through a layout row. `None` while the element has no record.
    #[must_use]
    pub(crate) fn element_published_style(
        &self,
        node: StyleNodeID,
    ) -> Option<crate::css::computed_value_views::ComputedValuesView<'_>> {
        let record = self.published_element_record(node)?;
        let payloads = self.computed_group_sets.style_record_payloads(record.raw())?;
        Some(crate::css::computed_value_views::ComputedValuesView::of(payloads))
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

    /// Replace the ordered list of nodes a slot has assigned to it, text nodes included.
    #[cfg(test)]
    pub fn set_slot_assigned_nodes(&mut self, slot: StyleNodeID, nodes: &[StyleNodeID]) {
        self.write_projection_for_test(|writes| writes.set_slot_assigned_nodes(slot, nodes));
    }

    /// Replace the document's top layer, in the order its members were added.
    pub fn set_top_layer_elements(&mut self, members: &[StyleNodeID]) {
        self.tree.set_top_layer(members);
    }

    /// Retire text identities as their nodes disconnect. The host drops them from the slots they were assigned to.
    pub fn retire_text_style_nodes(&mut self, nodes: impl IntoIterator<Item = StyleNodeID>) {
        let nodes: Vec<StyleNodeID> = nodes.into_iter().collect();
        #[cfg(test)]
        self.write_projection_for_test(|writes| writes.projection.retire_texts(nodes.iter().copied()));
        self.tree.retire_texts(nodes);
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
        self.tree.mark_relation_only(node);
    }

    // -- Stylesheet program ------------------------------------------------------------------
    //
    // Every CSSOM mutation changes the rules it touches and no others. None of them produces a
    // generic document invalidation: the transaction names the rules that moved, and the rule
    // index tells what each of them decides.

    pub fn add_sheet(&mut self, origin: CascadeOrigin) -> SheetID {
        // Reserving an unattached sheet identity cannot change any scope's semantic program.
        self.rules.add_sheet(origin)
    }

    /// Add a sheet standing for the sheet the process published from the native sheet
    /// `source_identity`, whose rules the document reads from the process.
    pub(crate) fn add_user_agent_sheet(&mut self, source_identity: u64) -> SheetID {
        let rules = self.process_style.user_agent_rules();
        let published = rules
            .sheet_of_source(source_identity)
            .expect("the host publishes a user agent sheet before a document adds it");
        self.note_selector_summary((*rules.selector_summary(published)).clone());
        let sheet = self.rules.add_sheet(CascadeOrigin::UserAgent);
        self.rules.bind_user_agent_sheet(sheet, rules, published);
        sheet
    }

    /// Keep whether the keyframes of the animations of `node`, as the host sampled them, say
    /// `inherit` for a property that does not inherit.
    pub(crate) fn note_keyframes_read_parent_reset_values(&mut self, node: StyleNodeID, reads: bool) {
        if reads == self.keyframes_reading_parent_reset_values.contains(&node) {
            return;
        }
        match reads {
            true => self.keyframes_reading_parent_reset_values.insert(node),
            false => self.keyframes_reading_parent_reset_values.remove(&node),
        };
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
                data,
            },
        );
    }

    /// Whether the animations of a pseudo-element of `node` sampled custom properties.
    pub(crate) fn pseudo_element_samples_custom_properties(&self, node: StyleNodeID) -> bool {
        self.pseudo_element_custom_property_data
            .get(&node)
            .is_some_and(|environments| environments.iter().any(|(_, held)| held.sampled_over.is_some()))
    }

    /// Keep the custom-property environment one of an element's synthetic pseudo-elements now
    /// holds; a null `data` is none.
    ///
    /// `data`, if any, is the environment named by `identity`, and the animation overlay over the
    /// one `sampled_over` names where that is some.
    pub(crate) fn set_pseudo_element_custom_property_data(
        &mut self,
        node: StyleNodeID,
        pseudo: u8,
        data: Option<RetainedCustomPropertyData>,
        identity: u64,
        sampled_over: Option<u64>,
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
            sampled_over,
            data,
        };
        let environments = self.pseudo_element_custom_property_data.entry(node).or_default();
        match environments.iter_mut().find(|(kind, _)| *kind == pseudo) {
            Some((_, environment)) if environment.data.data() == held.data.data() => {}
            Some((_, environment)) => *environment = held,
            None => environments.push((pseudo, held)),
        }
    }

    /// Note that an element in its host's shadow tree stands for one of the host's element-backed pseudo-elements,
    /// as its published row says.
    pub fn note_backing_element(&mut self, node: StyleNodeID) {
        if let Some(host) = self.projection.written_shadow_host_of(node)
            && !self
                .backing_elements
                .get(&host)
                .is_some_and(|backing_elements| backing_elements.contains(&node))
        {
            self.backing_elements.entry(host).or_default().push(node);
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
                self.projection.shadow_host_of(node) == Some(host)
                    && self
                        .projection
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

    /// Whether any container's box is held. Size and scroll-state containers get rows, and an element that stops being
    /// one keeps its row until it goes.
    pub(crate) fn has_layout_style_snapshots(&self) -> bool {
        !self.layout_style_snapshots.is_empty()
    }

    /// Take the box geometry a completed layout commit gathered, keeping each row's scroll state, and answer the
    /// containers it resized along an axis their container types query.
    pub(super) fn take_layout_style_snapshot_rows(
        &mut self,
        rows: &[crate::layout::style_snapshot::CommittedGeometry],
    ) -> Vec<StyleNodeID> {
        let mut resized = Vec::new();
        for geometry in rows {
            let row = self.layout_style_snapshots.entry(geometry.node).or_default();
            // NB: A box the layout tree rebuilt has no earlier size for its commit to compare, and its loss said nothing
            //     of one. The row keeps the size the last box had, so what queried the container is told here, as a
            //     commit tells of a box that stayed.
            let (width, height) = (row.content_width_raw, row.content_height_raw);
            let moved = !row.has_committed_box
                && geometry.has_committed_box
                && (width != 0 || height != 0)
                && (width != geometry.content_width_raw || height != geometry.content_height_raw);
            if geometry.has_committed_box {
                row.content_width_raw = geometry.content_width_raw;
                row.content_height_raw = geometry.content_height_raw;
            }
            row.has_committed_box = geometry.has_committed_box;
            if moved && self.size_container_query_axis_moved(geometry, width, height) {
                resized.push(geometry.node);
            }
        }
        resized
    }

    /// Whether a box moved from `width` and `height` to `geometry` along an axis its element's container type queries.
    fn size_container_query_axis_moved(
        &self,
        geometry: &crate::layout::style_snapshot::CommittedGeometry,
        width: i32,
        height: i32,
    ) -> bool {
        let Some(payloads) = self
            .held_style_record(geometry.node)
            .and_then(|record| self.computed_group_sets.style_record_payloads(record))
        else {
            return false;
        };
        let view = crate::css::computed_value_views::ComputedValuesView::of(payloads);
        let size = |width_raw, height_raw| crate::layout::used_values::FfiCssPixelSize {
            width: crate::css::css_pixels::CssPixels::from_raw(width_raw),
            height: crate::css::css_pixels::CssPixels::from_raw(height_raw),
        };
        crate::layout::commit::content_size_change_affects_container_queries(
            view,
            size(width, height),
            size(geometry.content_width_raw, geometry.content_height_raw),
        )
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
        if self.counter_style_environment_identities.get(&tree_scope) != Some(&identity) {
            self.counter_style_environment_identities.insert(tree_scope, identity);
            self.note_counter_style_environment_moved();
        }
    }

    /// Record the style record an element holds, or none. Keep it, and its identity, until the host
    /// replaces it, even when the engine publishes a newer assignment. Whether the element is a query
    /// container is what that record says.
    pub(crate) fn set_held_style_record(
        &mut self,
        node: StyleNodeID,
        record: Option<std::sync::Arc<super::pipeline::Record>>,
    ) {
        let style_record = record.as_deref().map_or(0, bridge::host_record_identity);
        let previous = self.held_style_records.set(node, record);
        // NB: Font-relative size queries can change even when the container's box keeps the same dimensions.
        let font_group = crate::css::table_group_builder::group_index::FONT;
        let font_inputs_changed = self.size_container_queries.is_queried_container(node)
            && previous.is_some_and(|previous| {
                self.held_style_records.get(node).is_some_and(|current| {
                    previous.payloads[font_group].as_ptr() != current.payloads[font_group].as_ptr()
                })
            });
        self.set_element_container_query_inputs(node, style_record);
        if font_inputs_changed {
            self.size_container_queries.note_font_input_change(node);
        }
        // A `rem` the host resolves reads the font of the record it holds for the document element,
        // which it can install in the middle of a style update.
        if self.projection.adjustment_facts(node) & bridge::element_adjustment_fact::IS_DOCUMENT_ELEMENT != 0 {
            self.held_root_font_inputs = computed::FinalStyleRecordID::from_raw(style_record)
                .and_then(|record| self.root_font_inputs_from_record(record));
        }
    }

    /// Record the style record the pseudo-element of `pseudo_kind` of an element holds; zero is
    /// none. The host's reference to the record keeps its identity naming it, even when the engine
    /// publishes a newer assignment.
    pub fn set_held_pseudo_style_record(&mut self, node: StyleNodeID, pseudo_kind: u8, style_record: u64) {
        let held = self.held_pseudo_style_records.entry(node).or_default();
        let slot = held.iter().position(|&(kind, _)| kind == pseudo_kind);
        match slot {
            Some(slot) if style_record == 0 => {
                held.swap_remove(slot);
            }
            Some(slot) => held[slot].1 = style_record,
            None if style_record == 0 => {}
            None => held.push((pseudo_kind, style_record)),
        }
        if held.is_empty() {
            self.held_pseudo_style_records.remove(&node);
        }
    }

    /// The record an element is a query container as: the one the engine assigned it last, which
    /// the host installs, or else the one the host holds.
    pub(super) fn container_style_record(&self, node: StyleNodeID) -> Option<u64> {
        self.computed_group_sets
            .assigned_style_record(node)
            .map(|record| record.raw())
            .or_else(|| self.held_style_record(node))
    }

    /// Whether, and as what, an element is a query container, as its record says.
    pub(super) fn container_query_inputs(&self, node: StyleNodeID) -> Option<tree::ContainerQueryInputs<'_>> {
        self.container_query_input_row(node, self.container_style_record(node)?, false)
    }

    /// Follow whether an element answers size queries as a record it is published with says. A
    /// record without its box group makes it answer none, and so does one naming no container type.
    pub(super) fn set_element_container_query_inputs(&mut self, node: StyleNodeID, style_record: u64) {
        let is_available = self
            .container_query_input_row(node, style_record, false)
            .is_some_and(|row| row.supports_size_queries());
        self.size_container_queries.set_availability(node, is_available);
    }

    /// What a record says of its element as a query container: none for a record without its box
    /// group, nor, unless `any_element` asks for every element's, for one naming no container type
    /// or name.
    pub(super) fn container_query_input_row(
        &self,
        node: StyleNodeID,
        style_record: u64,
        any_element: bool,
    ) -> Option<tree::ContainerQueryInputs<'_>> {
        let payloads = self.computed_group_sets.style_record_payloads(style_record)?;
        self.container_query_input_row_of_payloads(node, payloads, style_record, any_element)
    }

    /// What a style's groups say of its element as a query container, as
    /// `container_query_input_row` says for a record, for a style installed as `style_record`, or
    /// as none yet.
    pub(super) fn container_query_input_row_of_payloads<'a>(
        &self,
        node: StyleNodeID,
        payloads: &'a crate::css::computed_values::StyleGroups,
        style_record: u64,
        any_element: bool,
    ) -> Option<tree::ContainerQueryInputs<'a>> {
        let values = crate::css::computed_value_views::ComputedValuesView::of(payloads);
        let box_values = values.box_values();
        if !any_element
            && !box_values.is_size_container
            && !box_values.is_inline_size_container
            && !box_values.is_scroll_state_container
            && box_values.container_name.raws().is_empty()
        {
            return None;
        }
        let names = box_values.container_name.raws();
        use super::bridge::ElementBoxKind;
        let display = values.display();
        let box_kind = self.element_box_kind(node);
        let native_box_is_suppressed = values.appearance() == crate::css::css_enums::appearance::NONE
            && box_kind.is_suppressed_by_appearance_none();
        let has_atomic_principal_box = self.element_construction_facts(node)
            & super::bridge::element_construction_fact::IS_HTML_INPUT_ELEMENT
            != 0
            || (!native_box_is_suppressed
                && matches!(
                    box_kind,
                    ElementBoxKind::FieldSet
                        | ElementBoxKind::Legend
                        | ElementBoxKind::Audio
                        | ElementBoxKind::Video
                        | ElementBoxKind::Canvas
                        | ElementBoxKind::NavigableContainerViewport
                        | ElementBoxKind::TextArea
                        | ElementBoxKind::Image
                        | ElementBoxKind::SvgSvg
                        | ElementBoxKind::SvgForeignObject
                        | ElementBoxKind::InputButton
                        | ElementBoxKind::InputCheckBox
                        | ElementBoxKind::InputRadioButton
                        | ElementBoxKind::InputRange
                        | ElementBoxKind::InputText
                ));
        let non_atomic_inline = display.is_inline_outside()
            && (display.is_ruby_inside() || (display.is_flow_inside() && !has_atomic_principal_box));
        // https://drafts.csswg.org/css-conditional-5/#size-container
        // If the query container does not have a principal box, or the principal box is not a layout containment box,
        // or the query container does not support container size queries on the relevant axes,
        // then the result of evaluating the size feature is unknown.
        let size_queries_available = box_kind != ElementBoxKind::NoBox
            && !display.is_none()
            && !display.is_contents()
            && !display.is_table_inside()
            && !(display.is_internal() && !display.is_table_caption())
            && !non_atomic_inline;
        Some(tree::ContainerQueryInputs {
            style_record,
            size_queries_available,
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
        definitions: Box<[animations::HeldAppliedAnimationDefinition]>,
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

    pub(crate) fn custom_property_registration_generation(&self) -> u64 {
        self.document_style_computation_inputs
            .custom_property_registration_generation
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

    /// Has one of an element's described effects take `timing`, the one the host runs it on without sampling it (see
    /// [`super::effect_descriptions::AnimationEffectDescriptions::refresh_timing`]).
    pub(crate) fn refresh_element_animation_effect_timing(
        &mut self,
        node: StyleNodeID,
        identity: u64,
        timing: &crate::css::style_compute::FfiEffectTiming,
    ) {
        self.animation_effect_descriptions
            .refresh_timing(node, identity, timing);
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
        host_key: Option<f64>,
    ) -> Option<f64> {
        unsafe {
            self.animation_effect_descriptions
                .time_effect(node, slot, identity, timing, easing, host_key)
        }
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
        // NB: The root is a node selectors reach and nothing publishes features for, so the host gives it a row of its
        //     own as it names it.
        #[cfg(test)]
        self.write_projection_for_test(|writes| writes.projection.ensure_facts(root));
    }

    pub(super) fn scope_root(&self, tree_scope: TreeScopeID) -> Option<StyleNodeID> {
        self.scope_roots.get(tree_scope.0 as usize).copied().flatten()
    }

    /// Record that a rule sits in a cascade layer.
    ///
    /// Said as the rule is compiled rather than as an input: which layer a rule is in is part of what
    /// the rule is, and a rule that moves between layers is recompiled.
    /// Record which longhand properties an element's presentation attributes cover, each with the value it was written
    /// with.
    ///
    /// An element-attached declaration is a cascade component above layers: a style attribute beats
    /// every layered and unlayered rule in its context, whatever layer they are in.
    pub fn set_element_declared_properties(
        &mut self,
        node: StyleNodeID,
        kind: ElementDeclarationKind,
        declarations: Vec<(DeclarationEntry, RetainedStyleValueData)>,
    ) {
        let current = self.element_declarations.declarations(node, kind).unwrap_or_default();
        if current
            .iter()
            .eq(declarations.iter().map(|(declared, written)| (declared, written)))
        {
            return;
        }
        self.element_declarations.set(node, kind, declarations);
    }
}

impl StyleEngine {
    /// Have the engine share the style data and atoms of the process, as a document's does, which the selectors it
    /// matches name their names by.
    #[cfg(test)]
    pub(super) fn share_the_process_style_for_test(&mut self) {
        let process = Arc::clone(crate::render_state::process_style());
        self.retained.atoms = crate::fork::ForkShared::new(DocumentAtoms::for_process_engine(process.atoms()));
        self.retained.process_style = process;
    }

    #[must_use]
    pub(crate) fn new() -> Self {
        let tree = HostTree::new();
        let process_style = Arc::<super::ProcessStyle>::default();
        Self {
            retained: RetainedState {
                counters: Counters::new(),

                highlight_observers: Default::default(),
                tree: crate::fork::ForkShared::new(tree),
                deepest_element: Arc::default(),
                rules: crate::fork::ForkShared::new(RuleStore::new()),
                process_style: Arc::clone(&process_style),
                declaration_block_version: Arc::new(std::sync::atomic::AtomicU32::new(1)),

                document_style_computation_inputs: Default::default(),
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
                pass_fonts: None,
                computed_group_sets: Default::default(),
                style_record_view_epoch_depth: 0,
                custom_property_environments: Default::default(),
                element_custom_property_data: Default::default(),
                pseudo_element_custom_property_data: Default::default(),
                keyframes_reading_parent_reset_values: Default::default(),
                container_effects_for_host: Default::default(),
                layout_style_snapshots: HashMap::default(),
                size_container_queries: Default::default(),
                counter_style_environment_identities: Default::default(),
                held_style_records: Default::default(),
                held_pseudo_style_records: Default::default(),
                tick_shown: Default::default(),
                backing_elements: Default::default(),
                css_defined_animations: Default::default(),
                animation_keyframes: Default::default(),
                position_try_rules: Default::default(),
                animation_effect_descriptions: Default::default(),
                hover: Default::default(),
                held_root_font_inputs: None,
                random_base_values: Default::default(),
                replaced_content_inputs: Default::default(),
                transition_baselines: Default::default(),
                custom_property_registrations_changed: false,
                demand_records: Default::default(),

                html_element_namespace: StyleAtomID::NONE,
                sibling_positions: crate::fork::ForkReset::default(),
                containers_counted_by_style_queries: Default::default(),
                cold_pass_report: [0; pipeline::ReportField::COUNT],
                live_style: None,
                live_style_may_start: crate::fork::ForkReset::original(),
                cold_passes: std::sync::Arc::default(),
                live_counters: Default::default(),
                style_layout_pool: None,
                substituted_font_values: std::sync::Arc::default(),
                element_declarations: Default::default(),
                projection: Default::default(),
                attribute_value_text_names: Arc::default(),
                tested_attribute_names: Arc::default(),
                attribute_value_text_requirements_version: 0,
                selectors_test_local_link: false,
                selectors_test_validity: false,
                relational_arrival_keys: Arc::default(),
                unresolved_relational_arrival_keys: HashSet::default(),
                relational_arrival_keys_version: 0,

                scope_roots: Default::default(),
                scope_by_root: Default::default(),
                atoms: crate::fork::ForkShared::new(DocumentAtoms::for_live_engine(process_style.atoms())),
                fold_id_and_class_name_case: false,
            },
            host: HostState {
                hover_seal: Default::default(),
                font_resolver: None,
                element_inputs: PendingElementInputs::default(),
                deferred_element_style_inputs: Default::default(),
                deferred_element_style_inputs_moved: false,
                deferred_element_style_inputs_are_pending: false,

                program_staging: ProgramStaging::default(),
                sheet_occurrences: HashMap::default(),

                sheet_rule_replacement: None,
                reclaimed_style_atoms: Vec::new(),
                defers_atom_sweep: false,
                reinherited_records: None,
                released_style_nodes: Vec::new(),
            },
        }
    }

    /// The `StyleEnvironmentChange` bit of a change that can move how any element computes,
    /// restyling every element: the quirks mode moving, which only the parser and `document.open()`
    /// do, before the document holds an element.
    const ENVIRONMENT_CHANGE_EVERYTHING: u8 = 1 << 7;

    /// Record a change to style inputs which are properties of the document environment rather
    /// than of an element or stylesheet rule: the parts `changes` names as `StyleEnvironmentChange`
    /// bits, which restyle the elements reading them, or every element where it names everything.
    pub fn record_environment_change(&mut self, changes: u8) {
        self.retained.counters.bump(Counter::RawMutationRecords);
        self.retained.counters.bump(Counter::EnvironmentDeltas);
        if changes & Self::ENVIRONMENT_CHANGE_EVERYTHING != 0 {
            self.host.element_inputs.restyle_everything();
        }
        self.host
            .element_inputs
            .note_environment_moved(super::input_types::StyleEnvironment::from_bits(changes));
    }

    #[must_use]
    pub fn has_pending_transaction(&self) -> bool {
        (self.host.deferred_element_style_inputs_are_pending && !self.host.deferred_element_style_inputs.is_empty())
            || !self.host.element_inputs.is_empty()
            || self.retained.projection.has_pending_changes()
            || self.retained.projection.has_sealed_changes()
            || self.retained.projection.has_written_links()
            || self.host.program_staging.is_dirty()
            || self.host.sheet_rule_replacement.is_some()
            // The engine restyles what moved under it other than through an input, such as
            // the children of an element whose animations' composition moved.
            || self
                .retained
                .live_style
                .as_ref()
                .is_some_and(|live| live.has_pending())
    }

    #[must_use]
    pub fn has_deferred_geometry_transaction(&self) -> bool {
        self.retained.projection.has_sealed_changes()
    }

    /// Whether any element style input is still deferred, waiting for the first transaction with a
    /// document root. A rootless flush drains the pending inputs but preserves these, so an engine that
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
        self.record_derived_element_style_input(
            node,
            input_types::STYLE_REACTION_PUBLISHED_STYLE | input_types::STYLE_REACTION_RECOMPUTE_STYLE,
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
        use crate::css::computed_value_types::FontValues;
        use crate::css::custom_properties::Utf16SliceExt;
        use crate::css::style_value::StyleValueData;
        use crate::css::table_group_builder::group_index;

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
            let font = crate::css::computed_value_views::ComputedValuesView::of(payloads).font();
            *answers.entry(std::ptr::from_ref(font)).or_insert_with(|| {
                font_lists.contains(&(font.font_cascade_list.as_raw() as u64)) || names_changed_family(font)
            })
        };
        let users: Vec<StyleNodeID> = self
            .held_style_records
            .iter()
            .map(|(node, record)| (node, bridge::host_record_identity(record)))
            .chain(
                self.computed_group_sets
                    .pseudo_style_records()
                    .map(|(node, style_record)| (node, style_record.raw())),
            )
            .filter_map(|(node, style_record)| uses_changed_font(style_record).then_some(node))
            .collect();
        // An element and its pseudo-elements may both be users: what it owes merges. A font input
        // asks for no rematch, which a reaction merged with it may still ask for.
        for node in users {
            self.record_derived_element_style_input(
                node,
                input_types::STYLE_REACTION_PUBLISHED_STYLE | input_types::STYLE_REACTION_FONT_INPUTS_CHANGED,
                1 << group_index::FONT,
            );
        }
    }

    /// Fill the faces that loaded into the frozen cascades that waited on them, which keep their
    /// identities, as a font fallback list resolves a loaded face in place: a style naming a filled
    /// cascade stays as it is, and what renders its text reads the loaded face through it. Only the
    /// styles whose values derive from the metrics of a filled cascade's first available font,
    /// their own or their parent's, are computed again; so is the document element's, where its
    /// cascade is filled, as the root font the elements reading it resolve against moves with it.
    /// The elements whose container conditions were evaluated against a container whose cascade
    /// is filled decide their rules again, as the lengths of a query resolve against the
    /// container's font.
    pub(crate) fn fill_loaded_font_faces(&mut self, fills: super::boundary::LoadedFontFaceFills) {
        use crate::css::table_group_builder::group_index;

        for fill in fills.fills {
            fill.apply();
        }
        let font_lists: HashSet<u64> = fills.font_lists.iter().copied().collect();
        let sets = &self.computed_group_sets;
        let uses_filled_font = |style_record: u64| {
            sets.style_record_payloads(style_record).is_some_and(|payloads| {
                let font = crate::css::computed_value_views::ComputedValuesView::of(payloads).font();
                font_lists.contains(&(font.font_cascade_list.as_raw() as u64))
            })
        };
        let element_uses_filled_font = |node: StyleNodeID| {
            self.held_style_record(node).is_some_and(uses_filled_font)
                || sets
                    .assigned_pseudo_kinds(node)
                    .filter_map(|kind| sets.pseudo_style_record(node, kind))
                    .any(|style_record| uses_filled_font(style_record.raw()))
        };
        let live = self.retained.live_style.as_deref();
        let projection = &self.retained.projection;
        let mut restyled = Vec::new();
        let mut requeried = Vec::new();
        for (node, _) in self.held_style_records.iter() {
            let facts = self.projection.adjustment_facts(node);
            let is_document_element = facts & super::bridge::element_adjustment_fact::IS_DOCUMENT_ELEMENT != 0;
            // NB: The line-height of a single-line input settles against the metrics of its first available font.
            let settles_line_height = facts & super::bridge::element_adjustment_fact::CHECK_INPUT_LINE_HEIGHT != 0;
            let reads_font_metrics = live.is_none_or(|live| live.reads_font_metrics(node));
            if (is_document_element || settles_line_height || reads_font_metrics)
                && (element_uses_filled_font(node)
                    || (reads_font_metrics
                        && projection
                            .inheritance_parent(node)
                            .is_some_and(element_uses_filled_font)))
            {
                restyled.push(node);
            }
            if live.is_some_and(|live| {
                live.font_metric_query_containers(node)
                    .iter()
                    .any(|&container| element_uses_filled_font(container))
            }) {
                requeried.push(node);
            }
        }
        for node in restyled {
            self.record_derived_element_style_input(
                node,
                input_types::STYLE_REACTION_PUBLISHED_STYLE | input_types::STYLE_REACTION_FONT_INPUTS_CHANGED,
                1 << group_index::FONT,
            );
        }
        for node in requeried {
            self.record_container_query_input(node);
        }
    }

    /// Record a style reaction for one element, which the engine derived itself or C++ derived
    /// from what it saw move, merged with what the element already owes. It joins the next
    /// transaction, and the engine settles it where it can.
    #[cfg(test)]
    pub fn set_shadow_root(&mut self, host: StyleNodeID, shadow_root: StyleNodeID) {
        let attached = self
            .retained
            .write_projection_for_test(|writes| writes.set_shadow_root(host, shadow_root));
        self.note_shadow_root(host, attached);
    }

    /// Write the element facts the style computation's adjustments read, as the host does, and publish them.
    #[cfg(test)]
    pub fn set_element_adjustment_facts(&mut self, node: StyleNodeID, facts: u32) {
        let moved = self
            .retained
            .write_projection_for_test(|writes| writes.set_element_adjustment_facts(node, facts));
        if moved & super::bridge::element_adjustment_fact::RENDERED_IN_TOP_LAYER != 0 {
            self.note_top_layer_move(node);
        }
    }

    /// Write the element facts a layout row built for the element records, as the host does, and publish them.
    #[cfg(test)]
    pub fn set_element_construction_facts(&mut self, node: StyleNodeID, facts: u32) {
        self.retained
            .write_projection_for_test(|writes| writes.set_element_construction_facts(node, facts));
    }

    /// Write which principal box the element asks for, as the host does, and publish it.
    #[cfg(test)]
    pub fn set_element_box_kind(&mut self, node: StyleNodeID, box_kind: super::bridge::ElementBoxKind) {
        if self
            .retained
            .write_projection_for_test(|writes| writes.set_element_box_kind(node, box_kind))
        {
            self.note_box_kind_change(node);
        }
    }

    /// The element moved into or out of the top layer, as its published host facts say.
    pub(crate) fn note_top_layer_move(&mut self, node: StyleNodeID) {
        self.record_derived_element_style_input(
            node,
            super::input_types::STYLE_REACTION_PUBLISHED_STYLE | super::input_types::STYLE_REACTION_RECOMPUTE_STYLE,
            0,
        );
        self.set_hover(self.hover_target());
    }

    /// Take what the host published of its elements at a barrier: its projection rows, the elements it published
    /// standing for a pseudo-element of their shadow host, and the declaration blocks it published them to hold. The
    /// hover's chain follows the tree where the tree moved under it.
    pub(in crate::css::style) fn adopt_host_publication(
        &mut self,
        projection: super::pipeline::projection::ProjectionPublication,
        backing_elements: &[StyleNodeID],
        declarations: &[super::bridge::FfiElementDeclarationDelta],
    ) {
        let moves_links = projection.moves_links();
        self.retained
            .element_declarations
            .adopt_inline(projection.inline_declarations.clone());
        for (counter, count) in self.retained.projection.adopt(projection) {
            self.retained.counters.add(counter, count);
        }
        for &node in backing_elements {
            self.retained.note_backing_element(node);
        }
        self.record_element_declaration_deltas(declarations);
        if moves_links {
            self.set_hover(self.hover_target());
        }
    }

    /// Note that the host wrote `host`'s shadow root, which it did not have before where `attached`.
    pub(crate) fn note_shadow_root(&mut self, host: StyleNodeID, attached: bool) {
        self.retained.tree.note_element_links_moved();
        // A host's shadow root decides which `:host` rules it matches and the elements that
        // inherit from it, which the engine restyles the host and its flat tree descendants
        // for.
        if attached && self.retained.live_style.is_some() && self.retained.tree.is_live(host) {
            self.record_derived_element_style_input(
                host,
                input_types::STYLE_REACTION_RECOMPUTE_STYLE | input_types::STYLE_REACTION_RECOMPUTE_DESCENDANT_STYLES,
                0,
            );
        }
    }

    /// Make `nodes` the elements the host shows the highlight pseudo-element of `kind` in. Those
    /// that start or stop showing it style or drop theirs.
    pub fn set_highlight_observing_elements(&mut self, kind: u8, nodes: &[StyleNodeID]) {
        let mut moved = Vec::new();
        self.retained
            .highlight_observers
            .replace(kind, nodes, |node| moved.push(node));
        for node in moved {
            self.record_derived_element_style_input(
                node,
                input_types::STYLE_REACTION_PSEUDO_INPUTS_MAY_HAVE_CHANGED,
                0,
            );
        }
    }

    pub fn record_derived_element_style_input(&mut self, node: StyleNodeID, reaction: u8, inherited_style_groups: u8) {
        if reaction == 0 {
            return;
        }
        self.defer_element_style_input(node, reaction, inherited_style_groups);
        self.host.deferred_element_style_inputs_are_pending = true;
    }

    pub fn absorb_element_style_input(&mut self, node: StyleNodeID, reaction: u8, inherited_style_groups: u8) -> u32 {
        let Some(&DeferredElementStyleInput {
            reaction: pending_reaction,
            inherited_style_groups: pending_inherited_style_groups,
        }) = self.host.deferred_element_style_inputs.get(&node)
        else {
            return 0;
        };
        // A descendant recompute asks nothing of the element its own reaction does not answer, and
        // the applied reaction carries what it asks of the descendants to the element's children.
        if pending_reaction & !reaction & !input_types::STYLE_REACTION_RECOMPUTE_DESCENDANT_STYLES != 0
            || pending_inherited_style_groups & !inherited_style_groups != 0
        {
            return 0;
        }
        self.host.deferred_element_style_inputs.remove(&node);
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
                .map(|(&node, pending)| (node, pending.reaction, pending.inherited_style_groups))
                .collect(),
        )
    }

    /// Drop the style input an element owes: C++ computed the element's style, which answers it.
    pub fn consume_element_style_input(&mut self, node: StyleNodeID) {
        if self.host.deferred_element_style_inputs.remove(&node).is_some() {
            self.host.deferred_element_style_inputs_moved = true;
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
        self.host.element_inputs.moves_environment()
            || self.retained.projection.has_written_links()
            || self.host.program_staging.is_dirty()
            || self.host.sheet_rule_replacement.is_some()
            || !self.host.deferred_element_style_inputs.is_empty()
            || self.pending_inputs_may_affect_layout_geometry()
    }

    /// Preserve the pending paint-only selector facts as the style change event established by a
    /// geometry read. Repeated reads advance the same boundary to the latest observed facts.
    pub fn defer_pending_transaction_for_geometry_read(&mut self) {
        debug_assert!(!self.retained.projection.has_written_links());
        debug_assert!(!self.host.program_staging.is_dirty());
        debug_assert!(self.host.sheet_rule_replacement.is_none());
        debug_assert!(self.host.deferred_element_style_inputs.is_empty());
        debug_assert!(self.host.element_inputs.is_empty());
        self.retained.projection.seal_for_geometry_read();
    }

    /// Make the style update sealed by a geometry read current while preserving local facts
    /// recorded after it for the following style change event.
    pub fn begin_deferred_geometry_transaction_flush(&mut self) -> bool {
        if !self.retained.projection.has_sealed_changes()
            || !self.host.element_inputs.is_empty()
            || self.retained.projection.has_written_links()
            || self.host.program_staging.is_dirty()
            || self.host.sheet_rule_replacement.is_some()
            || !self.host.deferred_element_style_inputs.is_empty()
        {
            return false;
        }
        self.retained.projection.begin_flushing_seal();
        true
    }

    /// Leave the local facts recorded after the geometry boundary to the next transaction once
    /// the sealed one has been consumed.
    pub fn end_deferred_geometry_transaction_flush(&mut self) {
        assert!(self.host.element_inputs.is_empty());
        assert!(!self.retained.projection.has_written_links());
        assert!(!self.host.program_staging.is_dirty());
        assert!(self.host.sheet_rule_replacement.is_none());
        assert!(self.host.deferred_element_style_inputs.is_empty());
        self.retained.projection.end_flushing_seal();
    }

    #[must_use]
    pub(crate) fn node_arrival_is_pending(&self, node: StyleNodeID) -> bool {
        self.retained.projection.arrival_is_pending(node)
    }

    /// Count one input, and note it for the next transaction where the projection holds no row
    /// that says it: an element's facts and relations are written into the projection, whose
    /// changes say them.
    pub(super) fn record(&mut self, key: InputKey, old: InputValue, new: InputValue) {
        let counters = &self.retained.counters;
        counters.bump(Counter::RawMutationRecords);
        if let Some(counter) = key.counter() {
            counters.bump(counter);
        }
        match (key, new) {
            (InputKey::TreeRelations(_), _) => {}
            (InputKey::LocalFeature(node, _) | InputKey::State(node, _), _) => {
                // Every fact of an arriving element is said by its arrival.
                if self.node_arrival_is_pending(node) {
                    counters.bump(Counter::ArrivingNodeFactsFolded);
                }
            }
            (InputKey::ElementDeclaration(node, _), _) => {
                if old != new {
                    self.host.element_inputs.note_declaration(node);
                }
            }
            #[cfg(test)]
            (
                InputKey::ElementStyleInput(node),
                InputValue::ElementStyleInput {
                    reaction,
                    inherited_style_groups,
                },
            ) => self.host.element_inputs.note_style_input(
                node,
                ElementStyleInput {
                    reaction,
                    inherited_style_groups,
                },
            ),
            #[cfg(test)]
            (InputKey::ElementStyleInput(_), _) => unreachable!("an element style input carries its reaction"),
            // A registration made through `CSS.registerProperty()` moves how the elements declaring
            // or reading its name compute, which the transaction taking the registry in finds.
            (InputKey::CustomPropertyRegistration(_), _) => self.host.program_staging.note_registration_change(),
        }
    }

    #[cfg(test)]
    pub(super) fn stage_first_child(&mut self, parent: StyleNodeID, child: Option<StyleNodeID>) {
        self.retained
            .write_projection_for_test(|writes| writes.stage_first_child(parent, child));
    }

    /// Raises `deepest_element` to the depth of the deepest element the tree holds, and keeps `link_rows_written` at
    /// how many elements' links were written and not settled yet, both of which the document's host reads, rather than
    /// counts of the engine's own. Nothing is written yet.
    pub(crate) fn share_tree_depth(
        &mut self,
        deepest_element: std::sync::Arc<std::sync::atomic::AtomicU32>,
        link_rows_written: std::sync::Arc<std::sync::atomic::AtomicU32>,
    ) {
        deepest_element.fetch_max(
            self.retained.deepest_element.load(std::sync::atomic::Ordering::Relaxed),
            std::sync::atomic::Ordering::Relaxed,
        );
        self.retained.deepest_element = deepest_element;
        self.retained.projection.share_link_rows_written(link_rows_written);
    }

    /// Gives a fork counts of the tree's depth of its own in place of those it shares with the host, holding what they
    /// hold now.
    pub(crate) fn detach_tree_depth_for_fork(&mut self) {
        // NB: The depth lives beside the tree, so that a fork that never writes the tree shares it whole.
        let deepest = self.retained.deepest_element.load(std::sync::atomic::Ordering::Relaxed);
        self.retained.deepest_element = Arc::new(std::sync::atomic::AtomicU32::new(deepest));
        self.retained.projection.detach_link_rows_written_for_fork();
    }

    /// Settle the relation tree at the transaction barrier: recompute the depth of what the links written into the
    /// projection since the last transaction was released moved, and retire the elements that left.
    pub(super) fn settle_tree_at_barrier(&mut self) {
        if !self.retained.projection.has_written_links() {
            return;
        }
        let mut nodes: Vec<StyleNodeID> = self.retained.projection.links_written().collect();
        nodes.sort_unstable();
        // Depth changes only for arrivals and for nodes whose parent differs from the one the last transaction
        // committed: a sibling-only write must not count as a moved parent. A moved parent's subtree walk covers its
        // moved descendants, so those are skipped below.
        let depth_recompute_nodes = self.depth_recompute_nodes(&nodes);

        self.retained.tree.note_element_links_moved();
        for &node in &nodes {
            if !depth_recompute_nodes.contains(&node) || !self.retained.tree.is_live(node) {
                continue;
            }
            let parent_is_recomputed = self
                .retained
                .projection
                .written_relations(node)
                .and_then(|relations| relations.parent)
                .is_some_and(|parent| depth_recompute_nodes.contains(&parent));
            if !parent_is_recomputed {
                let deepest = self.retained.tree.deepest_depth_under(&self.retained.projection, node);
                self.retained
                    .deepest_element
                    .fetch_max(deepest, std::sync::atomic::Ordering::Relaxed);
            }
        }
        let live_animation_overlays_before = self.retained.computed_group_sets.live_animation_overlay_records();
        let mut retired_nodes: Vec<StyleNodeID> = Vec::new();
        let mut departed: Vec<StyleNodeID> = self
            .retained
            .projection
            .relations_written()
            .filter(|&node| self.retained.projection.written_relations(node).is_none())
            .collect();
        departed.sort_unstable();
        for node in departed {
            if !self.retained.tree.is_live(node) {
                continue;
            }
            self.retained.retire_node_state(node);
            retired_nodes.push(node);
        }
        if !retired_nodes.is_empty() {
            // NB: The host retired the elements' rows as it published their departure.
            self.retained.tree.retire_elements(&retired_nodes);
            let live_animation_overlays_after = self.retained.computed_group_sets.live_animation_overlay_records();
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
        selectors: &SelectorListRef,
        scope: Option<&super::bridge::BoundScopeChain>,
    ) -> RuleID {
        let style_scope = self.scope_of_chain(scope);
        self.add_style_rule_with(sheet, before, style_scope, |engine| {
            engine.note_selectors(selectors, scope);
            selectors.clone()
        })
    }

    /// The scope a rule inside the `@scope` levels `chain`, if any, names.
    fn scope_of_chain(&mut self, chain: Option<&super::bridge::BoundScopeChain>) -> StyleScopeID {
        chain.map_or(StyleScopeID::NONE, |chain| self.retained.rules.scope_of_chain(chain))
    }

    /// Add a style rule inside the `@scope` levels `scope` names whose selectors `attach` gives it.
    pub(super) fn add_style_rule_with(
        &mut self,
        sheet: SheetID,
        before: Option<RuleID>,
        scope: StyleScopeID,
        attach: impl FnOnce(&mut Self) -> SelectorListRef,
    ) -> RuleID {
        let rule = match before {
            Some(before) => self.insert_rule_before(before, RuleKind::Style),
            None => self
                .reuse_replaced_style_rule(sheet)
                .unwrap_or_else(|| self.append_rule(sheet, None, RuleKind::Style)),
        };
        let selectors = attach(self);
        let mut version = self.current_rule_version(rule);
        version.selectors = self.retained.rules.start_of_selectors(sheet, &selectors);
        version.scope = scope;
        self.replace_rule_version(rule, version);
        self.retained.counters.bump(Counter::StyleRulesCompiled);
        rule
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
        version.set_declared_name(name);
        self.replace_rule_version(rule, version);
        rule
    }

    /// Give an existing rule a new selector list, keeping its identity and its position.
    ///
    /// Editing `selectorText` is one rule changing, not the sheet being rebuilt. The rule keeps its
    /// order token, so nothing around it is renumbered, and the transaction sees exactly one rule
    /// move.
    pub fn replace_style_rule_selectors(
        &mut self,
        rule: RuleID,
        selectors: &SelectorListRef,
        scope: Option<&super::bridge::BoundScopeChain>,
    ) {
        self.note_selectors(selectors, scope);
        let mut version = self.current_rule_version(rule);
        let sheet = self.retained.rules.rule_sheet(rule);
        version.selectors = self.retained.rules.start_of_selectors(sheet, selectors);
        version.scope = self.scope_of_chain(scope);
        self.replace_rule_version(rule, version);
        self.retained.counters.bump(Counter::StyleRulesCompiled);
    }

    /// Report that a rule's declarations moved, without touching anything else about it.
    ///
    /// The block's contents changed even where the CSSOM object did not, so what makes this a
    /// change is a version rather than the object's address. The rule keeps its identity, its
    /// position, and its selectors, so the transaction sees one rule move and restyles
    /// exactly the elements that rule matches.
    pub fn record_rule_declarations_changed(&mut self, rule: RuleID, block_version: u32) {
        let mut version = self.current_rule_version(rule);
        version.declaration_block = Some(DeclarationBlockID::new(block_version));
        self.replace_rule_version(rule, version);
    }

    /// Make the identities the host minted live, ahead of anything the host records about their
    /// nodes. Mints are batched because a call per node is exactly the boundary shape this design
    /// rules out.
    pub fn mint_style_nodes(&mut self, nodes: &[u32]) {
        for node in nodes.iter().filter_map(|&raw| StyleNodeID::from_raw(raw)) {
            if node.text_index().is_some() {
                self.retained.tree.mint_text(node);
            } else {
                self.retained.tree.mint_element(node);
                #[cfg(test)]
                self.retained.write_projection_for_test(|writes| writes.mint([node]));
                self.retained.counters.bump(Counter::StyleNodesAllocated);
            }
        }
    }

    pub fn record_input(&mut self, key: InputKey, old: InputValue, new: InputValue) {
        self.record(key, old, new);
        self.apply_to_facts_without_settling(key, new);
    }

    /// Attach a compiled program at the end of a scope's sheet order.
    pub(super) fn attach_sheet(&mut self, sheet: SheetID, tree_scope: TreeScopeID) {
        let mut sheets = self.current_sheets_in_scope(tree_scope).to_vec();
        sheets.retain(|&candidate| candidate != sheet);
        sheets.push(sheet);
        self.stage_sheets_in_scope(tree_scope, sheets);
    }

    /// Attach at a position established before the sheet finished loading. Network completion order
    /// does not determine cascade order.
    pub(super) fn attach_sheet_before(&mut self, sheet: SheetID, before: SheetID, tree_scope: TreeScopeID) {
        let mut sheets = self.current_sheets_in_scope(tree_scope).to_vec();
        sheets.retain(|&candidate| candidate != sheet);
        let position = sheets
            .iter()
            .position(|&candidate| candidate == before)
            .unwrap_or(sheets.len());
        sheets.insert(position, sheet);
        // NB: A sheet arriving in the middle is not the scope's order changing: every sheet already
        //     attached keeps the priority it had against every other, which is how the transaction
        //     tells an arrival from a move (see `staged_rule_changes`).
        self.stage_sheets_in_scope(tree_scope, sheets);
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

    /// Write a structural change into the projection. The relation tree settles it at the next
    /// transaction barrier.
    #[cfg(test)]
    pub fn record_tree_delta(&mut self, node: StyleNodeID, old: Option<TreeRelations>, new: Option<TreeRelations>) {
        self.retained
            .write_projection_for_test(|writes| writes.record_tree_delta(node, old, new));
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

    /// Record the shadow parts an element exposes, each with the shadow host it is exposed to.
    #[cfg(test)]
    pub fn set_element_parts(&mut self, node: StyleNodeID, pairs: &[(StyleAtomID, StyleNodeID)]) {
        self.retained
            .write_projection_for_test(|writes| writes.set_element_parts(node, pairs));
    }

    /// Report the outermost host a `::part()` rule can address this element from.
    #[cfg(test)]
    pub fn set_element_part_exposure(&mut self, node: StyleNodeID, exposure: StyleAtomID) {
        self.retained
            .write_projection_for_test(|writes| writes.set_element_part_exposure(node, exposure));
    }

    #[cfg(test)]
    pub fn set_element_heading_level(&mut self, node: StyleNodeID, level: u8) {
        self.retained
            .write_projection_for_test(|writes| writes.set_element_heading_level(node, level));
    }

    #[cfg(test)]
    pub fn set_element_language(&mut self, node: StyleNodeID, language: StyleAtomID) {
        self.retained
            .write_projection_for_test(|writes| writes.set_element_language(node, language));
    }
}

impl RetainedState {
    /// Drop what the engine retains for an element whose identity retires. Identities are handed
    /// out again once released, so a row left behind would describe whatever element is given the
    /// identity next. Every field is named here, so a new field fails to compile until it says
    /// what it does with the node: a table this retires a row of is bound and cleared below, and
    /// `_` is either not keyed by style node or says where its rows go instead.
    pub(super) fn retire_node_state(&mut self, node: StyleNodeID) {
        let Self {
            counters: _,
            highlight_observers,
            // Retires the whole batch at once, in `retire_elements`.
            tree: _,
            deepest_element: _,
            hover,
            rules: _,
            process_style: _,
            declaration_block_version: _,
            document_style_computation_inputs: _,
            document_resource_contexts: _,
            document_media: _,
            document_functions: _,
            custom_property_registry: _,
            font_resolution: _,
            pass_fonts: _,
            monospace_font_family: _,
            computed_group_sets,
            style_record_view_epoch_depth: _,
            custom_property_environments: _,
            element_custom_property_data,
            pseudo_element_custom_property_data,
            keyframes_reading_parent_reset_values,
            container_effects_for_host,
            layout_style_snapshots,
            size_container_queries,
            counter_style_environment_identities: _,
            held_style_records,
            held_pseudo_style_records,
            // Empty but while a clock frame runs, which no retirement reaches.
            tick_shown: _,
            backing_elements,
            css_defined_animations,
            animation_keyframes: _,
            position_try_rules: _,
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
            demand_records,
            // Forgotten for departed elements in `forget_departed_elements`.
            // Forgotten for departed elements in `forget_departed_elements`.
            // Scratch of one traversal.
            // Keyed by the tree's DOM order version.
            sibling_positions: _,
            // Too many only walk more, and an element's identity is used again.
            containers_counted_by_style_queries: _,
            // Dropped in `forget_departed_elements` when a relation holds a departed element.
            // Scratch of one transaction.
            // Not keyed by style node.
            cold_pass_report: _,
            // Keyed by style node, and only ever started by a test.
            live_style: _,
            live_style_may_start: _,
            cold_passes: _,
            // The last pass's elements, which a reader leaves out where they departed.
            live_counters: _,
            style_layout_pool: _,
            substituted_font_values: _,
            // Forgotten for departed elements in `forget_departed_elements`.
            element_declarations: _,
            // Retires the whole batch at once with the tree, in `settle_tree_at_barrier`, but for the hover overlay,
            // which lets go of the element here.
            projection,
            attribute_value_text_names: _,
            tested_attribute_names: _,
            attribute_value_text_requirements_version: _,
            selectors_test_local_link: _,
            selectors_test_validity: _,
            relational_arrival_keys: _,
            unresolved_relational_arrival_keys: _,
            relational_arrival_keys_version: _,
            // A witness is checked against the live tree before it is used.
            // Scratch of one transaction.
            // Forgotten for departed scope roots in `forget_departed_elements`.
            scope_roots: _,
            scope_by_root: _,
            atoms: _,
            html_element_namespace: _,
            fold_id_and_class_name_case: _,
        } = self;
        hover.retire(node);
        projection.retire_hovered(node);
        highlight_observers.retire(node);
        computed_group_sets.remove(node);
        element_custom_property_data.remove(&node);
        pseudo_element_custom_property_data.remove(&node);
        if keyframes_reading_parent_reset_values.contains(&node) {
            keyframes_reading_parent_reset_values.remove(&node);
        }
        // NB: A fork shares the tables below until either side writes them, so retiring an element that has no row
        //     in one writes nothing to it.
        container_effects_for_host.set(node, None);
        layout_style_snapshots.remove(&node);
        if demand_records.keys().any(|target| target.node() == node) {
            demand_records.retain(|target, record| {
                let retired = target.node() == node;
                if retired {
                    computed_group_sets.unpin_style_record(record.raw());
                }
                !retired
            });
        }
        size_container_queries.retire(node);
        if backing_elements.contains_key(&node) {
            backing_elements.remove(&node);
        }
        held_style_records.set(node, None);
        held_pseudo_style_records.remove(&node);
        css_defined_animations.retire(node);
        animation_effect_descriptions.retire(node);
        random_base_values.retire(node);
        if replaced_content_inputs.contains_key(&node) {
            replaced_content_inputs.remove(&node);
        }
        // A retired identity can name another element before the epoch commits.
        transition_baselines.remove(node);
    }
}

impl HostState {
    /// Whether `node` still owes a deferred style input.
    pub(super) fn owes_element_style_input(&self, node: StyleNodeID) -> bool {
        self.deferred_element_style_inputs.contains_key(&node)
    }
}

/// Mints the next declaration block version of `versions`.
pub(crate) fn next_declaration_block_version(versions: &std::sync::atomic::AtomicU32) -> u32 {
    versions
        .fetch_add(1, std::sync::atomic::Ordering::Relaxed)
        .checked_add(1)
        .expect("declaration revision overflow")
}
