/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The engine's computation of one element's style from the winners its cascade decided.

use super::super::publication::pseudo_kind;
use super::super::*;
use super::cascade::Winner;
use super::drive::{self, Drive, Suspension, Unanswered, drive_font_metric};
use super::winner_store::{self, WinnerDeclaration, WinnerStore, WinnerValue, shorthand_longhand_data};
use crate::css::computed_longhand_table::FONT_METRICS_DEPEND_ON_VIEWPORT_METRICS;
use crate::css::computed_values::{GroupValueLayout, GroupValues, GroupValuesWriter, StyleGroupMasks};
use crate::css::style::input_types::StyleEnvironment;
use crate::css::style_compute::RootFontReads;
use crate::css::style_value::ShorthandPayload;
use crate::css::style_value::{RetainedStyleValueData, StyleValueData};

/// Root-relative computation reads these inputs independently of its inheritance parent.
/// Bitwise keys keep equality exact without using an invalidation generation as a substitute
/// for the values. The viewport-dependence bit matters even when today's metrics agree.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(in crate::css::style) struct RootFontInputs {
    metrics: [u64; 5],
    depends_on_viewport: bool,
    /// https://drafts.csswg.org/css-viewport/#effective-zoom
    /// The bits of the effective zoom of the document element, which the metrics are zoomed by.
    effective_zoom: u64,
}

impl RootFontInputs {
    /// What of these inputs moved from `previous`, as the least a style must read of the root
    /// element's font to resolve its lengths again: a load of a face into the root element's font
    /// moves its metrics alone, which a `rem` does not read.
    pub(in crate::css::style) fn moved_from(self, previous: Self) -> RootFontReads {
        if self.metrics[0] != previous.metrics[0]
            || self.depends_on_viewport != previous.depends_on_viewport
            || self.effective_zoom != previous.effective_zoom
        {
            return RootFontReads::FontSize;
        }
        match self == previous {
            true => RootFontReads::None,
            false => RootFontReads::Metrics,
        }
    }

    /// The metrics a `rem` resolves against, and whether they read the viewport.
    pub(in crate::css::style) fn font_metrics(self) -> (crate::css::style_compute::FfiFontMetrics, bool) {
        let [font_size, x_height, cap_height, zero_advance, line_height] = self.metrics.map(f64::from_bits);
        (
            crate::css::style_compute::FfiFontMetrics {
                font_size,
                x_height,
                cap_height,
                zero_advance,
                line_height,
            },
            self.depends_on_viewport,
        )
    }

    /// https://drafts.csswg.org/css-viewport/#effective-zoom
    pub(in crate::css::style) fn effective_zoom(self) -> f64 {
        f64::from_bits(self.effective_zoom)
    }

    pub(in crate::css::style) fn apply_to(self, inputs: &mut bridge::FfiDocumentStyleComputationInputs) {
        inputs.root_font_size = f64::from_bits(self.metrics[0]);
        inputs.root_font_x_height = f64::from_bits(self.metrics[1]);
        inputs.root_font_cap_height = f64::from_bits(self.metrics[2]);
        inputs.root_font_zero_advance = f64::from_bits(self.metrics[3]);
        inputs.root_line_height = f64::from_bits(self.metrics[4]);
        inputs.root_font_metrics_depend_on_viewport_metrics = self.depends_on_viewport;
        inputs.root_effective_zoom = f64::from_bits(self.effective_zoom);
    }

    /// The inputs a document element's computed groups and dependency flags give.
    pub(in crate::css::style) fn from_payloads(
        payloads: &crate::css::computed_values::StyleGroups,
        dependency_flags: u8,
    ) -> Self {
        use crate::css::table_group_builder::group_index;
        // SAFETY: The payloads are a record's live font group.
        let font = unsafe {
            payloads[group_index::FONT]
                .cast::<crate::css::computed_value_types::FontValues>()
                .deref()
        };
        let first_available = font.first_available_font_metrics();
        Self {
            metrics: [
                font.font_size.to_double().to_bits(),
                drive_font_metric(first_available.x_height).to_bits(),
                drive_font_metric(first_available.ascent).to_bits(),
                drive_font_metric(first_available.zero_advance).to_bits(),
                font.line_height_used().to_double().to_bits(),
            ],
            depends_on_viewport: dependency_flags & FONT_METRICS_DEPEND_ON_VIEWPORT_METRICS != 0,
            effective_zoom: f64::from(payloads.inherited_values().effective_zoom()).to_bits(),
        }
    }

    pub(in crate::css::style) fn from_document(inputs: &bridge::FfiDocumentStyleComputationInputs) -> Self {
        Self {
            metrics: [
                inputs.root_font_size.to_bits(),
                inputs.root_font_x_height.to_bits(),
                inputs.root_font_cap_height.to_bits(),
                inputs.root_font_zero_advance.to_bits(),
                inputs.root_line_height.to_bits(),
            ],
            depends_on_viewport: inputs.root_font_metrics_depend_on_viewport_metrics,
            effective_zoom: inputs.root_effective_zoom.to_bits(),
        }
    }
}

impl RetainedState {
    /// The engine: the computed initial values, as an element with no declarations and no parent
    /// computes them, with the groups they assemble into, each holding one reference, and the
    /// color they resolve. Every table is seeded from this one, and an element whose
    /// declarations move none of a non-inherited group's longhands shares the initial group. A
    /// drive that waits on a font names the request to refill before trying again.
    pub(in crate::css::style) fn initial_style(
        &self,
        root: StyleNodeID,
    ) -> Result<ComputedInitialStyle, Option<Box<font_resolution::FontRequest>>> {
        let subject = DriveSubject {
            target: computed::ComputedStyleTarget::new(root, u8::MAX),
            parent: None,
            facts: 0,
        };
        let store = WinnerStore::new(Vec::new());
        let inputs = self.document_style_computation_inputs;
        let mut font_scratch = drive::FontDriveScratch::default();
        // The initial values compute from nothing: from the default groups, which hold no values.
        crate::css::computed_values::with_default_seed(&self.process_style.default_groups, |seed| {
            let writer = GroupValuesWriter::seeded_for_child(
                seed,
                seed,
                std::iter::empty(),
                crate::css::computed_longhand_table::LonghandTableFlags::empty(),
                0,
            );
            match self.engine_full_drive(subject, writer, &store, &inputs, &mut font_scratch) {
                Ok(drive::DrivenValues {
                    writer,
                    length,
                    font: Some(font),
                }) => {
                    let (values, flags, dependency_flags) = writer.finish(&[]);
                    let source = crate::css::table_group_builder::LonghandSource::Values {
                        values: &values,
                        reads: None,
                    };
                    let color_inputs =
                        crate::css::table_group_builder::assembly_color_inputs_from_source(&source, &length);
                    let mut payloads = [std::ptr::null(); crate::css::table_group_builder::group_index::COUNT];
                    for (group, payload) in payloads.iter_mut().enumerate() {
                        // SAFETY: The values were driven for an element with no parent.
                        *payload = unsafe {
                            crate::css::table_group_builder::assemble_group_from_source(
                                &source,
                                group,
                                Some(&font),
                                crate::css::computed_values::GroupShares::new(
                                    &self.process_style.default_groups,
                                    group,
                                    None,
                                ),
                                color_inputs,
                                &length,
                            )
                        };
                    }
                    // The initial groups hold the initial values, each group in its own allocation.
                    // SAFETY: Each payload is a live payload of its group, whose reference the style takes.
                    unsafe { values.put_into(&mut payloads) };
                    let initial_font = super::traversal::InitialFont {
                        first_available_font: font.first_available_font as usize,
                        font_cascade_list: font.font_cascade_list as usize,
                    };
                    Ok(ComputedInitialStyle {
                        payloads,
                        value_facts: values.into_facts(),
                        flags,
                        dependency_flags,
                        color: color_inputs.1,
                        font: initial_font,
                    })
                }
                Err(Unanswered::Suspended(Suspension::Font)) => {
                    Err(Some(Box::new(font_scratch.take_suspended_request())))
                }
                _ => Err(None),
            }
        })
    }

    /// The engine: compute the record of `node` from the cascade winners it decided, against the
    /// records `records` holds for its ancestors, into a table seeded from `initial` and the
    /// inherited values of the parent's table. A winner the engine computes directly absolutizes
    /// against the element's context; the others go through the longhand drive, which then
    /// evaluates only them and the inputs every drive evaluates. An element whose winners all
    /// compute directly does not run the drive. Only the groups the winners move are assembled
    /// into `payloads`; the others share the parent's payload or the initial one. Each payload
    /// holds one reference the caller releases. A winner is the property it decides, whether it
    /// is important, and where it was declared: its source and its index in the source's block,
    /// highest priority first. Answers the frozen table, the element's resolved current color,
    /// and whether the element ran no drive.
    /// The engine: whether the `content` the pseudo-element of `kind` cascades to lets it generate a
    /// box: `none` generates nothing for every kind, and `normal`, the initial value, nothing for a
    /// `::before` or `::after`.
    pub(in crate::css::style) fn pseudo_content_generates(&self, winners: &[Winner], kind: u8) -> bool {
        use crate::css::property_metadata::property_id as prop;
        use crate::css::style_compute::keyword;
        let generated = matches!(kind, pseudo_kind::BEFORE | pseudo_kind::AFTER);
        let Some(&Winner { source, index, .. }) = winners.iter().find(|winner| winner.property == prop::CONTENT) else {
            return !generated;
        };
        let WinnerSource::Rule(rule) = source else {
            return true;
        };
        match self
            .rules
            .written_values_of(rule)
            .get(index)
            .and_then(RetainedStyleValueData::optional_data)
        {
            Some(crate::css::style_value::StyleValueData::Keyword { keyword }) => {
                *keyword != keyword::NONE && (*keyword != keyword::NORMAL || !generated)
            }
            _ => true,
        }
    }

    #[allow(clippy::too_many_arguments, clippy::too_many_lines)]
    pub(in crate::css::style) fn drive_element(
        &self,
        records: &impl drive::DriveRecords,
        node: StyleNodeID,
        parent: Option<(StyleNodeID, u32)>,
        initial: &super::traversal::InitialStyle,
        winners: &[Winner],
        cascade: &dyn CascadeBelow,
        suspended_font: &mut Option<font_resolution::FontRequest>,
        random_requests: &mut Vec<super::random_bases::RandomBaseRequest>,
        custom_properties: super::custom_properties::CustomPropertyStores<'_>,
        group_cache: &mut GroupCache,
        absolutized: &mut crate::css::style_compute::AbsolutizedValues,
        target_pseudo_kind: u8,
        root_font: Option<RootFontInputs>,
        group_seeds: GroupSeeds<'_>,
    ) -> Drive<ComputedElement> {
        use crate::css::property_metadata::{FIRST_LONGHAND_PROPERTY_ID, LONGHAND_WORD_COUNT, property_id as prop};
        use crate::css::style_compute::{
            DIRECTION_COUNT, LOGICAL_ALIAS_BIT, PHYSICAL_TO_LOGICAL_BIT, WRITING_MODE_COUNT,
            map_logical_alias_to_physical, map_physical_to_logical_alias, table_row_bits,
        };
        use crate::css::table_group_builder::group_index;

        // Root-relative lengths resolve against the font of the document element's record as the
        // pass has it, where the pass knows one, rather than the inputs installed before.
        let mut inputs = self.document_style_computation_inputs;
        if let Some(root_font) = root_font {
            root_font.apply_to(&mut inputs);
        }
        let GroupSeeds {
            seeds,
            parent: seeded_parent,
        } = group_seeds;
        let parent_view = match parent {
            Some((parent, _)) => Some(records.assigned_record_view(parent).ok_or(Unanswered::AwaitsParent)?),
            None => None,
        };
        let parent_longhands = parent_view.as_ref().map(computed::StyleRecordView::longhands);
        let parent_overlay = parent_view.as_ref().and_then(|view| view.animated_overlay);
        // A pseudo-element's computation reads only the facts about its element's markup language.
        let is_pseudo_element = target_pseudo_kind != u8::MAX;
        let declared_only = cascade.declared_only();
        let facts = declared_only.map_or_else(|| self.projection.adjustment_facts(node), |own| own.facts);
        let facts = match is_pseudo_element {
            true => facts & PSEUDO_ELEMENT_ADJUSTMENT_FACTS,
            false => facts,
        };

        // Which winners compute directly, with their values, and which need the drive.
        let element_declarations = [
            ElementDeclarationKind::InlineStyle,
            ElementDeclarationKind::PresentationalHint,
            ElementDeclarationKind::SvgPresentationAttribute,
        ]
        .map(|kind| {
            match declared_only {
                Some(own) => Some(own.declarations.declarations(kind)),
                None => self.element_declarations.declarations(node, kind),
            }
            .map(|declarations| (kind, declarations))
        });
        // The written values of the declarations live for the pass, so only substituted and
        // defaulted ones are held here.
        let mut easy: smallvec::SmallVec<[DirectWinner<'_>; 24]> = smallvec::SmallVec::new();
        let mut hard: smallvec::SmallVec<[u16; 24]> = smallvec::SmallVec::new();
        // The font longhands that compute natively, where the element resolves its font without
        // a drive.
        let mut native_font: smallvec::SmallVec<[(u16, std::borrow::Cow<'_, RetainedStyleValueData>); 8]> =
            smallvec::SmallVec::new();
        // The winners computing directly, with whether each takes the value its parent computed.
        let mut direct_inheritance: smallvec::SmallVec<[(u16, bool); 24]> = smallvec::SmallVec::new();
        // What each winner substituted to, where its value waited on substitution.
        let mut substituted: smallvec::SmallVec<[Option<RetainedStyleValueData>; 24]> = smallvec::SmallVec::new();
        // The winners that decide anything, with what each substituted to where its value waited
        // on substitution.
        let mut deciding: smallvec::SmallVec<[Winner; 24]> = smallvec::SmallVec::new();
        // Whether a winner computes against the element itself rather than only its context.
        let mut element_dependent = false;
        // The style groups of the properties that do not inherit whose value the winners take from the
        // parent.
        let mut parent_reset_groups = 0u32;
        // Whether a winner computes against the element's place among its siblings.
        let mut reads_sibling_position = false;
        // How much of the root element's font a winner resolves lengths against.
        let mut reads_root_font = RootFontReads::None;
        // Whether a winner resolves a length against a metric of a first available font.
        let mut reads_font_metric_lengths = false;
        // What the winners compute from of the document's environment.
        let mut environment_reads = StyleEnvironment::default();
        // What the winners' substitutions read beyond the element's custom properties.
        let mut substitution_reads = 0;
        // The custom properties the winners substitute, and whether one may read others.
        let mut custom_property_reads = Vec::new();
        let mut reads_unnamed_custom_properties = false;
        // A parent's computed value: its record's, or its table's with the animated value it hands
        // down where its animations compose.
        let parent_value = |property: u16| {
            let parent_longhands = parent_longhands?;
            parent_longhands
                .overlay_value(parent_overlay, property)
                .or_else(|| parent_longhands.handle(property))
        };
        // https://drafts.csswg.org/css-logical/#box
        // A logical alias computes as its physical counterpart for the element's writing mode and
        // direction, which an element declaring neither has from its parent.
        let parent_flow =
            match winners
                .iter()
                .any(|winner| winner.property == prop::WRITING_MODE || winner.property == prop::DIRECTION)
            {
                true => None,
                false => parent_value(prop::WRITING_MODE).zip(parent_value(prop::DIRECTION)).map(
                    |(writing_mode, direction)| {
                        crate::css::style_compute::writing_mode_and_direction_of(Some(writing_mode), Some(direction))
                    },
                ),
            };
        // The logical aliases declared with a CSS-wide keyword that compute directly, with the
        // physical longhand each computes as and whether it takes the parent's value.
        let mut keyword_aliases: smallvec::SmallVec<[(u16, u16, bool); 8]> = smallvec::SmallVec::new();
        // Each winner's classification, with the declaration it decides with and, where it rolled
        // back below a revert, the place of that declaration and of the revert.
        let mut resolved: smallvec::SmallVec<[(_, _, Option<RolledBack>); 24]> = smallvec::SmallVec::new();
        let mut rolled_below = false;
        for (position, &winner) in winners.iter().enumerate() {
            // https://drafts.csswg.org/css-cascade-5/#default
            // `revert` and `revert-layer`, declared or substituted, roll the property back below
            // the declaration that held them, to a lower declaration that is substituted in turn.
            // The declarations it rolled back, with the keyword of each.
            let mut reverted: smallvec::SmallVec<[(CascadePriority, CascadeOperator); 2]> = smallvec::SmallVec::new();
            let (mut priority, mut winner) = (None, winner);
            let classified = loop {
                let Winner {
                    property,
                    important,
                    source,
                    index,
                } = winner;
                let mut classified =
                    self.classify_winner(&element_declarations, custom_properties, property, source, index);
                // A declaration that rolls back still decides where it does.
                custom_property_reads.append(&mut classified.reads.custom_properties);
                reads_unnamed_custom_properties |= classified.reads.unnamed_custom_properties;
                let operator = match &classified.value {
                    ResolvedWinnerValue::Substituted(value) => value
                        .optional_data()
                        .map(crate::css::declaration_block::declaration_operator),
                    _ => classified.reads.css_wide_keyword,
                };
                let Some(operator @ (CascadeOperator::Revert | CascadeOperator::RevertLayer)) = operator else {
                    break Some(classified);
                };
                reverted.push((
                    priority.unwrap_or_else(|| cascade.priority_of(source, important)),
                    operator,
                ));
                // What the property rolls back to is decided by declarations the winners do not name.
                element_dependent = true;
                match cascade.winner_below(property, &reverted) {
                    Some((lower_priority, lower)) => (priority, winner) = (Some(lower_priority), lower),
                    // Nothing below declares the property, which is undeclared and computes as the
                    // element's seed holds it.
                    // https://drafts.csswg.org/css-logical/#box
                    // A logical or physical counterpart declared before the revert, which the revert
                    // rolls back past as well, leaves the group's value as if it were not declared
                    // either: the property computes as `unset` in the place of the revert.
                    None if !winners[position + 1..].iter().any(
                        |&Winner {
                             property: other,
                             important: other_important,
                             source: other_source,
                             ..
                         }| {
                            is_logical_counterpart(property, other)
                                && reverted[0]
                                    .0
                                    .reverts_past(reverted[0].1, &cascade.priority_of(other_source, other_important))
                        },
                    ) =>
                    {
                        break None;
                    }
                    None => {
                        // The revert decides in its own place.
                        winner = winners[position];
                        priority = None;
                        break Some(ClassifiedWinner {
                            value: ResolvedWinnerValue::Substituted(RetainedStyleValueData::from_owned(
                                crate::css::style_value::StyleValueData::Keyword {
                                    keyword: crate::css::style_compute::keyword::UNSET,
                                },
                            )),
                            computation: WinnerComputation::Full,
                            reads: WinnerReads {
                                element: true,
                                parent_reset_value: !crate::css::property_metadata::property_is_inherited(property),
                                css_wide_keyword: Some(CascadeOperator::Unset),
                                sibling_position: false,
                                substitution: 0,
                                ..WinnerReads::default()
                            },
                        });
                    }
                }
            };
            let rolled_back = priority.map(|priority| RolledBack {
                place: (priority, winner.index),
                revert: (reverted[0].0, winners[position].index),
                operator: reverted[0].1,
            });
            rolled_below |= rolled_back.is_some();
            resolved.push((winner, classified, rolled_back));
        }
        // https://drafts.csswg.org/css-logical/#box
        // Of a logical property and its physical counterpart, the one declared with higher priority
        // decides both. A property rolled back below a revert decides at the place of the
        // declaration it rolled back to, unless the revert is above a counterpart that does not
        // roll back and rolls back past it as well: the group's value is then as if the
        // counterpart were not declared either, and the property decides at the place of the
        // revert.
        if rolled_below {
            // The longhand of each winner, the place it decides at, and whether it rolled back.
            type Place = (u16, (CascadePriority, usize), bool);
            let places: smallvec::SmallVec<[Place; 24]> = resolved
                .iter()
                .map(|&(winner, _, ref rolled_back)| match rolled_back {
                    Some(rolled_back) => (winner.property, rolled_back.place, true),
                    None => (
                        winner.property,
                        (cascade.priority_of(winner.source, winner.important), winner.index),
                        false,
                    ),
                })
                .collect();
            let key = |position: usize, rolled_back: &Option<RolledBack>| match rolled_back {
                Some(rolled_back)
                    if places.iter().any(|&(property, counterpart, rolled)| {
                        !rolled
                            && is_logical_counterpart(places[position].0, property)
                            && rolled_back.revert > counterpart
                            && rolled_back.revert.0.reverts_past(rolled_back.operator, &counterpart.0)
                    }) =>
                {
                    rolled_back.revert
                }
                _ => places[position].1,
            };
            let mut keyed: smallvec::SmallVec<[_; 24]> = resolved
                .into_iter()
                .enumerate()
                .map(|(position, entry)| (key(position, &entry.2), entry))
                .collect();
            keyed.sort_by(|(a, _), (b, _)| b.cmp(a));
            resolved = keyed.into_iter().map(|(_, entry)| entry).collect();
        }
        for (winner, classified, _) in resolved {
            let Some(ClassifiedWinner {
                value,
                computation,
                reads,
            }) = classified
            else {
                continue;
            };
            substitution_reads |= reads.substitution;
            let property = winner.property;
            element_dependent |= reads.element;
            if reads.parent_reset_value {
                parent_reset_groups |= crate::css::property_metadata::property_style_group_index(property)
                    .map_or(u32::MAX, |group| 1 << group);
            }
            reads_sibling_position |= reads.sibling_position;
            reads_root_font = reads_root_font.max(reads.root_font);
            reads_font_metric_lengths |= reads.font_metrics;
            environment_reads |= reads.environment | property_environment_reads(property);
            let Some(written) = value.value() else {
                continue;
            };
            // https://drafts.csswg.org/css-cascade-5/#defaulting-keywords
            // A CSS-wide keyword's computed value is the initial value or the parent's computed
            // value, which computes directly as any computed value does.
            let mut inherits_keyword = false;
            let is_logical_alias = table_row_bits(property) & LOGICAL_ALIAS_BIT != 0;
            // The longhand whose parent's value the winner inherits: a logical alias inherits its
            // physical counterpart's.
            let mut inherited_property = property;
            let keyword_value = reads.css_wide_keyword.and_then(|keyword| {
                if is_logical_alias {
                    let (writing_mode, direction) = parent_flow?;
                    inherited_property = map_logical_alias_to_physical(property, writing_mode, direction);
                }
                let inherits = match keyword {
                    CascadeOperator::Inherit => true,
                    CascadeOperator::Initial => false,
                    CascadeOperator::Unset => crate::css::property_metadata::property_is_inherited(property),
                    _ => return None,
                };
                inherits_keyword = inherits;
                match (inherits, parent_longhands.is_some()) {
                    (true, true) => parent_value(inherited_property),
                    // NB: Both longhands of a logical pair take the alias's initial value, where a
                    //     drive gives each its own.
                    _ => {
                        let initial_value = |property| {
                            Some(initial.longhand(crate::css::computed_values::LonghandID::new(property)))
                                .filter(|value| !value.pointer().is_null())
                        };
                        initial_value(property).filter(|value| initial_value(inherited_property) == Some(*value))
                    }
                }
                .filter(|value| {
                    value.optional_data().is_some_and(|data| {
                        crate::css::style_compute::value_computes_directly(property, data, Some(Default::default()))
                    })
                })
                .cloned()
            });
            if is_logical_alias {
                if keyword_value.is_some() {
                    keyword_aliases.push((property, inherited_property, inherits_keyword));
                }
            } else if matches!(computation, WinnerComputation::Direct(_)) || keyword_value.is_some() {
                direct_inheritance.push((property, keyword_value.is_some() && inherits_keyword));
            }
            let font_computes_natively = !is_logical_alias
                && matches!(computation, WinnerComputation::Full)
                && reads.css_wide_keyword.is_none()
                && written
                    .optional_data()
                    .is_some_and(|data| crate::css::style_compute::font_value_computes_natively(property, data));
            match (computation, keyword_value) {
                (_, Some(value)) => easy.push((property, std::borrow::Cow::Owned(value), Some(Default::default()))),
                (WinnerComputation::Direct(dependencies), None) => easy.push((
                    property,
                    match &value {
                        ResolvedWinnerValue::Written(written) => std::borrow::Cow::Borrowed(*written),
                        _ => std::borrow::Cow::Owned(written.clone()),
                    },
                    dependencies,
                )),
                (WinnerComputation::Full, None) if font_computes_natively => native_font.push((
                    property,
                    match &value {
                        ResolvedWinnerValue::Written(written) => std::borrow::Cow::Borrowed(*written),
                        _ => std::borrow::Cow::Owned(written.clone()),
                    },
                )),
                (WinnerComputation::Full, None) => hard.push(property),
            }
            deciding.push(winner);
            substituted.push(value.into_substituted());
        }
        // Both longhands of a logical pair take their value from the one declared with higher
        // priority, so where the alias's physical counterpart is declared as well, the alias's
        // keyword computes directly only where the two inherit alike, and so mark the pair alike.
        keyword_aliases.retain(|&mut (alias, physical, inherits)| {
            let alike = direct_inheritance
                .iter()
                .all(|&(property, inherited)| property != physical || inherited == inherits);
            if !alike {
                easy.retain(|(property, ..)| *property != alias);
                hard.push(alias);
            }
            alike
        });
        let winners = &deciding[..];
        let mut groups = 0_u32;
        for winner in winners {
            groups |= moved_longhand_groups(winner.property);
        }
        // A drive evaluates the box inputs itself, so with a drive they are the drive's.
        // A zoom scales the computed values of longhands the element does not declare, such as the
        // font size it inherits or the initial widths of its borders, so an element declaring one
        // or inheriting a zoom other than one has every longhand driven.
        // https://drafts.csswg.org/css-pseudo-4/#highlight-cascade
        // A highlight pseudo-element inherits from another one than its element, which only a drive
        // of every longhand reads.
        // https://drafts.csswg.org/css-color-adjust-1/#color-scheme-effect
        // The root's initial values, such as the `canvastext` of color, resolve against the color
        // scheme it uses, which the initial values the seed holds were not computed under.
        let drives_everything = parent_view.is_none()
            || pseudo_kind::is_highlight(target_pseudo_kind)
            || winners.iter().any(|winner| winner.property == prop::ZOOM)
            || parent_longhands.is_some_and(|longhands| longhands.effective_zoom() != 1.0);
        // A drive of every longhand computes the color scheme and the widths of borders it does
        // not declare against the document's environment.
        if drives_everything {
            environment_reads |= StyleEnvironment::COLOR_SCHEME | StyleEnvironment::DEVICE_PIXEL_RATIO;
        }
        // The used line height the font group holds reads a line-height computing natively.
        if !native_font.is_empty() {
            groups |= 1 << group_index::FONT;
        }
        // The document element builds every group, its font group against the document's initial
        // font.
        let builds_font = groups & (1 << group_index::FONT) != 0 || parent_view.is_none();
        // An element whose font group moves resolves its font itself where its font longhands
        // compute natively against its parent's font, which reads no viewport, and nothing
        // zooms the sizes.
        // NB: The line-height the adjustments of an input or a select element settle, which its
        //     font group holds, is a drive's.
        let resolves_font = builds_font
            && parent_view
                .as_ref()
                .is_none_or(|view| view.dependency_flags & FONT_METRICS_DEPEND_ON_VIEWPORT_METRICS == 0)
            && !(inputs.root_effective_zoom > 0.0 && inputs.root_effective_zoom != 1.0)
            && facts
                & (bridge::element_adjustment_fact::CHECK_INPUT_LINE_HEIGHT
                    | bridge::element_adjustment_fact::FORCE_LINE_HEIGHT_NORMAL)
                == 0;
        let runs_drive = drives_everything || !hard.is_empty() || (builds_font && !resolves_font);
        // A drive maps a logical alias by the writing mode and direction it computes, and marks
        // the pair itself.
        if runs_drive {
            // NB: The drive builds the font group from the table as it ends its font phases, so
            //     the longhands the font group carries compute in it rather than after it.
            hard.extend(native_font.drain(..).map(|(property, _)| property));
            easy.retain(|(property, ..)| {
                let is_input = crate::css::style_compute::is_required_driver_input(*property)
                    || property_feeds_box_type_transformation(*property)
                    || font_group_carries_longhand(*property)
                    || keyword_aliases.iter().any(|&(alias, ..)| alias == *property);
                if is_input {
                    hard.push(*property);
                }
                !is_input
            });
        } else {
            for &(alias, physical, inherits) in &keyword_aliases {
                direct_inheritance.push((alias, inherits));
                direct_inheritance.push((physical, inherits));
            }
            direct_inheritance.extend(native_font.iter().map(|&(property, _)| (property, false)));
        }
        // https://drafts.csswg.org/css-backgrounds-3/#layering
        // The background layer longhands repeat to as many layers as background-image has, which
        // the drive coordinates them to as it computes background-image.
        if hard.contains(&prop::BACKGROUND_IMAGE) {
            easy.retain(|(property, ..)| {
                let is_layer = crate::css::style_compute::BACKGROUND_LAYER_LONGHANDS.contains(property);
                if is_layer {
                    hard.push(*property);
                }
                !is_layer
            });
        }

        let mut selected = [0_u64; LONGHAND_WORD_COUNT];
        let mut select = |property: u16, groups: &mut u32| {
            let index = usize::from(property - FIRST_LONGHAND_PROPERTY_ID);
            selected[index / 64] |= 1 << (index % 64);
            *groups |= moved_longhand_groups(property);
        };
        for &property in &hard {
            select(property, &mut groups);
            let bits = table_row_bits(property);
            if bits & (LOGICAL_ALIAS_BIT | PHYSICAL_TO_LOGICAL_BIT) != 0 {
                for writing_mode in 0..WRITING_MODE_COUNT as u8 {
                    for direction in 0..DIRECTION_COUNT as u8 {
                        let counterpart = if bits & LOGICAL_ALIAS_BIT != 0 {
                            map_logical_alias_to_physical(property, writing_mode, direction)
                        } else {
                            map_physical_to_logical_alias(property, writing_mode, direction)
                        };
                        select(counterpart, &mut groups);
                    }
                }
            }
            // A line's width computes to zero when its style is `none` or `hidden`.
            let partner = match property {
                prop::BORDER_TOP_STYLE => prop::BORDER_TOP_WIDTH,
                prop::BORDER_RIGHT_STYLE => prop::BORDER_RIGHT_WIDTH,
                prop::BORDER_BOTTOM_STYLE => prop::BORDER_BOTTOM_WIDTH,
                prop::BORDER_LEFT_STYLE => prop::BORDER_LEFT_WIDTH,
                prop::OUTLINE_STYLE => prop::OUTLINE_WIDTH,
                prop::COLUMN_RULE_STYLE => prop::COLUMN_RULE_WIDTH,
                _ => continue,
            };
            select(partner, &mut groups);
        }

        // The animated values a parent whose animations compose hands down over the values of its
        // record.
        let inherited_overrides =
            |seeded: &'_ std::sync::Arc<super::pipeline::Record>| -> Vec<(u16, RetainedStyleValueData)> {
                computed::ViewLonghands::of_record(seeded)
                    .inherited_overrides(parent_overlay)
                    .collect()
            };
        // The element's values start from its parent's groups where those hold the values of the
        // parent's record, with the animated values the parent hands down, and the document
        // element's from the initial ones. The values of a parent no record of the pass holds are
        // written over the initial groups. The drive and the direct computation both write into
        // them.
        let flags = match parent_view {
            Some(_) => crate::css::computed_longhand_table::LonghandTableFlags::of_child_seed(&initial.flags),
            None => crate::css::computed_longhand_table::LonghandTableFlags::of_drive_copy(&initial.flags),
        };
        let mut writer = match (parent_view.as_ref(), seeded_parent) {
            (None, None) => GroupValuesWriter::seeded_for_child(
                seeds[0],
                seeds[1],
                std::iter::empty(),
                flags,
                initial.dependency_flags,
            ),
            (Some(_), Some(seeded)) if parent_longhands.is_some_and(|longhands| longhands.are_of_record(seeded)) => {
                GroupValuesWriter::seeded_for_child(
                    seeds[0],
                    seeds[1],
                    inherited_overrides(seeded).into_iter(),
                    flags,
                    initial.dependency_flags,
                )
            }
            _ => {
                let parent_longhands = parent_longhands.expect("the style engine answers every row");
                let inherited = crate::css::computed_values::GroupValueLayout::inherited_longhands()
                    .filter_map(|property| Some((property, parent_longhands.handle(property)?.clone())))
                    .chain(parent_longhands.inherited_overrides(parent_overlay));
                let mut writer =
                    GroupValuesWriter::seeded_for_child(seeds[1], seeds[1], inherited, flags, initial.dependency_flags);
                writer.set_effective_zoom(parent_longhands.effective_zoom());
                writer.set_effective_color_scheme(parent_longhands.effective_color_scheme());
                writer
            }
        };

        // The inputs every drive evaluates move their groups only when their values do.
        let seeded_bits = writer.bits();

        let box_type_input = self.box_type_transformation_input_with(
            records,
            facts,
            match is_pseudo_element {
                true => crate::css::style_compute::StyleAdjustmentTarget::PseudoElement,
                false => crate::css::style_compute::StyleAdjustmentTarget::Element,
            },
            parent.map(|(parent, _)| parent),
        );
        let is_th_element = facts & bridge::element_adjustment_fact::IS_TH != 0;
        // A container-relative length never computes directly, so only a drive resolves one.
        let mut container_units = 0;
        let (mut writer, length, font, author_disables_native_appearance) = if runs_drive {
            let store = WinnerStore::new(
                winners
                    .iter()
                    .zip(&substituted)
                    .rev()
                    .map(
                        |(
                            &Winner {
                                property,
                                important,
                                source,
                                index,
                            },
                            substituted,
                        )| {
                            let value = match (substituted, source, declared_only) {
                                (Some(value), ..) => WinnerValue::Substituted {
                                    value: value.clone(),
                                    source,
                                },
                                // No node holds the declarations of an element no rule reaches.
                                (None, WinnerSource::Element(kind), Some(own)) => WinnerValue::Substituted {
                                    value: own.declarations.declarations(kind).written(index).clone(),
                                    source,
                                },
                                (None, ..) => WinnerValue::Written { node, source, index },
                            };
                            let origin = cascade.priority_of(source, important).cascade_origin();
                            WinnerDeclaration::new(property, important, value, origin)
                        },
                    )
                    .collect(),
            );
            container_units = store.container_unit_mask(self);
            // https://drafts.csswg.org/css-values-5/#random-caching
            // A random function computes against the base value its random caching key draws
            // once. The keys not drawn yet are drawn between passes, and the element waits.
            let undrawn = store.undrawn_random_keys(self, node);
            if !undrawn.is_empty() {
                random_requests.extend(undrawn);
                return Err(Unanswered::Suspended(Suspension::RandomBaseValues));
            }
            let subject = DriveSubject {
                target: computed::ComputedStyleTarget::new(node, target_pseudo_kind),
                parent: parent.map(|(parent, _)| parent),
                facts,
            };
            let mut font_scratch = drive::FontDriveScratch::default();
            let driven = self.engine_drive_with(
                records,
                subject,
                writer,
                (!drives_everything).then_some(&selected[..]),
                &store,
                &inputs,
                &mut font_scratch,
            );
            if matches!(driven, Err(Unanswered::Suspended(Suspension::Font))) {
                *suspended_font = Some(font_scratch.take_suspended_request());
            }
            let drive::DrivenValues {
                mut writer,
                length,
                font,
            } = driven?;
            let font = font.expect("the style engine answers every row");
            let scheme = u8::try_from(writer.effective_color_scheme()).ok();
            // A drive of every longhand computed the winners that compute directly too.
            if !drives_everything
                && !crate::css::style_compute::compute_declared_values(
                    &mut writer,
                    easy.iter()
                        .map(|(property, value, dependencies)| (*property, &**value, *dependencies)),
                    &length,
                    scheme,
                    &box_type_input,
                    is_th_element,
                    false,
                    Some(&selected),
                    absolutized,
                    self.document_style_computation_inputs.device_pixels_per_css_pixel,
                )
            {
                unreachable!("the style engine answers every row");
            }
            let author_disables_native_appearance =
                crate::css::style_compute::has_author_value_disabling_native_appearance(&store.view(self));
            (writer, length, Some(font), author_disables_native_appearance)
        } else {
            // https://drafts.csswg.org/css-fonts/#font-size-prop
            // NB: The document element's font resolves against the document's initial font, as in
            //     the font phase of a drive.
            let parent_length = match parent_view.as_ref() {
                Some(parent_view) => {
                    Self::length_from_parent_font(&inputs, parent_view).expect("the style engine answers every row")
                }
                None => Self::length_from_initial_font(&inputs),
            };
            let parentless_font_size = parent_view
                .is_none()
                .then(|| crate::css::css_pixels::CssPixels::from_raw(inputs.initial_font_size_raw));
            let scheme = u8::try_from(writer.effective_color_scheme()).ok();
            // The font longhands compute against the parent's font, line-height against the
            // element's own font, and the other values against the element's own font and line
            // height.
            let compute_font =
                |values: &mut GroupValuesWriter<'_>,
                 line_height: bool,
                 length: &crate::css::style_compute::FfiLengthResolutionContext| {
                    crate::css::style_compute::compute_font_values(
                        values,
                        native_font
                            .iter()
                            .filter(|&&(property, _)| (property == prop::LINE_HEIGHT) == line_height)
                            .map(|(property, value)| (*property, &**value)),
                        length,
                        inputs.default_font_size_raw,
                        parentless_font_size,
                    )
                };
            if !compute_font(&mut writer, false, &parent_length) {
                unreachable!("the style engine answers every row");
            }
            // The element's font, where it resolves one, and the length context of its values.
            let (font, length) = match resolves_font {
                false => (None, parent_length),
                true => {
                    let (font, length) = self.resolve_own_font(
                        node,
                        &mut writer,
                        &inputs,
                        &parent_length,
                        parent_view.is_none(),
                        |values, length| compute_font(values, true, length),
                        suspended_font,
                    )?;
                    (Some(font), length)
                }
            };
            if !crate::css::style_compute::compute_declared_values(
                &mut writer,
                easy.iter()
                    .map(|(property, value, dependencies)| (*property, &**value, *dependencies)),
                &length,
                scheme,
                &box_type_input,
                is_th_element,
                true,
                None,
                absolutized,
                self.document_style_computation_inputs.device_pixels_per_css_pixel,
            ) {
                unreachable!("the style engine answers every row");
            }
            // NB: None of these winners reverts, so each is the cascaded value of its property.
            let author_disables_native_appearance = winners.iter().any(|winner| {
                crate::css::style_compute::PROPERTIES_DISABLING_NATIVE_APPEARANCE.contains(&winner.property)
                    && matches!(
                        cascade.priority_of(winner.source, winner.important).cascade_origin(),
                        crate::css::cascaded_properties::CascadeOrigin::Author
                            | crate::css::cascaded_properties::CascadeOrigin::AuthorPresentationalHint
                    )
            });
            (writer, length, font, author_disables_native_appearance)
        };
        // https://drafts.csswg.org/css-ui/#appearance-disabling-properties
        // NB: Only an element whose appearance is not none can have a native appearance to disable. Leaving the flag
        //     clear for every other element keeps their author backgrounds and borders from splitting style groups.
        let appearance_is_none = matches!(
            writer.get(prop::APPEARANCE).map(RetainedStyleValueData::data),
            Some(StyleValueData::Keyword { keyword }) if *keyword == crate::css::style_compute::keyword::NONE
        );
        writer.set_native_appearance_disabled(!appearance_is_none && author_disables_native_appearance);
        let required_moved = crate::css::style_compute::REQUIRED_DRIVER_INPUTS
            .iter()
            .filter(|&&property| writer.moved_from_seed(property, seeded_bits))
            .fold(0, |groups, &property| groups | moved_longhand_groups(property));
        let (values, mut flags, dependency_flags) = writer.finish(&seeds);
        // NB: The flags keep the cascaded font size, which the monospace recascade of a
        //     descendant reads, as a drive's table does.
        if let Some((_, font_size)) = native_font.iter().find(|&&(property, _)| property == prop::FONT_SIZE) {
            flags.set_raw_cascaded_font_size(Some(font_size.clone().into_owned()));
        }
        // NB: A value computed after it was inherited, such as an overflow keyword the other
        //     overflow longhand turns or a display the box adjustments change, is no longer
        //     inherited, as in a drive.
        let computed_value = |property: u16| values.value(property);
        // NB: Only a winner taking its parent's value is compared with it: no other is inherited.
        for &(property, inherits) in &direct_inheritance {
            let inherited = inherits
                && (table_row_bits(property) & LOGICAL_ALIAS_BIT != 0
                    || computed_value(property).optional_data()
                        == parent_value(property).and_then(RetainedStyleValueData::optional_data));
            flags.set_inherited(property, inherited);
        }
        // NB: An important declaration wins over the animations of its property, which read from
        //     the table whether it won. A drive marks the longhands it evaluated; the ones computing
        //     directly are marked here.
        for &Winner {
            property, important, ..
        } in winners
        {
            if important && table_row_bits(property) & LOGICAL_ALIAS_BIT == 0 {
                flags.set_important(property, true);
            }
        }
        groups |= required_moved;

        let parent_in_display_none_subtree = parent_view.as_ref().is_some_and(|view| {
            view.dependency_flags & crate::css::computed_longhand_table::IN_DISPLAY_NONE_SUBTREE != 0
        });
        let display_is_none = crate::css::style_compute::display_of(values.value(prop::DISPLAY)).is_none();
        let facts = super::pipeline::RecordFacts::of_values(
            |property| values.value(property),
            dependency_flags,
            parent_in_display_none_subtree || display_is_none,
        );
        // Every group but the font group builds from the element's group values.
        let group_values = crate::css::table_group_builder::LonghandSource::Values {
            values: &values,
            reads: None,
        };
        let color_inputs = crate::css::table_group_builder::assembly_color_inputs_from_source(&group_values, &length);
        let current_color = color_inputs.1;
        // Groups holding colors resolved against `currentcolor` resolve again where the element's
        // color differs from the one their shared payload resolved against.
        let current_color_groups = computed::ComputedGroupSets::specified_value_dependency_mask_of(
            values
                .facts()
                .inheritance_dependent_values()
                .filter(|(property, value)| {
                    !crate::css::table_group_builder::group_holds_current_color_for_its_reader(
                        *property,
                        value.optional_data(),
                    )
                }),
            crate::css::style_value::retained_value_depends_on_current_color,
        )
            | crate::css::table_group_builder::groups_reading_current_color_through_inherited_values(|property| {
                values.value(property).optional_data()
            });
        let inherited_groups = (1_u32 << ENGINE_INHERITED_GROUP_COUNT) - 1;
        let parent_color = parent.map_or(initial.color, |(_, color)| color);
        if parent_color != current_color {
            groups |= current_color_groups & inherited_groups;
        }
        if initial.color != current_color {
            groups |= current_color_groups & !inherited_groups;
        }
        // NB: The misc layout group holds whether an author declaration disables the native appearance, which no
        //     longhand's value says.
        if values.facts().native_appearance_disabled() {
            groups |= 1 << group_index::MISC_LAYOUT;
        }
        // The document element's groups all build against no parent.
        if parent_view.is_none() || drives_everything {
            groups = (1 << group_index::COUNT) - 1;
        }
        // Without a drive, the element's font is its parent's.
        if font.is_none() {
            groups &= !(1 << group_index::FONT);
        }

        // NB: A group that is not built and whose values are the shared group's own is the parent's
        //     or the initial one, taken without a reference of its own: a record sharing its set
        //     needs none, and one that does not counts it as it makes its own set.
        let shared_payload = |group: usize| match group < ENGINE_INHERITED_GROUP_COUNT {
            true => parent_view
                .as_ref()
                .map_or(std::ptr::null(), |view| view.payloads[group].as_ptr()),
            false => initial.payloads[group].as_ptr(),
        };
        let borrowed = crate::css::computed_values::BorrowedGroups::of_groups_where(|group| {
            let shared = shared_payload(group);
            groups & (1 << group) == 0 && !shared.is_null() && values.hold_the_seed(group, shared)
        });
        // The lookups of the groups assembled anew, which the cache takes the groups for once they
        // hold their values.
        let mut assembled: smallvec::SmallVec<[GroupLookup<'_>; 4]> = smallvec::SmallVec::new();
        let mut payloads = [std::ptr::null(); group_index::COUNT];
        for (group, payload) in payloads.iter_mut().enumerate() {
            let parent_payload = parent_view
                .as_ref()
                .map_or(std::ptr::null(), |view| view.payloads[group].as_ptr());
            *payload = if groups & (1 << group) != 0 {
                let key = (group != group_index::FONT && group_cache.worth_looking_for(group)).then(|| {
                    // A group holding plain `currentcolor` for its reader is the same whatever the
                    // element's color.
                    let reads_current_color = current_color_groups & (1 << group) != 0
                        || crate::css::table_group_builder::GROUPS_HOLDING_CURRENT_COLOR_FOR_THEIR_READER
                            & (1 << group)
                            == 0;
                    GroupCache::key(&values, group, color_inputs, reads_current_color, &length)
                });
                let found = key.as_ref().and_then(|key| group_cache.find(key));
                if key.is_some() {
                    match found {
                        Some(_) => group_cache.hits[group] += 1,
                        None => group_cache.misses[group] += 1,
                    }
                }
                match found {
                    // SAFETY: The cache holds the payload, which is a live background group.
                    Some(payload) if group == crate::css::table_group_builder::group_index::BACKGROUND => unsafe {
                        crate::css::table_group_builder::patch_background_color(
                            &group_values,
                            payload,
                            crate::css::computed_values::GroupShares::new(
                                &self.process_style.default_groups,
                                group,
                                crate::css::computed_values::GroupPayloadRef::new(group, parent_payload),
                            ),
                            color_inputs,
                            &length,
                        )
                    },
                    Some(payload) => {
                        crate::css::computed_values::retain_group_payload(group, payload);
                        payload
                    }
                    None => {
                        // SAFETY: The parent payloads are live for the parent's record.
                        let payload = unsafe {
                            crate::css::table_group_builder::assemble_group_from_source(
                                &group_values,
                                group,
                                font.as_ref(),
                                crate::css::computed_values::GroupShares::new(
                                    &self.process_style.default_groups,
                                    group,
                                    crate::css::computed_values::GroupPayloadRef::new(group, parent_payload),
                                ),
                                color_inputs,
                                &length,
                            )
                        };
                        assembled.extend(key);
                        payload
                    }
                }
            } else {
                let shared = match group < ENGINE_INHERITED_GROUP_COUNT && !parent_payload.is_null() {
                    true => parent_payload,
                    false => initial.payloads[group].as_ptr(),
                };
                if !borrowed.contains(group) {
                    crate::css::computed_values::retain_group_payload(group, shared);
                }
                shared
            };
        }
        if self.process_style.verifies_reads {
            Self::verify_value_built_groups(
                &values,
                &payloads,
                groups,
                color_inputs,
                &length,
                &self.process_style.default_groups,
            );
        }
        let before = payloads;
        // SAFETY: Each payload is a live payload of its group holding a reference for the element,
        //         but the borrowed ones, whose values are the very values the element holds.
        unsafe { values.put_into(&mut payloads) };
        for group in (0..group_index::COUNT).filter(|&group| borrowed.contains(group)) {
            assert_eq!(
                payloads[group], before[group],
                "a borrowed group holds the element's values"
            );
        }
        for lookup in &assembled {
            group_cache.insert(lookup, payloads[lookup.group]);
        }
        drop(assembled);
        let style_bits = values.style_bits();
        let value_facts = values.into_facts();
        Ok(ComputedElement {
            flags,
            facts,
            value_facts,
            // SAFETY: Each payload holds a reference for the element but the borrowed ones, which
            //         are groups of the parent's or the initial style the element shares.
            groups: unsafe {
                crate::css::computed_values::ComputedGroups::new(payloads, style_bits, current_color, borrowed)
            },
            directly: !runs_drive,
            element_dependent,
            custom_property_reads: super::custom_property_cascade::LonghandCustomPropertyReads::new(
                custom_property_reads,
                reads_unnamed_custom_properties,
            ),
            reads: super::records::ComputationReads {
                parent_reset_groups,
                reads_sibling_position,
                reads_root_font,
                reads_font_metric_lengths,
                environment_reads,
                reads_attributes: false,
                container_units,
                display_none_is_animated: false,
            },
            length: crate::css::style_compute::FfiLengthResolutionContext {
                resolved_viewport_relative_length: std::ptr::null_mut(),
                ..length
            },
            substitution_reads,
        })
    }

    /// Checks that each group `groups` names, as the element takes it, shared, found in the cache
    /// or assembled, is the group its values build anew, reading only what the cache keys cover.
    /// This is what LIBWEB_VERIFY_STYLE_CHUNKS asks of every element.
    fn verify_value_built_groups(
        values: &GroupValues<'_>,
        payloads: &[*const std::ffi::c_void; crate::css::table_group_builder::group_index::COUNT],
        groups: u32,
        (used_color_scheme, current_color): (u8, u32),
        length: &crate::css::style_compute::FfiLengthResolutionContext,
        defaults: &crate::css::computed_values::DefaultGroups,
    ) {
        use crate::css::table_group_builder::{LonghandSource, group_index, rebuild_group_from_source};
        for group in (0..group_index::COUNT).filter(|group| groups & (1 << group) != 0) {
            // The payload handles holding values hold the very values the element computed.
            // SAFETY: The element's payloads are live groups of their indices.
            let payload = unsafe { crate::css::computed_values::GroupPayloadRef::new(group, payloads[group]) }
                .expect("an element holds a group of each index");
            for (property, place) in GroupValueLayout::handles_of(group) {
                let (held, computed) = (
                    crate::css::computed_values::value_at(payload, place),
                    values.value(property),
                );
                if held.pointer() != computed.pointer() && held != computed {
                    eprintln!(
                        "style group {group} holds another value of longhand {property} than the element computed"
                    );
                    std::process::abort();
                }
            }
            // NB: The font group builds from platform font resources besides the values.
            let source = LonghandSource::Values {
                values,
                reads: Some(GroupValueLayout::get().reads_of(group)),
            };
            let Some(built) = rebuild_group_from_source(
                &source,
                group,
                crate::css::computed_values::GroupShares::new(defaults, group, None),
                current_color,
                used_color_scheme,
                Some(length),
            ) else {
                continue;
            };
            // SAFETY: Both payloads are live payloads of the group.
            let equal =
                unsafe { crate::css::computed_values::style_group_payloads_equal(group, built, payloads[group]) };
            crate::css::computed_values::release_group_payload(group, built);
            if !equal {
                eprintln!("style group {group} as the element takes it differs from the one its values build");
                std::process::abort();
            }
        }
    }

    /// The one value the engine substitutes a font longhand to, so that the font resolver, which
    /// knows a family and its feature values by their values' identities, finds what it resolved
    /// for them before.
    fn canonical_font_value(&self, property: u16, value: RetainedStyleValueData) -> RetainedStyleValueData {
        let mut values = self
            .substituted_font_values
            .lock()
            .expect("no thread panicked while holding the font values");
        if let Some((_, canonical)) = values
            .iter()
            .find(|(held_property, held)| *held_property == property && held.optional_data() == value.optional_data())
        {
            return canonical.clone();
        }
        values.push((property, value.clone()));
        value
    }

    /// What the engine computes a winner from: its written value, or what that substitutes to where
    /// it waits on substitution, and, where the value computes directly from a plain declaration
    /// of a longhand the engine computes directly, the value's external dependencies.
    #[allow(clippy::too_many_arguments)]
    fn classify_winner<'a>(
        &'a self,
        element_declarations: &[Option<(
            ElementDeclarationKind,
            crate::css::style::element_declarations::ElementDeclarations<'a>,
        )>; 3],
        custom_properties: super::custom_properties::CustomPropertyStores<'_>,
        property: u16,
        source: WinnerSource,
        index: usize,
    ) -> ClassifiedWinner<'a> {
        let (declaration, written, checks) = match source {
            WinnerSource::Rule(rule) => (
                *self
                    .rules
                    .declared_properties_of(rule)
                    .get(index)
                    .expect("the style engine answers every row"),
                self.rules
                    .written_values_of(rule)
                    .get(index)
                    .expect("the style engine answers every row"),
                self.rules.written_value_checks(rule, index),
            ),
            WinnerSource::Element(kind) => {
                let (_, declarations) = element_declarations
                    .iter()
                    .flatten()
                    .find(|(declarations_kind, _)| *declarations_kind == kind)
                    .expect("the style engine answers every row");
                (
                    *declarations
                        .declared_property(index)
                        .expect("the style engine answers every row"),
                    declarations.written(index),
                    declarations.checks(index),
                )
            }
        };
        if checks.waits_on_substitution() {
            use crate::css::style_value::StyleValueData;
            let value = match written.data() {
                // A longhand pending its shorthand's substitution takes its part of the substituted
                // shorthand.
                StyleValueData::PendingSubstitution {
                    original_shorthand,
                    original_shorthand_value,
                } => {
                    let (shorthand, shorthand_written) =
                        written_shorthand_of_pending(*original_shorthand, original_shorthand_value);
                    let resolved = self.substitute_value(custom_properties, shorthand, shorthand_written);
                    match resolved.data() {
                        StyleValueData::GuaranteedInvalid => resolved,
                        _ => expanded_longhand_value(shorthand, property, &resolved)
                            .unwrap_or_else(|| RetainedStyleValueData::from_owned(StyleValueData::GuaranteedInvalid)),
                    }
                }
                _ => self.substitute_value(custom_properties, property, written),
            };
            // https://drafts.csswg.org/css-variables-2/#invalid-at-computed-value-time
            // A declaration invalid at computed-value time computes as if `unset`, which the
            // element's seed already holds for every longhand it does not decide.
            // What `attr()`, `if()`, `inherit()` or a custom function substitutes is the element's
            // alone.
            let reads_subject = super::custom_property_cascade::value_reads_substitution_subject(written.data())
                || super::custom_property_cascade::value_reads_inherited_values(written.data());
            let substitution = super::custom_property_cascade::substitution_reads(written.data());
            let mut custom_properties = Vec::new();
            let unnamed_custom_properties = !super::custom_property_cascade::append_named_custom_property_reads(
                written.data(),
                &mut custom_properties,
            );
            if matches!(value.optional_data(), Some(StyleValueData::GuaranteedInvalid)) {
                return ClassifiedWinner {
                    value: ResolvedWinnerValue::Unset,
                    computation: WinnerComputation::Full,
                    reads: WinnerReads {
                        element: reads_subject,
                        substitution,
                        custom_properties,
                        unnamed_custom_properties,
                        ..WinnerReads::default()
                    },
                };
            }
            // https://drafts.csswg.org/css-cascade-5/#defaulting-keywords
            // A substitution that produces a CSS-wide keyword takes the keyword's value, as a
            // declaration of the keyword itself does.
            let operator = value
                .optional_data()
                .map(crate::css::declaration_block::declaration_operator);
            if let Some(operator @ (CascadeOperator::Inherit | CascadeOperator::Initial | CascadeOperator::Unset)) =
                operator
            {
                let parent_reset_value = operator != CascadeOperator::Initial
                    && !crate::css::property_metadata::property_is_inherited(property);
                return ClassifiedWinner {
                    value: ResolvedWinnerValue::Substituted(value),
                    computation: WinnerComputation::Full,
                    reads: WinnerReads {
                        element: reads_subject,
                        parent_reset_value,
                        css_wide_keyword: Some(operator),
                        sibling_position: false,
                        root_font: RootFontReads::None,
                        font_metrics: false,
                        environment: StyleEnvironment::default(),
                        substitution,
                        custom_properties,
                        unnamed_custom_properties,
                    },
                };
            }
            let value = match crate::css::style_compute::computes_in_font_phases(property) {
                true => self.canonical_font_value(property, value),
                false => value,
            };
            let computation = match declaration.operator() == CascadeOperator::Declared
                && value
                    .optional_data()
                    .is_some_and(|data| crate::css::style_compute::value_computes_directly(property, data, None))
            {
                true => WinnerComputation::Direct(None),
                false => WinnerComputation::Full,
            };
            let substituted_dependencies = value
                .optional_data()
                .map(crate::css::style_compute::try_external_value_dependencies);
            let element_dependent = reads_subject || substituted_dependencies.is_none_or(depends_on_element);
            let sibling_position = substituted_dependencies.is_none_or(reads_sibling_position);
            let root_font = reads_root_font(value.optional_data(), substituted_dependencies.flatten());
            let font_metrics = reads_font_metrics(value.optional_data(), substituted_dependencies.flatten());
            let environment = environment_reads(value.optional_data(), substituted_dependencies.flatten());
            let parent_reset_value = !crate::css::property_metadata::property_is_inherited(property)
                && value
                    .optional_data()
                    .is_some_and(crate::css::style_compute::value_is_css_wide_keyword);
            return ClassifiedWinner {
                value: ResolvedWinnerValue::Substituted(value),
                computation,
                reads: WinnerReads {
                    element: element_dependent,
                    parent_reset_value,
                    css_wide_keyword: None,
                    sibling_position,
                    root_font,
                    font_metrics,
                    environment,
                    substitution,
                    custom_properties,
                    unnamed_custom_properties,
                },
            };
        }
        let value = match written.data() {
            data @ crate::css::style_value::StyleValueData::Shorthand { .. } => {
                shorthand_longhand_data(property, data).expect("the style engine answers every row")
            }
            _ => written,
        };
        if declaration.operator() != CascadeOperator::Declared {
            let parent_reset_value = declaration.operator() == CascadeOperator::Inherit
                && !crate::css::property_metadata::property_is_inherited(property);
            return ClassifiedWinner {
                value: ResolvedWinnerValue::Written(value),
                computation: WinnerComputation::Full,
                reads: WinnerReads {
                    element: false,
                    parent_reset_value,
                    css_wide_keyword: Some(declaration.operator()),
                    sibling_position: false,
                    substitution: 0,
                    ..WinnerReads::default()
                },
            };
        }
        let prepared = checks.dependencies();
        let dependencies =
            (prepared & winner_store::DEPENDENCIES_COMPUTED != 0).then(|| winner_store::unpack_dependencies(prepared));
        let computation = match value
            .optional_data()
            .is_some_and(|data| crate::css::style_compute::value_computes_directly(property, data, dependencies))
        {
            true => WinnerComputation::Direct(dependencies),
            false => WinnerComputation::Full,
        };
        let value_dependencies = value
            .optional_data()
            .map(|data| dependencies.or_else(|| crate::css::style_compute::try_external_value_dependencies(data)));
        let element_dependent = value_dependencies.is_none_or(depends_on_element);
        let sibling_position = value_dependencies.is_none_or(reads_sibling_position);
        let root_font = reads_root_font(value.optional_data(), dependencies);
        let font_metrics = reads_font_metrics(value.optional_data(), dependencies);
        let environment = environment_reads(value.optional_data(), dependencies);
        ClassifiedWinner {
            value: ResolvedWinnerValue::Written(value),
            computation,
            reads: WinnerReads {
                element: element_dependent,
                parent_reset_value: false,
                css_wide_keyword: None,
                sibling_position,
                root_font,
                font_metrics,
                environment,
                substitution: 0,
                ..WinnerReads::default()
            },
        }
    }

    /// The font of an element whose font longhands computed natively into `values`, resolved as a
    /// drive resolves it, as its font group builds from it, with the length context of the
    /// element's own font and line height, the parent's being `parent_length`. Its line-height
    /// computes into `values` by `compute_line_height` in between. Suspends on a font the
    /// document's resolver has not resolved yet.
    #[allow(clippy::too_many_arguments)]
    fn resolve_own_font(
        &self,
        node: StyleNodeID,
        values: &mut GroupValuesWriter<'_>,
        inputs: &bridge::FfiDocumentStyleComputationInputs,
        parent_length: &crate::css::style_compute::FfiLengthResolutionContext,
        is_document_element: bool,
        compute_line_height: impl FnOnce(
            &mut GroupValuesWriter<'_>,
            &crate::css::style_compute::FfiLengthResolutionContext,
        ) -> bool,
        suspended_font: &mut Option<font_resolution::FontRequest>,
    ) -> Drive<(
        crate::css::table_group_builder::FfiFontGroupBuildInputs,
        crate::css::style_compute::FfiLengthResolutionContext,
    )> {
        let request = engine_sample::font_resolution_request(
            |property| values.get(property),
            inputs,
            || self.font_feature_values_scope(node),
        );
        let Some(resolved) = self
            .font_resolution
            .as_ref()
            .and_then(|resolutions| resolutions.lookup(request))
        else {
            *suspended_font = Some(font_resolution::FontRequest::new(request));
            return Err(Unanswered::Suspended(Suspension::Font));
        };
        let own_length = |line_height: f64| {
            let font_metrics = crate::css::style_compute::FfiFontMetrics {
                font_size: request.font_size(),
                x_height: drive::drive_font_metric(resolved.x_height),
                cap_height: drive::drive_font_metric(resolved.ascent),
                zero_advance: drive::drive_font_metric(resolved.zero_advance),
                line_height,
            };
            // NB: The document element's root-relative lengths resolve against its own font, as
            //     in the line-height and remaining phases of a drive.
            crate::css::style_compute::FfiLengthResolutionContext {
                font_metrics,
                root_font_metrics: match is_document_element {
                    true => font_metrics,
                    false => parent_length.root_font_metrics,
                },
                root_font_metrics_depend_on_viewport_metrics: match is_document_element {
                    true => parent_length.font_metrics_depend_on_viewport_metrics,
                    false => parent_length.root_font_metrics_depend_on_viewport_metrics,
                },
                ..*parent_length
            }
        };
        // NB: Line-height computes against the element's own font and its parent's line height,
        //     as in the line-height phase of a drive.
        if !compute_line_height(values, &own_length(parent_length.font_metrics.line_height)) {
            unreachable!("the style engine answers every row");
        }
        let line_height = engine_sample::used_line_height(|property| values.get(property), &request, &resolved);
        Ok((
            engine_sample::font_group_build_inputs(|property| values.get(property), &request, line_height, &resolved),
            own_length(line_height),
        ))
    }

    /// The length context the document element's font resolves against: the document's initial
    /// font, as its own and the root font.
    fn length_from_initial_font(
        inputs: &bridge::FfiDocumentStyleComputationInputs,
    ) -> crate::css::style_compute::FfiLengthResolutionContext {
        let initial_metrics = crate::css::style_compute::FfiFontMetrics {
            font_size: inputs.initial_font_size,
            x_height: inputs.initial_font_x_height,
            cap_height: inputs.initial_font_cap_height,
            zero_advance: inputs.initial_font_zero_advance,
            line_height: inputs.initial_line_height,
        };
        crate::css::style_compute::FfiLengthResolutionContext {
            viewport_width: inputs.viewport_width,
            viewport_height: inputs.viewport_height,
            font_metrics: initial_metrics,
            root_font_metrics: initial_metrics,
            font_metrics_depend_on_viewport_metrics: false,
            root_font_metrics_depend_on_viewport_metrics: false,
            has_container_width_basis: false,
            has_container_height_basis: false,
            container_width_basis: 0.0,
            container_height_basis: 0.0,
            container_width_basis_depends_on_viewport_metrics: false,
            container_height_basis_depends_on_viewport_metrics: false,
            subject_inline_axis_is_horizontal: true,
            resolved_viewport_relative_length: std::ptr::null_mut(),
            zoom: 1.0,
        }
    }

    /// The length context of an element whose font is its parent's, which it is where it declares
    /// no font longhand.
    fn length_from_parent_font(
        inputs: &bridge::FfiDocumentStyleComputationInputs,
        parent_view: &computed::StyleRecordView<'_>,
    ) -> Option<crate::css::style_compute::FfiLengthResolutionContext> {
        use crate::css::computed_value_types::FontValues;
        use crate::css::style_compute::{FfiFontMetrics, FfiLengthResolutionContext};
        use crate::css::table_group_builder::group_index;

        let parent_font_payload = parent_view.payloads[group_index::FONT].as_ptr();
        if parent_font_payload.is_null() {
            return None;
        }
        // SAFETY: The payload is the parent record's live font group.
        let parent_font = unsafe { &*parent_font_payload.cast::<FontValues>() };
        let first_available = parent_font.first_available_font_metrics();
        Some(FfiLengthResolutionContext {
            viewport_width: inputs.viewport_width,
            viewport_height: inputs.viewport_height,
            font_metrics: FfiFontMetrics {
                font_size: parent_font.font_size.to_double(),
                x_height: drive::drive_font_metric(first_available.x_height),
                cap_height: drive::drive_font_metric(first_available.ascent),
                zero_advance: drive::drive_font_metric(first_available.zero_advance),
                line_height: parent_font.line_height_used().to_double(),
            },
            root_font_metrics: FfiFontMetrics {
                font_size: inputs.root_font_size,
                x_height: inputs.root_font_x_height,
                cap_height: inputs.root_font_cap_height,
                zero_advance: inputs.root_font_zero_advance,
                line_height: inputs.root_line_height,
            },
            font_metrics_depend_on_viewport_metrics: parent_view.dependency_flags
                & FONT_METRICS_DEPEND_ON_VIEWPORT_METRICS
                != 0,
            root_font_metrics_depend_on_viewport_metrics: inputs.root_font_metrics_depend_on_viewport_metrics,
            has_container_width_basis: false,
            has_container_height_basis: false,
            container_width_basis: 0.0,
            container_height_basis: 0.0,
            container_width_basis_depends_on_viewport_metrics: false,
            container_height_basis_depends_on_viewport_metrics: false,
            subject_inline_axis_is_horizontal: parent_view.payloads.inherited_bits().writing_mode()
                == crate::css::css_enums::writing_mode::HORIZONTAL_TB,
            resolved_viewport_relative_length: std::ptr::null_mut(),
            zoom: 1.0,
        })
    }
}

/// What the engine computes a winner from.
/// The engine: the group payloads a thread assembled, by what each was assembled from, so an element
/// whose values for a group are the ones another's were takes that payload rather than assembling
/// its own. A group's payload is a function of the values of the longhands the group reads, which
/// include every value the group holds, and of its color, color scheme, zoom, box display and
/// length context; two values equal by content are the same value.
#[derive(Default)]
pub(in crate::css::style) struct GroupCache {
    entries: HashMap<u64, smallvec::SmallVec<[GroupCacheEntry; 1]>>,
    count: usize,
    /// How often each group was found and missed since the counts last started over. A group
    /// whose payloads are rarely the same, such as one holding each element's own inline style,
    /// is assembled without looking for it until the counts start over.
    hits: [u32; crate::css::table_group_builder::group_index::COUNT],
    misses: [u32; crate::css::table_group_builder::group_index::COUNT],
    lookups: u32,
    /// The longhands and recorded specified values the groups of the entries read in place of
    /// computed values, compared by identity, one run per entry, which the keys name by where it
    /// starts. The entries hold them, as they outlive the pass they came from and no other value
    /// may take their addresses. Most entries have none.
    key_values: Vec<(u16, crate::css::style_value::RetainedStyleValueData)>,
    /// The distinct inputs of the entries, which the keys name by index. The elements of a pass
    /// mostly assemble their groups against the same ones.
    key_inputs: Vec<[u64; 18]>,
}

struct GroupCacheEntry {
    group: usize,
    key: GroupKey,
    payload: *const std::ffi::c_void,
}

/// How many groups' values a group's build reads at most, as the groups hold them today.
const READ_GROUPS_INLINE: usize = 3;

/// What an entry's payload was assembled from.
struct GroupKey {
    /// The values of the groups the group read any of, in group order, which the entry keeps.
    groups: smallvec::SmallVec<[crate::css::computed_values::HeldGroup; READ_GROUPS_INLINE]>,
    /// The recorded specified values the group read in place of computed values, as the run of
    /// the cache's key values starting here and this long.
    values_start: u32,
    values_length: u32,
    /// The index of the inputs in the cache's key inputs.
    inputs: u32,
}

/// What a group's payload is looked for by: the element's values the group reads, which are only
/// kept in a [`GroupKey`] where the payload goes into the cache.
struct GroupLookup<'a> {
    hash: u64,
    group: usize,
    values: &'a crate::css::computed_values::GroupValues<'a>,
    reads: crate::css::computed_values::GroupReads,
    inputs: [u64; 18],
}

impl GroupLookup<'_> {
    /// The longhands and recorded specified values the group reads in place of computed values.
    fn values_read_in_place(
        &self,
    ) -> impl Iterator<Item = (u16, &crate::css::style_value::RetainedStyleValueData)> + '_ {
        self.values.facts().values_read_in_place(&self.reads)
    }

    fn matches(&self, group: usize, key: &GroupKey, cache: &GroupCache) -> bool {
        let start = key.values_start as usize;
        group == self.group
            && cache.key_inputs[key.inputs as usize] == self.inputs
            && self.values.holds_read_values(&self.reads, &key.groups)
            && cache.key_values[start..start + key.values_length as usize]
                .iter()
                .map(|(property, value)| (*property, value.pointer()))
                .eq(self
                    .values_read_in_place()
                    .map(|(property, value)| (property, value.pointer())))
    }
}

// SAFETY: The cache holds counted references to immutable groups, which any thread may read and
//         release, and only the thread styling with it reaches it.
unsafe impl Send for GroupCache {}

impl GroupCache {
    /// How many payloads a thread keeps before it starts over.
    const CAPACITY: usize = 4096;

    /// How many payloads a thread keeps from one pass to the next: what a restyle of a few
    /// elements finds again, without a large page's groups outliving its pass.
    const KEPT_ACROSS_PASSES: usize = 256;

    /// Counts the cache's tables and the groups only it holds toward the worker caches.
    pub(in crate::css::style) fn report_memory(&self, report: &mut crate::css::style::memory_report::MemoryReport) {
        use crate::css::style::memory_report::MemoryOwner;
        use crate::heap_size::{map_bytes, vec_bytes};
        for entry in self.entries.values().flatten() {
            if report.sizer.first_sight(entry.payload) {
                let bytes = crate::css::computed_values::group_payload_allocation_bytes(entry.group, entry.payload);
                report.add(MemoryOwner::WorkerCaches, bytes);
            }
        }
        report.add(
            MemoryOwner::WorkerCaches,
            map_bytes(&self.entries) + vec_bytes(&self.key_values) + vec_bytes(&self.key_inputs),
        );
    }

    /// This cache, as a thread keeps it for its next pass: as it is where it holds few payloads,
    /// else empty.
    pub(in crate::css::style) fn kept(mut self) -> Self {
        if self.count > Self::KEPT_ACROSS_PASSES {
            self.clear();
        }
        self
    }

    /// How many lookups the hit counts cover before they start over.
    const COUNT_PERIOD: u32 = 4096;

    /// Whether looking for a payload of `group` is worth its cost.
    fn worth_looking_for(&mut self, group: usize) -> bool {
        self.lookups += 1;
        if self.lookups == Self::COUNT_PERIOD {
            self.lookups = 0;
            self.hits = Default::default();
            self.misses = Default::default();
        }
        self.misses[group] < 32 || self.hits[group] * 4 >= self.misses[group]
    }

    fn key<'a>(
        values: &'a crate::css::computed_values::GroupValues<'a>,
        group: usize,
        (scheme, current_color): (u8, u32),
        reads_current_color: bool,
        length: &crate::css::style_compute::FfiLengthResolutionContext,
    ) -> GroupLookup<'a> {
        use crate::css::computed_values::{GroupValueLayout, LonghandID};
        use std::hash::{Hash, Hasher};
        let metrics = |metrics: &crate::css::style_compute::FfiFontMetrics| {
            [
                metrics.font_size,
                metrics.x_height,
                metrics.cap_height,
                metrics.zero_advance,
                metrics.line_height,
            ]
            .map(f64::to_bits)
        };
        let [a, b, c, d, e] = metrics(&length.font_metrics);
        let [f, g, h, i, j] = metrics(&length.root_font_metrics);
        let facts = values.facts();
        let inputs = [
            u64::from(scheme),
            if reads_current_color {
                u64::from(current_color)
            } else {
                0
            },
            u64::from(facts.display_before_box_type_transformation())
                | (u64::from(facts.native_appearance_disabled()) << u32::BITS),
            u64::from(facts.effective_zoom().to_bits()),
            length.viewport_width.to_bits(),
            length.viewport_height.to_bits(),
            length.zoom.to_bits(),
            u64::from(length.subject_inline_axis_is_horizontal),
            a,
            b,
            c,
            d,
            e,
            f,
            g,
            h,
            i,
            j,
        ];
        let mut reads = *GroupValueLayout::get().reads_of(group);
        // A background group's payload for one element is another's with the background color
        // patched, where the two hold the same values of every other longhand the group reads.
        // The key of the group leaves the longhand out.
        if group == crate::css::table_group_builder::group_index::BACKGROUND {
            let color = GroupValueLayout::get().place(LonghandID::new(
                crate::css::property_metadata::property_id::BACKGROUND_COLOR,
            ));
            reads[color.group()] &= !(1 << color.index());
        }
        let mut hasher = crate::fast_hash::fast_hasher();
        group.hash(&mut hasher);
        values.read_hash(&reads, &mut hasher);
        for (property, value) in facts.values_read_in_place(&reads) {
            hasher.write_u16(property);
            hasher.write_usize(value.pointer() as usize);
        }
        inputs.hash(&mut hasher);
        GroupLookup {
            hash: hasher.finish(),
            group,
            values,
            reads,
            inputs,
        }
    }

    /// The payload a group with `key` was assembled to, which the cache holds a reference to.
    fn find(&self, lookup: &GroupLookup<'_>) -> Option<*const std::ffi::c_void> {
        let entry = self
            .entries
            .get(&lookup.hash)?
            .iter()
            .find(|entry| lookup.matches(entry.group, &entry.key, self))?;
        Some(entry.payload)
    }

    fn insert(&mut self, lookup: &GroupLookup<'_>, payload: *const std::ffi::c_void) {
        if self.count == Self::CAPACITY {
            self.clear();
        }
        let group = lookup.group;
        crate::css::computed_values::retain_group_payload(group, payload);
        if self.key_inputs.last() != Some(&lookup.inputs) {
            self.key_inputs.push(lookup.inputs);
        }
        let values_start = self.key_values.len();
        self.key_values.extend(
            lookup
                .values_read_in_place()
                .map(|(property, value)| (property, value.clone())),
        );
        let key = GroupKey {
            groups: lookup.values.held_read_groups(&lookup.reads).collect(),
            values_start: values_start as u32,
            values_length: (self.key_values.len() - values_start) as u32,
            inputs: (self.key_inputs.len() - 1) as u32,
        };
        self.entries
            .entry(lookup.hash)
            .or_default()
            .push(GroupCacheEntry { group, key, payload });
        self.count += 1;
    }

    fn clear(&mut self) {
        for entry in self.entries.drain().flat_map(|(_, entries)| entries) {
            crate::css::computed_values::release_group_payload(entry.group, entry.payload);
        }
        self.key_values.clear();
        self.key_inputs.clear();
        self.count = 0;
    }
}

impl Drop for GroupCache {
    fn drop(&mut self) {
        self.clear();
    }
}

/// The engine: a winner computing directly, with its value and the external dependencies prepared
/// for it.
type DirectWinner<'a> = (
    u16,
    std::borrow::Cow<'a, RetainedStyleValueData>,
    Option<crate::css::style_compute::ExternalValueDependencies>,
);

/// The engine: what an element's group values start from.
#[derive(Clone, Copy)]
pub(in crate::css::style) struct GroupSeeds<'a> {
    /// The groups the element's values share where they hold the same values: its parent's, then
    /// the initial ones.
    pub(in crate::css::style) seeds: [crate::css::computed_values::GroupSeed<'a>; 2],
    /// The record whose groups the first seed is, or none where it is the initial style's.
    pub(in crate::css::style) parent: Option<&'a std::sync::Arc<super::pipeline::Record>>,
}

/// The initial values as a drive computes them, with a reference to each payload.
pub(in crate::css::style) struct ComputedInitialStyle {
    pub(in crate::css::style) payloads: [*const std::ffi::c_void; crate::css::table_group_builder::group_index::COUNT],
    pub(in crate::css::style) value_facts: crate::css::computed_values::GroupValueFacts,
    pub(in crate::css::style) flags: crate::css::computed_longhand_table::LonghandTableFlags,
    pub(in crate::css::style) dependency_flags: u8,
    pub(in crate::css::style) color: u32,
    pub(in crate::css::style) font: super::traversal::InitialFont,
}

/// One element's computed style from the engine.
pub(in crate::css::style) struct ComputedElement {
    /// The flags the element's records keep beside its values.
    pub(in crate::css::style) flags: crate::css::computed_longhand_table::LonghandTableFlags,
    /// What the element's records read of its style without its longhand table.
    pub(in crate::css::style) facts: super::pipeline::RecordFacts,
    /// What the element's groups were built from beside their values, which its groups hold.
    pub(in crate::css::style) value_facts: crate::css::computed_values::GroupValueFacts,
    /// The element's groups, which its record takes, borrowing those of the sets of its parent's
    /// or the initial style it shares.
    pub(in crate::css::style) groups: crate::css::computed_values::ComputedGroups,
    /// Whether every winner computed directly, without the longhand drive.
    pub(in crate::css::style) directly: bool,
    /// Whether a winner computed against the element itself, so the style is the element's alone.
    pub(in crate::css::style) element_dependent: bool,
    /// The custom properties the winners substitute.
    pub(in crate::css::style) custom_property_reads: super::custom_property_cascade::LonghandCustomPropertyReads,
    /// What else the computation read.
    pub(in crate::css::style) reads: super::records::ComputationReads,
    /// What the element's lengths resolve against.
    pub(in crate::css::style) length: crate::css::style_compute::FfiLengthResolutionContext,
    /// What the winners' substitutions read beyond the element's custom properties, as
    /// `FfiNodeRecordReads` bits.
    pub(in crate::css::style) substitution_reads: u8,
}

/// A winner with its value resolved, how that computes and what it reads.
struct ClassifiedWinner<'a> {
    value: ResolvedWinnerValue<'a>,
    computation: WinnerComputation,
    reads: WinnerReads,
}

/// How a winner's value computes.
#[derive(Clone, Copy)]
enum WinnerComputation {
    /// With the rest of the element's values, in the full computation.
    Full,
    /// On its own, with the dependencies that were prepared for it, if any.
    Direct(Option<crate::css::style_compute::ExternalValueDependencies>),
}

/// What a winner's computed value reads beyond the element's inherited context.
#[derive(Default)]
pub(in crate::css::style) struct WinnerReads {
    /// Something of the element itself.
    element: bool,
    /// The CSS-wide keyword the winner was declared with.
    css_wide_keyword: Option<CascadeOperator>,
    /// The parent's value of a property that is not inherited.
    parent_reset_value: bool,
    /// The element's place among its siblings.
    sibling_position: bool,
    /// How much of the root element's font, through root-relative lengths.
    root_font: RootFontReads,
    /// A metric of a first available font, through a font-relative length.
    font_metrics: bool,
    /// What of the document's environment the winner's value resolves against.
    environment: StyleEnvironment,
    /// What the winner's substitution read beyond the element's custom properties, as
    /// `FfiNodeRecordReads` bits.
    substitution: u8,
    /// The custom properties the winner's substitution read by name.
    custom_properties: Vec<std::sync::Arc<[u16]>>,
    /// Whether the winner's substitution may read custom properties it does not name.
    unnamed_custom_properties: bool,
}

/// Whether a value computes against something of the element beyond its parent's style and its own
/// declarations: its place among its siblings, its own random values or its containers. A value
/// whose dependencies are unknown is taken to.
/// Whether a value computes against the element's place among its siblings, through a
/// tree-counting function. A value whose dependencies are unknown is taken to.
fn reads_sibling_position(dependencies: Option<crate::css::style_compute::ExternalValueDependencies>) -> bool {
    dependencies.is_none_or(|dependencies| dependencies.uses_tree_counting_function)
}

/// How much of the root element's font `value` resolves lengths against, by its `dependencies`
/// where they are known. Root-relative lengths are found by walking the value, which needs no
/// answer to whether the value is computationally independent.
fn reads_root_font(
    value: Option<&StyleValueData>,
    dependencies: Option<crate::css::style_compute::ExternalValueDependencies>,
) -> RootFontReads {
    value.map_or(RootFontReads::None, |value| {
        dependencies
            .unwrap_or_else(|| crate::css::style_compute::collect_external_value_dependencies(value))
            .root_font_reads
    })
}

/// Whether `value` resolves a length against a metric of the first available font of the element or
/// its parent, by its `dependencies` where they are known.
fn reads_font_metrics(
    value: Option<&StyleValueData>,
    dependencies: Option<crate::css::style_compute::ExternalValueDependencies>,
) -> bool {
    value.is_some_and(|value| {
        dependencies
            .unwrap_or_else(|| crate::css::style_compute::collect_external_value_dependencies(value))
            .font_metric_relative_length
    })
}

/// What of the document's environment `value` resolves against, by its `dependencies` where they
/// are known: the used color scheme, through `light-dark()` or a system color, the device pixel
/// ratio, through `image-set()`, and the axes of the viewport its viewport-relative lengths
/// resolve against.
fn environment_reads(
    value: Option<&StyleValueData>,
    dependencies: Option<crate::css::style_compute::ExternalValueDependencies>,
) -> StyleEnvironment {
    let Some(value) = value else {
        return StyleEnvironment::default();
    };
    let dependencies =
        dependencies.unwrap_or_else(|| crate::css::style_compute::collect_external_value_dependencies(value));
    let mut reads = StyleEnvironment::default();
    if dependencies.color_scheme_dependent {
        reads |= StyleEnvironment::COLOR_SCHEME;
    }
    if dependencies.device_pixel_ratio_dependent {
        reads |= StyleEnvironment::DEVICE_PIXEL_RATIO;
    }
    if dependencies.viewport_axes & crate::css::style_compute::VIEWPORT_WIDTH_AXIS != 0 {
        reads |= StyleEnvironment::VIEWPORT_WIDTH;
    }
    if dependencies.viewport_axes & crate::css::style_compute::VIEWPORT_HEIGHT_AXIS != 0 {
        reads |= StyleEnvironment::VIEWPORT_HEIGHT;
    }
    reads
}

/// What of the document's environment the computed value of `property` reads whatever its value:
/// `color-scheme` computes the used color scheme, and a border, outline or column rule width is
/// snapped to device pixels.
fn property_environment_reads(property: u16) -> StyleEnvironment {
    use crate::css::property_metadata::property_id as prop;
    if property == prop::COLOR_SCHEME {
        return StyleEnvironment::COLOR_SCHEME;
    }
    if crate::css::style_compute::property_snaps_as_border_width(property) {
        return StyleEnvironment::DEVICE_PIXEL_RATIO;
    }
    StyleEnvironment::default()
}

fn depends_on_element(dependencies: Option<crate::css::style_compute::ExternalValueDependencies>) -> bool {
    dependencies.is_none_or(|dependencies| {
        dependencies.uses_tree_counting_function
            || dependencies.uses_random_function
            || dependencies.container_relative_length_unit_mask != 0
    })
}

/// What the engine's cascade of an element holds below its winners, for a winner whose substitution
/// produces `revert` or `revert-layer`.
pub(in crate::css::style) trait CascadeBelow {
    /// The priority of the block a winner of the cascade came from.
    fn priority_of(&self, source: WinnerSource, important: bool) -> CascadePriority;

    /// The highest declaration of `property` in the cascade that none of the reverting declarations,
    /// at their priorities with the keywords they substituted to, roll back past, with its priority.
    fn winner_below(
        &self,
        property: u16,
        reverted: &[(CascadePriority, CascadeOperator)],
    ) -> Option<(CascadePriority, Winner)>;

    /// The own declarations of an element no rule reaches, which its winners name in place of
    /// those of the node they are keyed by, where the cascade is of those alone.
    fn declared_only(&self) -> Option<&super::pipeline::DeclaredOnly>;
}

pub(in crate::css::style) enum ResolvedWinnerValue<'a> {
    Written(&'a RetainedStyleValueData),
    Substituted(RetainedStyleValueData),
    /// The declaration is invalid at computed-value time.
    Unset,
}

impl ResolvedWinnerValue<'_> {
    fn value(&self) -> Option<&RetainedStyleValueData> {
        match self {
            Self::Written(value) => Some(value),
            Self::Substituted(value) => Some(value),
            Self::Unset => None,
        }
    }

    fn into_substituted(self) -> Option<RetainedStyleValueData> {
        match self {
            Self::Written(_) | Self::Unset => None,
            Self::Substituted(value) => Some(value),
        }
    }
}

/// Where a winner rolled back below a revert decides: at the declaration it rolled back to, by
/// its block's priority and its index in the block, or at the revert, which rolled back as
/// `operator`.
struct RolledBack {
    place: (CascadePriority, usize),
    revert: (CascadePriority, usize),
    operator: CascadeOperator,
}

/// Whether `other` is the logical or physical counterpart of `property` in some writing mode and
/// direction.
fn is_logical_counterpart(property: u16, other: u16) -> bool {
    use crate::css::style_compute::{
        DIRECTION_COUNT, LOGICAL_ALIAS_BIT, PHYSICAL_TO_LOGICAL_BIT, WRITING_MODE_COUNT, map_logical_alias_to_physical,
        map_physical_to_logical_alias, table_row_bits,
    };
    let bits = table_row_bits(property);
    if bits & (LOGICAL_ALIAS_BIT | PHYSICAL_TO_LOGICAL_BIT) == 0 {
        return false;
    }
    (0..WRITING_MODE_COUNT as u8).any(|writing_mode| {
        (0..DIRECTION_COUNT as u8).any(|direction| {
            other
                == match bits & LOGICAL_ALIAS_BIT != 0 {
                    true => map_logical_alias_to_physical(property, writing_mode, direction),
                    false => map_physical_to_logical_alias(property, writing_mode, direction),
                }
        })
    })
}

/// What a drive computes for: the node whose record it inherits from (the flat-tree parent, or
/// the originating element of a pseudo-element) and the element facts the computation's
/// adjustments read.
#[derive(Clone, Copy)]
pub(in crate::css::style) struct DriveSubject {
    /// What is being driven, element or pseudo-element. A drive that suspends to wait for a font
    /// names it, so the suspended drive can only be resumed by the one it belongs to.
    pub(in crate::css::style) target: computed::ComputedStyleTarget,
    /// The flat-tree parent the element inherits from; the document element has none and
    /// inherits from the initial values.
    pub(in crate::css::style) parent: Option<StyleNodeID>,
    pub(in crate::css::style) facts: u32,
}

/// The element facts a pseudo-element's computation reads: the C++ adjustments for what the
/// originating element is stay off for its pseudo-elements, past the ones about its markup
/// language.
const PSEUDO_ELEMENT_ADJUSTMENT_FACTS: u32 = {
    use bridge::element_adjustment_fact as fact;
    fact::IS_MATHML
        | fact::IS_MATHML_MTABLE
        | fact::IS_MATHML_MTR
        | fact::IS_MATHML_MTD
        | fact::IS_TH
        | fact::HAS_ANIMATIONS
};

/// The outermost shorthand a longhand pending substitution takes its part of, and the value that
/// shorthand was written with. A shorthand nested in another, such as `border-width` in `border`,
/// is itself pending the outer one.
fn written_shorthand_of_pending(
    shorthand: u16,
    value: &crate::css::style_value::RetainedStyleValueData,
) -> (u16, &crate::css::style_value::RetainedStyleValueData) {
    match value.data() {
        crate::css::style_value::StyleValueData::PendingSubstitution {
            original_shorthand,
            original_shorthand_value,
        } => written_shorthand_of_pending(*original_shorthand, original_shorthand_value),
        _ => (shorthand, value),
    }
}

/// The longhand's part of a substituted shorthand value, as the cascade expands it.
fn expanded_longhand_value(
    shorthand: u16,
    property: u16,
    value: &crate::css::style_value::RetainedStyleValueData,
) -> Option<crate::css::style_value::RetainedStyleValueData> {
    let mut found = None;
    crate::css::style_compute::expand_shorthands_with(shorthand, value, false, &mut |longhand, data, _| {
        if longhand == property && found.is_none() {
            found = Some(data.clone());
        }
    });
    found
}

fn shorthand_longhand_value(
    property: u16,
    data: &crate::css::style_value::StyleValueData,
) -> Option<crate::css::style_value::RetainedStyleValueData> {
    let crate::css::style_value::StyleValueData::Shorthand(shorthand_payload) = data else {
        return None;
    };
    let ShorthandPayload {
        sub_properties, values, ..
    } = &**shorthand_payload;
    for (&sub_property, sub_value) in sub_properties.as_slice().iter().zip(values.as_slice()) {
        if sub_property == property {
            return Some(sub_value.clone());
        }
        if let Some(found) = shorthand_longhand_value(property, sub_value.data()) {
            return Some(found);
        }
    }
    None
}

/// Whether a longhand is read by the box-type transformation, which rewrites the computed display.
/// A `-webkit-box` becomes a block container while its `-webkit-box-orient` and `continue` say
/// that `-webkit-line-clamp` applies to it.
fn property_feeds_box_type_transformation(property: u16) -> bool {
    use crate::css::property_metadata::property_id as prop;
    matches!(
        property,
        prop::DISPLAY | prop::POSITION | prop::FLOAT | prop::_WEBKIT_BOX_ORIENT | prop::CONTINUE
    )
}

/// Whether a longhand's new value would start an animation or a transition in the C++
/// computation, register anchor names there, or feed the counter-style environment identity it
/// resolves.
/// The longhands the drive's font resolution selects a font by, which its request carries.
fn font_resolution_selects_by(property: u16) -> bool {
    use crate::css::property_metadata::property_id as prop;
    matches!(
        property,
        prop::FONT_FAMILY
            | prop::FONT_STYLE
            | prop::FONT_WEIGHT
            | prop::FONT_WIDTH
            | prop::FONT_OPTICAL_SIZING
            | prop::FONT_SIZE_ADJUST
    )
}

/// The computed style groups a moved longhand reaches. One the font group carries feeds no group of
/// its own and reaches every value the font feeds, through the font group: a delta moving it is
/// driven in full, as is one moving a longhand no registered group names.
fn moved_longhand_groups(property: u16) -> u32 {
    use crate::css::table_group_builder::group_index;
    // NB: The computation writes a longhand the bits beside the groups hold into the bits
    //     themselves: it reaches only the groups other than the one its readers see it in.
    if crate::css::computed_values::StyleBitField::of(property).is_some() {
        return StyleGroupMasks::get().dependencies(property)
            & !crate::css::computed_values::computed_group_output_mask(property).unwrap_or(0);
    }
    match StyleGroupMasks::get().dependencies(property) {
        0 if font_group_carries_longhand(property) => 1 << group_index::FONT,
        0 => (1 << crate::css::table_group_builder::group_index::COUNT) - 1,
        groups => groups,
    }
}

/// The font-phase longhands the font group carries without a group binding of their own.
fn font_group_carries_longhand(property: u16) -> bool {
    use crate::css::property_metadata::property_id as prop;
    font_resolution_selects_by(property)
        || matches!(
            property,
            prop::FONT_FEATURE_SETTINGS
                | prop::FONT_KERNING
                | prop::FONT_LANGUAGE_OVERRIDE
                | prop::FONT_VARIANT_ALTERNATES
                | prop::FONT_VARIANT_CAPS
                | prop::FONT_VARIANT_EAST_ASIAN
                | prop::FONT_VARIANT_EMOJI
                | prop::FONT_VARIANT_LIGATURES
                | prop::FONT_VARIANT_NUMERIC
                | prop::FONT_VARIANT_POSITION
                | prop::FONT_VARIATION_SETTINGS
                | prop::MATH_DEPTH
                | prop::MATH_SHIFT
                | prop::MATH_STYLE
                | prop::TEXT_RENDERING
        )
}

/// Whether a value resolves against the element itself: a `random()` it draws, or a
/// container-relative length its query containers measure.
fn value_resolves_against_the_element(dependencies: &crate::css::style_compute::ExternalValueDependencies) -> bool {
    dependencies.uses_random_function
        || dependencies.has_unfixed_random_sharing
        || dependencies.container_relative_length_unit_mask != 0
}

/// Whether a written value computes from the record, the parent and the document's computation
/// inputs alone: no custom-property substitution, and none of the element or sheet facts the C++
/// computation gathers per drive.
fn value_computes_without_document_context(value: &StyleValueData) -> bool {
    // A longhand a shorthand written with a substitution declares holds a pending substitution
    // until the shorthand resolves.
    !matches!(
        value,
        StyleValueData::Unresolved { .. } | StyleValueData::PendingSubstitution { .. }
    ) && crate::css::style_compute::value_is_computationally_independent(value).is_some_and(|_| {
        let dependencies = crate::css::style_compute::external_value_dependencies(value);
        !value_resolves_against_the_element(&dependencies)
            && !dependencies.needs_document_base_url
            && !dependencies.may_need_style_sheet_resource_context
            && !dependencies.uses_tree_counting_function
    })
}

/// The proof that a record reads no resource context: no value its winners give it resolves a
/// `url()` against a style sheet's or the document's base URL. The engine keeps only such records
/// across transactions, so a move of those base URLs, which every `history.pushState()` makes
/// without a `<base>`, leaves nothing it keeps to forget.
#[derive(Clone, Copy)]
pub(in crate::css::style) struct ReadsNoResourceContexts(pub(in crate::css::style) ());

/// Static checks attached to the original declaration spelling at input preparation, in one word:
/// the external dependencies of the value a longhand computes from, packed the way a winner
/// memoizes them (bits 0-20 and 31), or zero for a value that waits on substitution; whether the
/// whole value computes without document context (bit 21); and whether its longhand does (bit 22),
/// where it does not wait on substitution (bit 23 clear).
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(transparent)]
pub(in crate::css::style) struct WrittenValueChecks(u32);

const _: () = assert!(size_of::<WrittenValueChecks>() == 4);
const _: () = assert!(
    winner_store::DEPENDENCY_BITS
        & (WrittenValueChecks::WHOLE_CONTEXT_FREE
            | WrittenValueChecks::LONGHAND_CONTEXT_FREE
            | WrittenValueChecks::WAITS_ON_SUBSTITUTION)
        == 0
);

impl WrittenValueChecks {
    const WHOLE_CONTEXT_FREE: u32 = 1 << 21;
    const LONGHAND_CONTEXT_FREE: u32 = 1 << 22;
    const WAITS_ON_SUBSTITUTION: u32 = 1 << 23;

    /// The packed external dependencies of the value, when they were prepared.
    pub(in crate::css::style) fn dependencies(self) -> u32 {
        self.0 & winner_store::DEPENDENCY_BITS
    }

    /// Whether the value waits on `var()` or `attr()` substitution before it computes.
    pub(in crate::css::style) fn waits_on_substitution(self) -> bool {
        self.0 & Self::WAITS_ON_SUBSTITUTION != 0
    }

    pub(in crate::css::style) fn prepare(
        property: u16,
        value: &crate::css::style_value::RetainedStyleValueData,
    ) -> Self {
        use crate::css::style_value::StyleValueData;
        let whole_context_free = value_computes_without_document_context(value.data());
        let longhand_context_free = match value.data() {
            StyleValueData::Unresolved { .. } | StyleValueData::PendingSubstitution { .. } => None,
            StyleValueData::Shorthand { .. } => Some(
                shorthand_longhand_value(property, value.data())
                    .is_some_and(|value| value_computes_without_document_context(value.data())),
            ),
            _ => Some(whole_context_free),
        };
        // The longhand a shorthand declared whole contributes is what a winner computes from.
        let dependencies = match value.data() {
            StyleValueData::Unresolved { .. } | StyleValueData::PendingSubstitution { .. } => 0,
            StyleValueData::Shorthand { .. } => shorthand_longhand_value(property, value.data())
                .filter(|value| !matches!(value.data(), StyleValueData::Unresolved { .. }))
                .and_then(|value| crate::css::style_compute::try_external_value_dependencies(value.data()))
                .map_or(0, winner_store::pack_dependencies),
            data => crate::css::style_compute::try_external_value_dependencies(data)
                .map_or(0, winner_store::pack_dependencies),
        };
        debug_assert_eq!(dependencies & !winner_store::DEPENDENCY_BITS, 0);
        let mut bits = dependencies;
        if whole_context_free {
            bits |= Self::WHOLE_CONTEXT_FREE;
        }
        match longhand_context_free {
            None => bits |= Self::WAITS_ON_SUBSTITUTION,
            Some(true) => bits |= Self::LONGHAND_CONTEXT_FREE,
            Some(false) => {}
        }
        Self(bits)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pending_longhand_names_the_outermost_written_shorthand() {
        use crate::css::property_metadata::property_id;
        use crate::css::style_value::{RetainedStyleValueData, StyleValueData};

        for (shorthand, nested_shorthand) in [
            (property_id::BACKGROUND, property_id::BACKGROUND_POSITION),
            (property_id::BORDER, property_id::BORDER_WIDTH),
            (property_id::FONT, property_id::FONT_VARIANT),
        ] {
            let written = RetainedStyleValueData::from_owned(StyleValueData::Number { value: 1.0 });
            let nested = RetainedStyleValueData::from_owned(StyleValueData::PendingSubstitution {
                original_shorthand: shorthand,
                original_shorthand_value: written.clone_retained(),
            });
            let (found_shorthand, found_value) = written_shorthand_of_pending(nested_shorthand, &nested);
            assert_eq!(found_shorthand, shorthand);
            assert!(std::ptr::eq(found_value.pointer(), written.pointer()));
            let (found_shorthand, found_value) = written_shorthand_of_pending(shorthand, &written);
            assert_eq!(found_shorthand, shorthand);
            assert!(std::ptr::eq(found_value.pointer(), written.pointer()));
        }
    }
}
