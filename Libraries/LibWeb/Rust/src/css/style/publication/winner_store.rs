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
use std::cell::Cell;

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
    dependencies: Cell<Option<ExternalValueDependencies>>,
}

impl WinnerDeclaration {
    pub(super) fn new(property: u16, important: bool, value: WinnerValue) -> Self {
        Self {
            property,
            important,
            value,
            dependencies: Cell::new(None),
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
        let dependencies = declaration.dependencies.get().unwrap_or_else(|| {
            let dependencies = external_value_dependencies(value);
            declaration.dependencies.set(Some(dependencies));
            dependencies
        });
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
