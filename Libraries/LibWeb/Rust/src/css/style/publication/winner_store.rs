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
    Substituted(RetainedStyleValueData),
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

#[derive(Default)]
pub(in crate::css::style) struct WinnerStore {
    // NB: Cascade order supplies both source slots and logical/physical tie breaking.
    declarations: Vec<WinnerDeclaration>,
    by_property: Vec<u16>,
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
        }
    }

    pub(super) fn capacity_bytes(&self) -> u64 {
        (size_of::<Self>()
            + 2 * size_of::<usize>()
            + self.declarations.capacity() * size_of::<WinnerDeclaration>()
            + self.by_property.capacity() * size_of::<u16>()) as u64
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

pub(super) fn shorthand_longhand_data(property: u16, data: &StyleValueData) -> Option<&StyleValueData> {
    let StyleValueData::Shorthand {
        sub_properties, values, ..
    } = data
    else {
        return None;
    };
    for (&sub_property, sub_value) in sub_properties.as_slice().iter().zip(values.as_slice()) {
        // SAFETY: The shorthand owns every nested value for this borrow.
        let sub_data = unsafe { &*sub_value.pointer().cast::<StyleValueData>() };
        if sub_property == property {
            return Some(sub_data);
        }
        if let Some(found) = shorthand_longhand_data(property, sub_data) {
            return Some(found);
        }
    }
    None
}

impl WinnerView<'_> {
    fn value<'a>(&'a self, declaration: &'a WinnerDeclaration) -> &'a StyleValueData {
        let written = match &declaration.value {
            WinnerValue::Substituted(value) => return value.data(),
            WinnerValue::Written { node, source, index } => match source {
                WinnerSource::Rule(rule) => &self.engine.program.written_values_of(*rule)[*index],
                WinnerSource::Element(kind) => &self.engine.facts.element_written_declared_values(*node, *kind)[*index],
                WinnerSource::ExactCascade => unreachable!("a winner recipe requires original spelling"),
            },
        };
        match written.data() {
            data @ StyleValueData::Shorthand { .. } => {
                shorthand_longhand_data(declaration.property, data).expect("validated shorthand winner")
            }
            data => data,
        }
    }
}

impl CascadedValues for WinnerView<'_> {
    fn winning_declaration(&self, property: u16) -> Option<WinningDeclaration> {
        let (index, declaration) = self.store.declaration(property)?;
        let value = self.value(declaration);
        let memoized = declaration.dependencies.load(Ordering::Relaxed);
        let dependencies = if memoized & DEPENDENCIES_COMPUTED == 0 {
            let dependencies = external_value_dependencies(value);
            declaration
                .dependencies
                .store(pack_dependencies(dependencies), Ordering::Relaxed);
            dependencies
        } else {
            unpack_dependencies(memoized)
        };
        Some((
            std::ptr::from_ref(value).cast(),
            declaration.important,
            u32::try_from(index).unwrap(),
            false,
            dependencies,
        ))
    }

    fn winning_origin(&self, property: u16) -> Option<CascadeOrigin> {
        self.store.declaration(property).map(|_| CascadeOrigin::Author)
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
    use std::sync::Arc;

    #[test]
    fn winner_view_borrows_written_spelling_and_preserves_cascade_order() {
        let mut engine = StyleEngine::new(DeviceClass::ForegroundDesktop);
        let node = StyleNodeID::element(1);
        let kind = ElementDeclarationKind::InlineStyle;
        let properties = [property_id::MARGIN_LEFT, property_id::MARGIN_INLINE_START];
        let written = [1.25, 2.5].map(|value| RetainedStyleValueData::from_owned(StyleValueData::Number { value }));
        let mut owned = CascadedPropertyStore::new();
        for (&property, value) in properties.iter().zip(&written) {
            owned.seed_retained_property(property, value.clone_retained(), true, false);
        }
        let observer = written[0].clone_retained().into_arc();
        engine.state.facts.set_element_declared_properties(
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
                .collect(),
            written.into(),
            true,
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
        let view = store.view(&engine.state);
        for property in properties {
            let actual = view.winning_declaration(property).unwrap();
            let expected = owned.winning_declaration(property).unwrap();
            assert_eq!(
                (actual.0, actual.1, actual.2, actual.3),
                (expected.0, expected.1, expected.2, expected.3)
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
