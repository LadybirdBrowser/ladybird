/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The winning declarations of one element's drive, with values borrowed from the committed
//! declaration inputs. Only substituted values need ownership here. A view cannot outlive its
//! input borrow.

use super::super::*;
use crate::css::cascaded_properties::{CascadedValues, WinningDeclaration};
use crate::css::style_compute::{ExternalValueDependencies, RootFontReads};
use crate::css::style_value::ShorthandPayload;
use crate::css::style_value::{RetainedStyleValueData, StyleValueData};
use std::cell::Cell;

pub(in crate::css::style) enum WinnerValue {
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

pub(in crate::css::style) struct WinnerDeclaration {
    pub(in crate::css::style) property: u16,
    pub(in crate::css::style) important: bool,
    pub(in crate::css::style) value: WinnerValue,
    /// The origin of the winning declaration.
    pub(in crate::css::style) origin: CascadeOrigin,
    /// `external_value_dependencies(value)`, memoized, packed into one word with a bit saying
    /// it has been computed.
    dependencies: Cell<u32>,
}

/// Set on a packed `ExternalValueDependencies` word that has been computed.
pub(in crate::css::style) const DEPENDENCIES_COMPUTED: u32 = 1 << 31;

/// The bits a packed `ExternalValueDependencies` word may set: those `pack_dependencies` sets.
pub(in crate::css::style) const DEPENDENCY_BITS: u32 = DEPENDENCIES_COMPUTED | ((1 << 21) - 1);

pub(in crate::css::style) fn pack_dependencies(dependencies: ExternalValueDependencies) -> u32 {
    let ExternalValueDependencies {
        uses_tree_counting_function,
        container_relative_length_unit_mask,
        has_unfixed_random_sharing,
        uses_random_function,
        needs_document_base_url,
        may_need_style_sheet_resource_context,
        inheritance_dependent,
        root_font_reads,
        font_metric_relative_length,
        color_scheme_dependent,
        device_pixel_ratio_dependent,
        viewport_axes,
    } = dependencies;
    DEPENDENCIES_COMPUTED
        | u32::from(container_relative_length_unit_mask)
        | (u32::from(uses_tree_counting_function) << 8)
        | (u32::from(has_unfixed_random_sharing) << 9)
        | (u32::from(uses_random_function) << 10)
        | (u32::from(needs_document_base_url) << 11)
        | (u32::from(may_need_style_sheet_resource_context) << 12)
        | (u32::from(inheritance_dependent) << 13)
        | (u32::from(root_font_reads as u8) << 14)
        | (u32::from(color_scheme_dependent) << 17)
        | (u32::from(device_pixel_ratio_dependent) << 18)
        | (u32::from(viewport_axes) << 19)
        | (u32::from(font_metric_relative_length) << 16)
}

pub(in crate::css::style) fn unpack_dependencies(bits: u32) -> ExternalValueDependencies {
    ExternalValueDependencies {
        uses_tree_counting_function: bits & (1 << 8) != 0,
        container_relative_length_unit_mask: (bits & 0xff) as u8,
        has_unfixed_random_sharing: bits & (1 << 9) != 0,
        uses_random_function: bits & (1 << 10) != 0,
        needs_document_base_url: bits & (1 << 11) != 0,
        may_need_style_sheet_resource_context: bits & (1 << 12) != 0,
        inheritance_dependent: bits & (1 << 13) != 0,
        root_font_reads: match (bits >> 14) & 0b11 {
            0 => RootFontReads::None,
            1 => RootFontReads::FontSize,
            _ => RootFontReads::Metrics,
        },
        font_metric_relative_length: bits & (1 << 16) != 0,
        color_scheme_dependent: bits & (1 << 17) != 0,
        device_pixel_ratio_dependent: bits & (1 << 18) != 0,
        viewport_axes: ((bits >> 19) & 0b11) as u8,
    }
}

impl WinnerDeclaration {
    pub(in crate::css::style) fn new(
        property: u16,
        important: bool,
        value: WinnerValue,
        origin: CascadeOrigin,
    ) -> Self {
        Self {
            property,
            important,
            value,
            origin,
            dependencies: Cell::new(0),
        }
    }
}

#[derive(Default)]
pub(in crate::css::style) struct WinnerStore {
    // NB: Cascade order supplies both source slots and logical/physical tie breaking.
    declarations: Vec<WinnerDeclaration>,
    by_property: Vec<u16>,
}

impl WinnerStore {
    pub(in crate::css::style) fn new(declarations: Vec<WinnerDeclaration>) -> Self {
        let mut by_property = vec![u16::MAX; crate::css::property_metadata::NUMBER_OF_LONGHAND_PROPERTIES];
        for (index, declaration) in declarations.iter().enumerate() {
            by_property
                [usize::from(declaration.property - crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID)] =
                u16::try_from(index).expect("winner count exceeds longhand count");
        }
        Self {
            declarations,
            by_property,
        }
    }

    /// Whether no value the store holds resolves a `url()` against a style sheet's or the
    /// document's base URL. The engine keeps only such records across transactions, so a move of
    /// those base URLs, which every `history.pushState()` makes without a `<base>`, leaves nothing
    /// it keeps to forget.
    pub(in crate::css::style) fn reads_no_resource_contexts(&self, engine: &RetainedState) -> bool {
        let view = self.view(engine);
        !self
            .declarations
            .iter()
            .any(|declaration| view.dependencies(declaration).may_need_style_sheet_resource_context)
    }

    /// The container-relative units the values the store holds resolve, as a
    /// `container_relative_length_unit_mask`.
    pub(in crate::css::style) fn container_unit_mask(&self, engine: &RetainedState) -> u8 {
        let view = self.view(engine);
        self.declarations.iter().fold(0, |mask, declaration| {
            mask | view.dependencies(declaration).container_relative_length_unit_mask
        })
    }

    pub(in crate::css::style) fn view<'a>(&'a self, engine: &'a RetainedState) -> WinnerView<'a> {
        WinnerView { store: self, engine }
    }

    fn declaration(&self, property: u16) -> Option<(usize, &WinnerDeclaration)> {
        let index = usize::from(*self.by_property.get(usize::from(
            property.checked_sub(crate::css::property_metadata::FIRST_LONGHAND_PROPERTY_ID)?,
        ))?);
        self.declarations.get(index).map(|declaration| (index, declaration))
    }
}

pub(in crate::css::style) struct WinnerView<'a> {
    store: &'a WinnerStore,
    engine: &'a RetainedState,
}

pub(in crate::css::style) fn shorthand_longhand_data(
    property: u16,
    data: &StyleValueData,
) -> Option<&RetainedStyleValueData> {
    let StyleValueData::Shorthand(shorthand_payload) = data else {
        return None;
    };
    let ShorthandPayload {
        sub_properties, values, ..
    } = &**shorthand_payload;
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
                WinnerSource::Rule(rule) => &self.engine.rules.written_values_of(*rule)[*index],
                WinnerSource::Element(kind) => self
                    .engine
                    .element_declarations
                    .declarations(*node, *kind)
                    .expect("a written winner's declarations are held")
                    .written(*index),
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
    pub(in crate::css::style) fn drive_resource_contexts(
        &self,
        engine: &RetainedState,
    ) -> Vec<crate::css::style_compute::FfiStyleSheetResourceContext> {
        if self.reads_no_resource_contexts(engine) {
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

    /// The random caching key of every random function in the declarations that has not got a
    /// base value yet, for a drive of `node`'s record.
    pub(in crate::css::style) fn undrawn_random_keys(
        &self,
        engine: &RetainedState,
        node: StyleNodeID,
    ) -> Vec<crate::css::style::random_bases::RandomBaseRequest> {
        self.unfixed_random_sharings(engine)
            .into_iter()
            .filter_map(|sharing| {
                // SAFETY: The declarations retain every value a sharing is part of.
                let (name, element_shared) = crate::css::style_compute::random_caching_key(unsafe { &*sharing });
                engine
                    .drawn_random_base_value(node, name, element_shared)
                    .is_none()
                    .then(|| crate::css::style::random_bases::RandomBaseRequest {
                        node,
                        name: name.into(),
                        element_shared,
                    })
            })
            .collect()
    }

    /// The random base values of the declarations' random functions for a drive of `node`'s
    /// record. Empty, and no allocation, when they hold none; `None` when one was not drawn for
    /// the node.
    pub(in crate::css::style) fn drive_random_base_values(
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
        let memoized = declaration.dependencies.get();
        if memoized & DEPENDENCIES_COMPUTED != 0 {
            return unpack_dependencies(memoized);
        }
        // A written value's dependencies were prepared with the declaration it was written in.
        if let WinnerValue::Written { node, source, index } = &declaration.value {
            let prepared = match source {
                WinnerSource::Rule(rule) => self.engine.rules.written_value_checks(*rule, *index).dependencies(),
                WinnerSource::Element(kind) => self
                    .engine
                    .element_declarations
                    .declarations(*node, *kind)
                    .map_or(0, |declarations| declarations.checks(*index).dependencies()),
            };
            if prepared & DEPENDENCIES_COMPUTED != 0 {
                declaration.dependencies.set(prepared);
                return unpack_dependencies(prepared);
            }
        }
        // A value whose independence is not known reads its parent, conservatively.
        let dependencies = crate::css::style_compute::try_external_value_dependencies(self.value(declaration).data())
            .unwrap_or(ExternalValueDependencies {
                inheritance_dependent: true,
                ..ExternalValueDependencies::default()
            });
        declaration.dependencies.set(pack_dependencies(dependencies));
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
        Some(self.store.declaration(property)?.1.origin)
    }

    fn property_with_higher_priority(&self, first: u16, second: u16) -> u16 {
        match (self.store.declaration(first), self.store.declaration(second)) {
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
    use crate::css::style_value::{ImagePayload, StyleValueBox, UrlPayload};

    #[test]
    fn only_urls_without_resource_dependencies_read_no_resource_contexts() {
        use crate::css::style_value::{ImageResourceContext, RetainedRequestUrlModifierList, RetainedString};

        let store = |property, value| {
            WinnerStore::new(vec![WinnerDeclaration::new(
                property,
                false,
                WinnerValue::Substituted {
                    value: RetainedStyleValueData::from_owned(value),
                    source: WinnerSource::Element(ElementDeclarationKind::InlineStyle),
                },
                CascadeOrigin::Author,
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
            let url_value = || {
                StyleValueData::Url(StyleValueBox::new(UrlPayload {
                    url: RetainedString::from_ascii(url.to_owned()),
                    url_type: 0,
                    modifiers: RetainedRequestUrlModifierList::from_vec(Vec::new()),
                }))
            };
            let first = store(property_id::FILL, url_value());
            assert_eq!(first.reads_no_resource_contexts(&engine.retained), can_reuse, "{url}");
        }

        let image_value = |base_url: &str, origin_clean| {
            StyleValueData::Image(StyleValueBox::new(ImagePayload {
                url: RetainedString::from_ascii("https://example.com/image.png".to_owned()),
                url_type: 0,
                url_modifiers: RetainedRequestUrlModifierList::from_vec(Vec::new()),
                resource_context: ImageResourceContext {
                    base_url: RetainedString::from_ascii(base_url.to_owned()),
                    has_base_url: true,
                    has_parent_style_sheet_origin_clean: true,
                    parent_style_sheet_origin_clean: origin_clean,
                    should_absolutize_url_for_computed_value: true,
                },
            }))
        };
        let first = store(
            property_id::BACKGROUND_IMAGE,
            image_value("https://example.com/first.css", true),
        );
        assert!(!first.reads_no_resource_contexts(&engine.retained));
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
        engine.element_declarations.set_inline_for_test(
            node,
            properties
                .iter()
                .map(|&property| DeclarationEntry::new(property, true, CascadeOperator::Declared))
                .zip(written)
                .collect(),
        );
        let owners = crate::css::style_value_arc::StyleValueArc::strong_count(&observer);
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
                        CascadeOrigin::Author,
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
        assert_eq!(
            crate::css::style_value_arc::StyleValueArc::strong_count(&observer),
            owners
        );
        drop(store);
        assert_eq!(
            crate::css::style_value_arc::StyleValueArc::strong_count(&observer),
            owners
        );
    }
}
