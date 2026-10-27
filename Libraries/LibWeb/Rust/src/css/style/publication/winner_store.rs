/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Cached winner locations, with values borrowed from the committed declaration inputs.
//! Only substituted values need ownership here. A view cannot outlive its input borrow.

use super::*;
use crate::css::cascaded_properties::{CascadedValues, WinningDeclaration};
use crate::css::style_compute::{ExternalValueDependencies, external_value_dependencies};
use crate::css::style_value::{RetainedStyleValueData, StyleValueData};
use std::sync::atomic::AtomicU32;
use std::sync::atomic::Ordering;

pub(super) enum WinnerValue {
    Written {
        node: StyleNodeID,
        source: WinnerSource,
        index: usize,
    },
    /// A value substituted from what `source` declared, which keeps the sheet a `url()` in it
    /// resolves against.
    Substituted {
        value: RetainedStyleValueData,
        source: WinnerSource,
    },
}

pub(super) struct WinnerDeclaration {
    pub(super) property: u16,
    pub(super) important: bool,
    pub(super) value: WinnerValue,
    /// `external_value_dependencies(value)`, memoized, packed into one word with a bit saying
    /// it has been computed. A `Cell` would do on one thread, but a winner store is on the read
    /// side an evaluation step borrows, which has to be `Sync`. The memo is a pure function of
    /// the value, so two workers computing it and storing the same answer is not a race and
    /// relaxed ordering is enough.
    dependencies: AtomicU32,
}

/// Set on a packed `ExternalValueDependencies` word that has been computed.
const DEPENDENCIES_COMPUTED: u32 = 1 << 31;

fn pack_dependencies(dependencies: ExternalValueDependencies) -> u32 {
    let ExternalValueDependencies {
        uses_tree_counting_function,
        container_relative_length_unit_mask,
        has_unfixed_random_sharing,
        uses_random_function,
        needs_document_base_url,
        may_need_style_sheet_resource_context,
        inheritance_dependent,
    } = dependencies;
    DEPENDENCIES_COMPUTED
        | u32::from(container_relative_length_unit_mask)
        | (u32::from(uses_tree_counting_function) << 8)
        | (u32::from(has_unfixed_random_sharing) << 9)
        | (u32::from(uses_random_function) << 10)
        | (u32::from(needs_document_base_url) << 11)
        | (u32::from(may_need_style_sheet_resource_context) << 12)
        | (u32::from(inheritance_dependent) << 13)
}

fn unpack_dependencies(bits: u32) -> ExternalValueDependencies {
    ExternalValueDependencies {
        uses_tree_counting_function: bits & (1 << 8) != 0,
        container_relative_length_unit_mask: (bits & 0xff) as u8,
        has_unfixed_random_sharing: bits & (1 << 9) != 0,
        uses_random_function: bits & (1 << 10) != 0,
        needs_document_base_url: bits & (1 << 11) != 0,
        may_need_style_sheet_resource_context: bits & (1 << 12) != 0,
        inheritance_dependent: bits & (1 << 13) != 0,
    }
}

impl WinnerDeclaration {
    pub(super) fn new(property: u16, important: bool, value: WinnerValue) -> Self {
        Self {
            property,
            important,
            value,
            dependencies: AtomicU32::new(0),
        }
    }
}

/// A winner that rolled back below a `revert` or `revert-layer`, as it was declared.
#[derive(Clone, Copy)]
pub(super) struct RevertedDeclaration {
    /// The reverting declaration's priority and its index in its block.
    pub(super) position: (CascadePriority, usize),
    pub(super) operator: CascadeOperator,
}

#[derive(Default)]
pub(in crate::css::style) struct WinnerStore {
    // NB: Cascade order supplies both source slots and logical/physical tie breaking.
    declarations: Vec<WinnerDeclaration>,
    by_property: Vec<u16>,
    /// Each declaration's priority and index in its block, when known, and the declarations that
    /// reverted, by their index in `declarations`.
    positions: Vec<(CascadePriority, usize)>,
    reverted_declarations: Vec<(usize, RevertedDeclaration)>,
    /// A style query read the sibling position while substituting these declarations.
    pub(super) reads_sibling_position: bool,
}

impl WinnerStore {
    pub(super) fn new(declarations: Vec<WinnerDeclaration>) -> Self {
        let mut by_property = vec![u16::MAX; crate::css::property_metadata::NUMBER_OF_LONGHAND_PROPERTIES];
        for (index, declaration) in declarations.iter().enumerate() {
            by_property
                [usize::from(declaration.property - crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID)] =
                u16::try_from(index).expect("winner count exceeds longhand count");
        }
        Self {
            declarations,
            by_property,
            positions: Vec::new(),
            reverted_declarations: Vec::new(),
            reads_sibling_position: false,
        }
    }

    pub(super) fn set_reverted_declarations(
        &mut self,
        positions: Vec<(CascadePriority, usize)>,
        reverted_declarations: Vec<(usize, RevertedDeclaration)>,
    ) {
        debug_assert_eq!(positions.len(), self.declarations.len());
        // NB: The positions only order declarations against a revert, so a store without one keeps none.
        if reverted_declarations.is_empty() {
            return;
        }
        self.positions = positions;
        self.reverted_declarations = reverted_declarations;
    }

    fn reverted_declaration(&self, index: usize) -> Option<&RevertedDeclaration> {
        self.reverted_declarations
            .iter()
            .find_map(|(reverted_index, reverted)| (*reverted_index == index).then_some(reverted))
    }

    /// https://drafts.csswg.org/css-logical/#box
    /// Whether the declaration at `reverting` reverted past the one at `other`, its counterpart in a
    /// logical property group: declared later than `other`, the revert leaves the group's value as if
    /// `other` were not declared either.
    fn reverts_past_counterpart(&self, reverting: usize, other: usize) -> bool {
        let Some(reverted) = self.reverted_declaration(reverting) else {
            return false;
        };
        if self.reverted_declaration(other).is_some() {
            return false;
        }
        let Some(other_position) = self.positions.get(other) else {
            return false;
        };
        reverted.position > *other_position && reverted.position.0.reverts_past(reverted.operator, &other_position.0)
    }

    /// NB: Compare the resolved declarations while retaining context-dependent computation guards.
    pub(super) fn environment_value_delta(&self, other: &Self, engine: &RetainedState) -> Option<SmallVec<[u16; 8]>> {
        if self.reads_sibling_position || other.reads_sibling_position {
            return None;
        }
        if self.declarations.len() != other.declarations.len() {
            return None;
        }
        let mine = self.view(engine);
        let theirs = other.view(engine);
        let mut changed = SmallVec::new();
        for (old, new) in self.declarations.iter().zip(&other.declarations) {
            if old.property != new.property || old.important != new.important {
                return None;
            }
            for dependencies in [mine.dependencies(old), theirs.dependencies(new)] {
                if value_resolves_against_the_element(&dependencies)
                    || dependencies.uses_tree_counting_function
                    || dependencies.may_need_style_sheet_resource_context
                    || dependencies.needs_document_base_url
                {
                    return None;
                }
            }
            if mine.value(old) != theirs.value(new) {
                changed.push(old.property);
            }
        }
        Some(changed)
    }

    #[cfg(test)]
    fn environment_values_equal(&self, other: &Self, engine: &RetainedState) -> bool {
        self.environment_value_delta(other, engine)
            .is_some_and(|properties| properties.is_empty())
    }

    pub(super) fn uses_tree_counting_function(&self, engine: &RetainedState) -> bool {
        let view = self.view(engine);
        self.reads_sibling_position
            || self
                .declarations
                .iter()
                .any(|declaration| view.dependencies(declaration).uses_tree_counting_function)
    }

    /// Whether a substitution made a non-inherited longhand `inherit`, which its written value
    /// does not show: the value is then read from the parent's record, not its inherited style.
    pub(super) fn substitutes_inherit_for_non_inherited_property(&self, engine: &RetainedState) -> bool {
        let view = self.view(engine);
        self.declarations.iter().any(|declaration| {
            matches!(declaration.value, WinnerValue::Substituted { .. })
                && declaration.property >= crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID
                && !crate::css::property_metadata::property_is_inherited(declaration.property)
                && matches!(view.value(declaration).data(), StyleValueData::Keyword { keyword }
                    if *keyword == crate::css::style_compute::keyword::INHERIT)
        })
    }

    /// The proof that no value the store holds resolves a `url()`, or `None` when one may.
    pub(super) fn reads_no_resource_contexts(&self, engine: &RetainedState) -> Option<ReadsNoResourceContexts> {
        let view = self.view(engine);
        (!self
            .declarations
            .iter()
            .any(|declaration| view.dependencies(declaration).may_need_style_sheet_resource_context))
        .then_some(ReadsNoResourceContexts(()))
    }

    /// What a record computed from the store reads beside its winners.
    pub(super) fn record_reads(&self, engine: &RetainedState) -> StateRecordReads {
        StateRecordReads {
            sibling_position: self.uses_tree_counting_function(engine),
            no_resource_contexts: self.reads_no_resource_contexts(engine),
        }
    }

    /// Whether a value the store holds resolves a container-relative length, which the drive
    /// resolves against the element's query containers.
    pub(super) fn reads_container_units(&self, engine: &RetainedState) -> bool {
        let view = self.view(engine);
        self.declarations
            .iter()
            .any(|declaration| view.dependencies(declaration).container_relative_length_unit_mask != 0)
    }

    pub(super) fn view<'a>(&'a self, engine: &'a RetainedState) -> WinnerView<'a> {
        WinnerView { store: self, engine }
    }

    fn declaration(&self, property: u16) -> Option<(usize, &WinnerDeclaration)> {
        let index = usize::from(*self.by_property.get(usize::from(
            property.checked_sub(crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID)?,
        ))?);
        self.declarations.get(index).map(|declaration| (index, declaration))
    }
}

pub(super) struct WinnerView<'a> {
    store: &'a WinnerStore,
    engine: &'a RetainedState,
}

pub(super) fn shorthand_longhand_data(property: u16, data: &StyleValueData) -> Option<&RetainedStyleValueData> {
    let StyleValueData::Shorthand {
        sub_properties, values, ..
    } = data
    else {
        return None;
    };
    for (&sub_property, sub_value) in sub_properties.as_slice().iter().zip(values.as_slice()) {
        if sub_property == property {
            return Some(sub_value);
        }
        if let Some(found) = shorthand_longhand_data(property, sub_value.data()) {
            return Some(found);
        }
    }
    None
}

impl WinnerView<'_> {
    fn value<'a>(&'a self, declaration: &'a WinnerDeclaration) -> &'a RetainedStyleValueData {
        let written = match &declaration.value {
            WinnerValue::Substituted { value, .. } => return value,
            WinnerValue::Written { node, source, index } => match source {
                WinnerSource::Rule(rule) => &self.engine.program.written_values_of(*rule)[*index],
                WinnerSource::Element(kind) => self
                    .engine
                    .facts
                    .element_declarations(*node, *kind)
                    .expect("a written winner's declarations are held")
                    .written(*index),
                WinnerSource::ExactCascade => unreachable!("a winner recipe requires original spelling"),
            },
        };
        match written.data() {
            data @ StyleValueData::Shorthand { .. } => {
                shorthand_longhand_data(declaration.property, data).expect("validated shorthand winner")
            }
            _ => written,
        }
    }
}

impl WinnerStore {
    /// The resource context of each declaration, in cascade order, as the drive reads a winner's
    /// source slot. Empty, and no allocation, when no declaration may read one.
    pub(super) fn drive_resource_contexts(
        &self,
        engine: &RetainedState,
    ) -> Vec<crate::css::style_compute::FfiStyleSheetResourceContext> {
        if self.reads_no_resource_contexts(engine).is_some() {
            return Vec::new();
        }
        let view = self.view(engine);
        self.declarations
            .iter()
            .map(|declaration| {
                view.resource_context(declaration).map_or(
                    crate::css::style_compute::FfiStyleSheetResourceContext::empty(),
                    |context| context.as_drive_context(),
                )
            })
            .collect()
    }
}

impl WinnerStore {
    /// The random sharings of the declarations' random functions whose random base value is not
    /// fixed, as the drive finds their base values: by pointer.
    fn unfixed_random_sharings(&self, engine: &RetainedState) -> Vec<*const StyleValueData> {
        let view = self.view(engine);
        let mut sharings = Vec::new();
        for declaration in &self.declarations {
            if view.dependencies(declaration).has_unfixed_random_sharing {
                crate::css::style_compute::collect_unfixed_random_sharings_in_value(
                    view.value(declaration).data(),
                    &mut sharings,
                );
            }
        }
        sharings
    }

    /// Draw the random base value of every random function in the declarations that has not got
    /// one yet, for a drive of `node`'s record.
    pub(super) fn draw_random_base_values(&self, engine: &mut RetainedState, node: StyleNodeID) {
        for sharing in self.unfixed_random_sharings(engine) {
            // SAFETY: The declarations retain every value a sharing is part of.
            let (name, element_shared) = crate::css::style_compute::random_caching_key(unsafe { &*sharing });
            engine.ensure_random_base_value(Some(node), name, element_shared);
        }
    }

    /// The random base values of the declarations' random functions for a drive of `node`'s
    /// record. Empty, and no allocation, when they hold none; `None` when one was not drawn for
    /// the node.
    pub(super) fn drive_random_base_values(
        &self,
        engine: &RetainedState,
        node: StyleNodeID,
    ) -> Option<Vec<crate::css::style_compute::FfiRandomBaseValue>> {
        self.unfixed_random_sharings(engine)
            .into_iter()
            .map(|sharing| {
                // SAFETY: The declarations retain every value a sharing is part of.
                let (name, element_shared) = crate::css::style_compute::random_caching_key(unsafe { &*sharing });
                Some(crate::css::style_compute::FfiRandomBaseValue {
                    source: sharing.cast(),
                    value: engine.random_base_values.get(node, name, element_shared)?,
                })
            })
            .collect()
    }
}

impl<'a> WinnerView<'a> {
    fn dependencies(&self, declaration: &WinnerDeclaration) -> ExternalValueDependencies {
        let memoized = declaration.dependencies.load(Ordering::Relaxed);
        if memoized & DEPENDENCIES_COMPUTED != 0 {
            return unpack_dependencies(memoized);
        }
        let dependencies = external_value_dependencies(self.value(declaration).data());
        declaration
            .dependencies
            .store(pack_dependencies(dependencies), Ordering::Relaxed);
        dependencies
    }

    /// The resource context of the sheet a rule declaration came from, written or substituted. An
    /// element's own declarations resolve against the document's base URL, as they do in the
    /// host's cascade.
    fn resource_context(
        &self,
        declaration: &WinnerDeclaration,
    ) -> Option<&'a super::resource_contexts::StyleSheetResourceContext> {
        let (WinnerValue::Written {
            source: WinnerSource::Rule(rule),
            ..
        }
        | WinnerValue::Substituted {
            source: WinnerSource::Rule(rule),
            ..
        }) = declaration.value
        else {
            return None;
        };
        let engine: &'a RetainedState = self.engine;
        engine
            .document_resource_contexts
            .for_source(engine.rule_source_identity(rule)?)
    }
}

impl CascadedValues for WinnerView<'_> {
    fn winning_declaration(&self, property: u16) -> Option<WinningDeclaration<'_>> {
        let (index, declaration) = self.store.declaration(property)?;
        Some((
            self.value(declaration),
            declaration.important,
            u32::try_from(index).unwrap(),
            self.resource_context(declaration).is_some(),
            self.dependencies(declaration),
        ))
    }

    fn winning_origin(&self, property: u16) -> Option<CascadeOrigin> {
        self.store.declaration(property).map(|_| CascadeOrigin::Author)
    }

    fn property_with_higher_priority(&self, first: u16, second: u16) -> u16 {
        match (self.store.declaration(first), self.store.declaration(second)) {
            (Some((first_index, _)), Some((second_index, _)))
                if self.store.reverts_past_counterpart(first_index, second_index) =>
            {
                first
            }
            (Some((first_index, _)), Some((second_index, _)))
                if self.store.reverts_past_counterpart(second_index, first_index) =>
            {
                second
            }
            (Some((first_index, _)), Some((second_index, _))) if first_index >= second_index => first,
            (Some(_), None) => first,
            _ => second,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::property_metadata::property_id;
    use std::sync::Arc;

    fn length_store(value: f64, unit: &str) -> WinnerStore {
        let unit = u8::try_from(
            crate::css::style_compute::LENGTH_UNIT_NAMES
                .iter()
                .position(|&name| name == unit)
                .unwrap(),
        )
        .unwrap();
        WinnerStore::new(vec![WinnerDeclaration::new(
            property_id::WIDTH,
            false,
            WinnerValue::Substituted {
                value: RetainedStyleValueData::from_owned(StyleValueData::Length { value, unit }),
                source: WinnerSource::Element(ElementDeclarationKind::InlineStyle),
            },
        )])
    }

    #[test]
    fn environment_value_comparison_checks_resolved_values() {
        let engine = StyleEngine::new();
        let first = length_store(10.0, "px");
        let equal = length_store(10.0, "px");
        let changed = length_store(20.0, "px");
        assert!(first.environment_values_equal(&equal, &engine.retained));
        assert!(equal.environment_values_equal(&first, &engine.retained));
        assert!(!first.environment_values_equal(&changed, &engine.retained));
        assert_eq!(
            first.environment_value_delta(&changed, &engine.retained).as_deref(),
            Some([property_id::WIDTH].as_slice())
        );
        assert!(!first.environment_values_equal(&WinnerStore::new(Vec::new()), &engine.retained));
    }

    #[test]
    fn environment_value_comparison_leaves_font_context_to_driver_checks() {
        let engine = StyleEngine::new();
        assert!(length_store(1.0, "em").environment_values_equal(&length_store(1.0, "em"), &engine.retained));
    }

    #[test]
    fn environment_value_comparison_rejects_container_relative_values() {
        let engine = StyleEngine::new();
        assert!(!length_store(1.0, "cqw").environment_values_equal(&length_store(1.0, "cqw"), &engine.retained));
    }

    #[test]
    fn environment_value_comparison_only_reuses_urls_without_resource_dependencies() {
        use crate::css::style_value::{ImageResourceContext, RetainedRequestUrlModifierList, RetainedString};

        let store = |property, value| {
            WinnerStore::new(vec![WinnerDeclaration::new(
                property,
                false,
                WinnerValue::Substituted {
                    value: RetainedStyleValueData::from_owned(value),
                    source: WinnerSource::Element(ElementDeclarationKind::InlineStyle),
                },
            )])
        };
        let engine = StyleEngine::new();
        for (url, can_reuse) in [
            ("https://example.com/paint.svg#fill", true),
            ("data:image/svg+xml,paint", true),
            ("paint.svg#fill", false),
            ("/paint.svg#fill", false),
            ("//example.com/paint.svg#fill", false),
        ] {
            let url_value = || StyleValueData::Url {
                url: RetainedString::from_ascii(url.to_owned()),
                url_type: 0,
                modifiers: RetainedRequestUrlModifierList::from_retained_modifiers(Vec::new()),
            };
            let first = store(property_id::FILL, url_value());
            let equal = store(property_id::FILL, url_value());
            assert_eq!(
                first.environment_values_equal(&equal, &engine.retained),
                can_reuse,
                "{url}"
            );
            assert_eq!(
                first.reads_no_resource_contexts(&engine.retained).is_some(),
                can_reuse,
                "{url}"
            );
        }

        let image_value = |base_url: &str, origin_clean| StyleValueData::Image {
            url: RetainedString::from_ascii("https://example.com/image.png".to_owned()),
            url_type: 0,
            url_modifiers: RetainedRequestUrlModifierList::from_retained_modifiers(Vec::new()),
            resource_context: ImageResourceContext {
                base_url: RetainedString::from_ascii(base_url.to_owned()),
                has_base_url: true,
                has_parent_style_sheet_origin_clean: true,
                parent_style_sheet_origin_clean: origin_clean,
                should_absolutize_url_for_computed_value: true,
            },
        };
        let first = store(
            property_id::BACKGROUND_IMAGE,
            image_value("https://example.com/first.css", true),
        );
        let equal = store(
            property_id::BACKGROUND_IMAGE,
            image_value("https://example.com/second.css", false),
        );
        assert!(!first.environment_values_equal(&equal, &engine.retained));
        assert!(first.reads_no_resource_contexts(&engine.retained).is_none());
    }

    #[test]
    fn winner_view_borrows_written_spelling_and_preserves_cascade_order() {
        let mut engine = StyleEngine::new();
        let node = StyleNodeID::element(1);
        let kind = ElementDeclarationKind::InlineStyle;
        let properties = [property_id::MARGIN_LEFT, property_id::MARGIN_INLINE_START];
        let written = [1.25, 2.5].map(|value| RetainedStyleValueData::from_owned(StyleValueData::Number { value }));
        let mut owned = CascadedPropertyStore::new();
        for (&property, value) in properties.iter().zip(&written) {
            owned.seed_retained_property(property, value.clone_retained(), true, false);
        }
        let observer = written[0].clone_retained().into_arc();
        engine.facts.set_element_declared_properties(
            node,
            kind,
            properties
                .iter()
                .map(|&property| DeclaredProperty {
                    property,
                    important: true,
                    operator: CascadeOperator::Declared,
                    // NB: The identity does not supply the written value to the drive.
                    value: SpecifiedValueID(1),
                })
                .zip(written)
                .collect(),
        );
        let owners = Arc::strong_count(&observer);
        let store = WinnerStore::new(
            properties
                .iter()
                .enumerate()
                .map(|(index, &property)| {
                    WinnerDeclaration::new(
                        property,
                        true,
                        WinnerValue::Written {
                            node,
                            source: WinnerSource::Element(kind),
                            index,
                        },
                    )
                })
                .collect(),
        );
        let view = store.view(&engine);
        for property in properties {
            let actual = view.winning_declaration(property).unwrap();
            let expected = owned.winning_declaration(property).unwrap();
            assert_eq!(
                (actual.0.pointer(), actual.1, actual.2, actual.3),
                (expected.0.pointer(), expected.1, expected.2, expected.3)
            );
            assert!(matches!(view.winning_origin(property), Some(CascadeOrigin::Author)));
        }
        assert_eq!(
            view.property_with_higher_priority(properties[0], properties[1]),
            properties[1]
        );
        assert_eq!(
            view.property_with_higher_priority(properties[1], properties[0]),
            properties[1]
        );
        assert!(view.winning_declaration(property_id::OPACITY).is_none());
        assert_eq!(Arc::strong_count(&observer), owners);
        drop(store);
        assert_eq!(Arc::strong_count(&observer), owners);
    }
}
