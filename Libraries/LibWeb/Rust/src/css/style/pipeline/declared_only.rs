/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The engine for an element no rule reaches, such as one outside the document: the cascade of its
//! own declarations alone over the initial values, which no pass keeps and no element holds.

use super::super::custom_property_cascade::PreparedCustomFunctions;
use super::super::*;
use super::cascade::{Cascade, CascadeBelow, DeclaredOnly};
use super::custom_properties::{
    CustomPropertyStores, DrivenWithRegisteredProperties, SubstitutingSubject, SubstitutionCache,
    drive_with_registered_properties,
};
use super::records::Record;
use std::sync::Arc;

/// What a substitution in the declarations reads of the element: nothing of a tree, as the element
/// is in none the engine knows, so its conditions compute against no parent and it has no query
/// containers.
struct DeclaredOnlySubject<'a> {
    state: &'a RetainedState,
    node: StyleNodeID,
}

impl SubstitutingSubject for DeclaredOnlySubject<'_> {
    fn target(&self) -> (StyleNodeID, Option<u8>) {
        (self.node, None)
    }

    fn style_query_inputs(&self) -> Option<crate::css::cascaded_properties::StyleQueryInputs> {
        self.state.style_query_inputs_of(None, self.node)
    }

    fn custom_functions(&self) -> Option<PreparedCustomFunctions<'_>> {
        self.state
            .prepare_custom_functions(&self.state.document_functions, self.node, None)
    }

    fn apply_container_unit_bases(&self, _length: &mut crate::css::style_compute::FfiLengthResolutionContext) {}
}

impl StyleEngine {
    /// The record of an element no rule reaches, such as one outside the document: the cascade of
    /// its own declarations `declared` alone, in cascade order, over the initial values, with no
    /// parent. The computation is keyed by `subject`, the document's style node, which names no
    /// parent and no siblings. No element holds the record, which no slot takes either: the caller
    /// holds it.
    pub(in super::super) fn declared_only_record(
        &mut self,
        subject: StyleNodeID,
        declared: DeclaredOnly,
    ) -> Option<Arc<Record>> {
        let live = self
            .retained
            .live_style
            .take()
            .expect("the style engine answers every row");
        let cascade = Cascade::of_declared_only(&self.retained, &live.index, subject, declared);
        let answer = self.until_resolved(subject, |engine| {
            let mut waits = super::Waits::default();
            let computed = engine.retained.compute_declared_only(
                &live,
                &cascade,
                subject,
                &mut waits.fonts,
                &mut waits.random_bases,
            );
            match waits.fonts.is_empty() && waits.random_bases.is_empty() {
                true => Ok(computed),
                false => Err(waits),
            }
        });
        self.retained.live_style = Some(live);
        let record = answer.expect("the style engine answers every row")?;
        let custom_property_environment =
            self.retained
                .record_environment(subject, &record, None, 0, &mut HashMap::default());
        let record = self
            .retained
            .record_in_environment(&record, custom_property_environment)
            .into_owned();
        record.settle_view_dependency_flags(computed::record_dependency_flags(&record.facts(), &record.payloads));
        Some(record)
    }
}

impl RetainedState {
    /// The record [`StyleEngine::declared_only_record`] computes from `cascade`, or why there is
    /// none yet: the fonts and random base values it waits on are added to `font_requests` and
    /// `random_requests`.
    fn compute_declared_only(
        &self,
        live: &super::Live,
        cascade: &Cascade,
        node: StyleNodeID,
        font_requests: &mut Vec<font_resolution::FontRequest>,
        random_requests: &mut Vec<super::super::random_bases::RandomBaseRequest>,
    ) -> Option<Arc<Record>> {
        let initial = &live.initial;
        let subject = DeclaredOnlySubject { state: self, node };
        let substitutions = std::cell::RefCell::new(SubstitutionCache::default());
        let custom_properties = self
            .custom_property_store(None, None, &cascade.custom_winners, &mut HashMap::default(), &subject)
            .expect("the style engine answers every row");
        let mut groups = super::compute::GroupCache::default();
        let mut absolutized = crate::css::style_compute::AbsolutizedValues::default();
        let (computed, registered) = drive_with_registered_properties(|computed_store| {
            let mut suspended_font = None;
            let substituted = computed_store.unwrap_or(&custom_properties);
            let driven = self.drive_element(
                self,
                node,
                None,
                initial,
                &cascade.winners,
                &CascadeBelow {
                    cascade,
                    state: self,
                    node,
                },
                &mut suspended_font,
                random_requests,
                CustomPropertyStores {
                    own: substituted.as_ref(),
                    inherited: None,
                    substitutions: &substitutions,
                    subject: &subject,
                },
                &mut groups,
                &mut absolutized,
                super::PseudoElementTarget::of_kind(super::rules::ELEMENT_PSEUDO_KIND),
                None,
                initial.group_seeds(None),
                None,
            );
            font_requests.extend(suspended_font);
            let mut computed = driven.ok_or(())?;
            let registered = self.registered_custom_property_store(
                node,
                None,
                None,
                &cascade.custom_winners,
                &mut computed,
                &subject,
                random_requests,
            );
            // NB: Where base values of random functions are still to be drawn, the caller draws them and asks again.
            Ok::<_, ()>(DrivenWithRegisteredProperties {
                computed,
                registered,
                computes_again: false,
            })
        })
        .ok()?;
        let custom_properties = registered.unwrap_or(custom_properties);
        let value_of = |property| computed.groups.value(property);
        let counter_style_environment_identity = self.values_counter_style_environment_identity(
            computed::ComputedStyleTarget::new(node, u8::MAX),
            |property| {
                computed
                    .value_facts
                    .value_read_in_place(property)
                    .unwrap_or_else(|| value_of(property))
            },
        );
        let table_flags = Arc::new(computed.flags);
        Some(Arc::new(Record {
            header: super::records::RecordHeader::new(computed.facts),
            payloads: computed
                .groups
                .adopt_sharing(&[Some(&initial.payloads)])
                .with_rare_facts(
                    Arc::new(super::records::RareStyleFacts::of_computation(
                        super::records::InheritableCustomProperties::of(
                            custom_properties.as_ref(),
                            self.inheritable_custom_property_store(custom_properties.as_ref(), None),
                        ),
                        counter_style_environment_identity,
                        table_flags,
                        computed.value_facts,
                        computed.custom_property_reads,
                        computed.reads,
                        cascade.animation_name_scope,
                    )),
                    (!cascade.custom_winners.is_empty()).then(|| cascade.custom_winners.as_slice().into()),
                    &[],
                )
                .with_custom_properties(custom_properties.as_ref()),
        }))
    }
}
