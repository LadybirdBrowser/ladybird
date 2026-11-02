/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The canonical values of elements' own declarations, and the entries those declarations take.

use crate::css::declaration_block;
use crate::css::declaration_block::DeclarationEntry;
use crate::css::style_value::RetainedStyleValueData;
use crate::css::style_value::StyleValueData;
use crate::css::style_value_arc::StyleValueArc;

use super::RetainedState;
use super::instrumentation::Counter;

define_id! { struct CanonicalValueIndex(); }

impl super::intern_table::InternIdentity for CanonicalValueIndex {
    fn index(self) -> usize {
        self.0 as usize
    }

    fn from_index(index: usize) -> Self {
        Self(u32::try_from(index).expect("canonical value set exceeds u32 indexing"))
    }
}

/// How many values the set holds before its first sweep.
const MINIMUM_SWEEP_THRESHOLD: usize = 1024;

/// The values elements' own declarations were written with, one per content, so that elements
/// declaring equal values share one, which the caches over computed values key by. A value the set
/// alone holds is no element's any more, and a sweep drops it.
#[derive(Clone)]
pub(super) struct CanonicalValues {
    entries: super::intern_table::InternTable<CanonicalValueIndex, RetainedStyleValueData>,
    /// How many values the set holds before it sweeps the ones only it holds: twice as many as the
    /// last sweep kept, so sweeping costs a constant per value entered.
    sweep_threshold: usize,
}

impl Default for CanonicalValues {
    fn default() -> Self {
        Self {
            entries: super::intern_table::InternTable::default(),
            sweep_threshold: MINIMUM_SWEEP_THRESHOLD,
        }
    }
}

impl CanonicalValues {
    /// The bytes of the set and its index, with the values it holds counted aside.
    pub(super) fn heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        use crate::heap_size::HeapSize;
        self.entries.heap_size(sizer, |value, sizer| value.heap_size(sizer))
    }

    /// Whether the set grew enough since its last sweep to sweep it again.
    pub(super) fn should_sweep(&self) -> bool {
        self.entries.len() >= self.sweep_threshold
    }

    /// Drop the values only the set holds, which no element declares any more.
    ///
    /// NB: A fork's copy of the set holds a second reference to each of its values, so a sweep
    ///     while a fork lives keeps them all.
    pub(super) fn sweep(&mut self) {
        self.entries.retain(|value| !value.is_unique());
        self.sweep_threshold = (self.entries.len() * 2).max(MINIMUM_SWEEP_THRESHOLD);
    }

    /// The value equal to `value` elements share, entering `value` as that one where none is, and
    /// whether one was. A value whose content cannot name it, as an image's resource context is
    /// not part of its content, is its own.
    fn canonical(&mut self, value: &StyleValueArc) -> (RetainedStyleValueData, bool) {
        if crate::css::style_compute::value_may_need_style_sheet_resource_context(value) {
            return (RetainedStyleValueData::from_arc(value.clone()), false);
        }
        let content_hash = value.content_hash();
        let data: &StyleValueData = value;
        if let Some(index) = self.entries.find(content_hash, |_index, entry| entry.data() == data) {
            return (self.entries.get(index).clone(), true);
        }
        let index =
            CanonicalValueIndex(u32::try_from(self.entries.len()).expect("canonical value set exceeds u32 indexing"));
        let canonical = RetainedStyleValueData::from_arc(value.clone());
        self.entries.insert(content_hash, index, canonical.clone());
        (canonical, false)
    }
}

/// The entries of the longhands `declarations` declare, each with the value an element declaring
/// it is written with, a shorthand's longhands taking its value.
pub(crate) fn element_declared_properties<'a>(
    declarations: impl ExactSizeIterator<Item = declaration_block::DeclarationRef<'a>>,
) -> Vec<(DeclarationEntry, RetainedStyleValueData)> {
    fn append(
        property: u16,
        declaration: declaration_block::DeclarationRef<'_>,
        declarations: &mut Vec<(DeclarationEntry, RetainedStyleValueData)>,
    ) {
        use crate::css::property_metadata::{longhands_for_shorthand, property_is_shorthand};
        // Element declarations decide longhands. Presentation attributes can name shorthands
        // whose children are themselves shorthands, so expand the complete property inventory.
        // The longhands of a shorthand awaiting substitution have declarations of their own, which
        // a later change to the block may have replaced or removed.
        if property_is_shorthand(property)
            && !matches!(
                &**declaration.value,
                StyleValueData::Unresolved { .. } | StyleValueData::PendingSubstitution { .. }
            )
        {
            for &longhand in longhands_for_shorthand(property) {
                append(longhand, declaration, declarations);
            }
        } else {
            declarations.push((
                DeclarationEntry::new(property, declaration.important, declaration.operator),
                RetainedStyleValueData::from_arc(declaration.value.clone()),
            ));
        }
    }
    let mut entries = Vec::with_capacity(declarations.len());
    for declaration in declarations {
        append(declaration.property_id, declaration, &mut entries);
    }
    entries
}

impl RetainedState {
    /// The value equal to `value` the elements of the document share.
    pub(super) fn canonical_value(&mut self, value: &StyleValueArc) -> RetainedStyleValueData {
        let (canonical, reused) = self.canonical_values.canonical(value);
        if reused {
            self.counters.bump(Counter::SpecifiedValuesReused);
        }
        canonical
    }
}

#[cfg(test)]
#[allow(clippy::arc_with_non_send_sync)]
mod tests {
    use super::*;

    #[test]
    fn equal_content_shares_one_value() {
        let mut values = CanonicalValues::default();
        let originals: Vec<_> = (0..64)
            .map(|i| StyleValueArc::new(StyleValueData::Number { value: f64::from(i) }))
            .collect();
        let canonical: Vec<_> = originals.iter().map(|value| values.canonical(value).0).collect();
        let respellings: Vec<_> = (0..64)
            .map(|i| StyleValueArc::new(StyleValueData::Number { value: f64::from(i) }))
            .collect();
        for (respelling, canonical) in respellings.iter().zip(&canonical) {
            let (shared, reused) = values.canonical(respelling);
            assert!(reused);
            assert_eq!(shared.pointer(), canonical.pointer());
        }
    }

    #[test]
    fn a_sweep_drops_the_values_only_the_set_holds() {
        let mut values = CanonicalValues::default();
        let kept = values
            .canonical(&StyleValueArc::new(StyleValueData::Number { value: 1.0 }))
            .0;
        drop(values.canonical(&StyleValueArc::new(StyleValueData::Number { value: 2.0 })));
        values.sweep();
        assert_eq!(values.entries.len(), 1);
        assert!(
            values
                .canonical(&StyleValueArc::new(StyleValueData::Number { value: 1.0 }))
                .1
        );
        drop(kept);
    }
}
