/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The host's plain writes and reads of a document's style engine, one row each in the table below. A write row names
//! the [`StyleChange`] its entry queues for the document's render state, which applies the change in the order the host
//! made them; a read is answered in place while the host waits. A row is exported as `style_engine_` and its name, and
//! does what the engine method of that name does unless the row says otherwise.
//!
//! cbindgen does not expand macros, so the C++ declarations of the entries are written by hand in
//! `CSS/StyleEngineBridge.h`, and a test checks each against its row.

use super::ReplacedContentInput;
use super::StyleEngine;
use super::bridge::{ElementBoxKind, FfiAppliedAnimationDefinition, borrow};
use super::engine_calls::with_engine;
use super::index::StyleAtomID;
use super::program::{CascadeLayerID, SheetID};
use super::tree::{StyleNodeID, TableSpans, TreeScopeID};
use crate::abort_on_panic;
use crate::css::css_string::CssString;
use crate::render_state::{ArenaChange, BegunRead, DocumentHost};

/// An array the host lends an entry for the call, as C++ passes an AK `Span`.
#[repr(C)]
pub struct FfiSpan<T> {
    data: *const T,
    size: usize,
}

/// A value of a row: what its entry takes, the C++ type the entry's declaration names, and what a change holds of it.
pub(crate) trait Carried: Sized {
    type Ffi;
    type Held;
    #[cfg(test)]
    const CPP: &'static str;

    /// # Safety
    ///
    /// An array the value names must be live for the call.
    unsafe fn carry(value: Self::Ffi) -> Self::Held;

    /// The value applying the change hands the engine, or none where the change has nothing to apply.
    fn applied(held: Self::Held) -> Option<Self>;
}

macro_rules! carried {
    ($($type:ty: $ffi:ty as $cpp:literal = |$value:ident| $carry:expr;)+) => {
        $(impl Carried for $type {
            type Ffi = $ffi;
            type Held = Self;
            #[cfg(test)]
            const CPP: &'static str = $cpp;

            unsafe fn carry($value: $ffi) -> Self {
                $carry
            }

            fn applied(held: Self) -> Option<Self> {
                Some(held)
            }
        })+
    };
}

// SAFETY: An entry carries what its caller lends live for the call.
carried! {
    bool: bool as "bool" = |value| value;
    u8: u8 as "u8" = |value| value;
    u32: u32 as "u32" = |value| value;
    u64: u64 as "u64" = |value| value;
    i64: u64 as "u64" = |value| value as i64;
    usize: usize as "size_t" = |value| value;
    StyleAtomID: u32 as "StyleAtomID" = |atom| StyleAtomID(atom);
    TreeScopeID: u32 as "TreeScopeID" = |tree_scope| TreeScopeID(tree_scope);
    CascadeLayerID: u32 as "u32" = |layer| CascadeLayerID(layer);
    ElementBoxKind: u8 as "u8" = |box_kind| ElementBoxKind::from_raw(box_kind);
    Option<StyleNodeID>: u32 as "StyleNodeID" = |node| StyleNodeID::from_raw(node);
    Option<SheetID>: u32 as "SheetID" = |sheet| sheet.checked_sub(1).map(SheetID);
    [u32; 4]: *const [u32; 4] as "u32 const*" = |values| unsafe { *values };
    Box<[u16]>: FfiSpan<u16> as "ReadonlySpan<u16>" = |span| unsafe { borrow(span.data, span.size) }.into();
    // The names of fly strings the host lends, as their raw identities, which a change copies.
    Box<[CssString]>: FfiSpan<usize> as "ReadonlySpan<FlatPtr>" =
        |span| unsafe { borrow(span.data, span.size) }.iter().map(|&raw| unsafe { CssString::from_borrowed_raw(raw) }).collect();
    Box<[u32]>: FfiSpan<u32> as "ReadonlySpan<u32>" = |span| unsafe { borrow(span.data, span.size) }.into();
    Box<[u64]>: FfiSpan<u64> as "ReadonlySpan<u64>" = |span| unsafe { borrow(span.data, span.size) }.into();
    Box<[StyleAtomID]>: FfiSpan<u32> as "ReadonlySpan<StyleAtomID>" =
        |span| unsafe { borrow(span.data, span.size) }.iter().copied().map(StyleAtomID).collect();
    Box<[StyleNodeID]>: FfiSpan<u32> as "ReadonlySpan<StyleNodeID>" =
        |span| unsafe { borrow(span.data, span.size) }.iter().copied().filter_map(StyleNodeID::from_raw).collect();
    Box<[FfiAppliedAnimationDefinition]>: FfiSpan<FfiAppliedAnimationDefinition>
        as "ReadonlySpan<FfiAppliedAnimationDefinition>" = |span| unsafe { borrow(span.data, span.size) }.into();
    FfiSpan<u16>: FfiSpan<u16> as "ReadonlySpan<u16>" = |span| span;
    FfiSpan<u32>: FfiSpan<u32> as "ReadonlySpan<StyleNodeID>" = |span| span;
}

// A change whose style node or sheet the host names as none has nothing to apply.
macro_rules! required {
    ($($type:ty),+) => {
        $(impl Carried for $type {
            type Ffi = u32;
            type Held = Option<Self>;
            #[cfg(test)]
            const CPP: &'static str = <Option<Self>>::CPP;

            unsafe fn carry(value: u32) -> Option<Self> {
                // SAFETY: An identity borrows nothing.
                unsafe { <Option<Self>>::carry(value) }
            }

            fn applied(held: Option<Self>) -> Option<Self> {
                held
            }
        })+
    };
}

required!(StyleNodeID, SheetID);

macro_rules! style_boundary {
    (@apply $engine:ident $method:ident($($value:ident),*)) => {
        $engine.$method($($value),*)
    };
    (@apply $engine:ident $method:ident($($value:ident),*) $apply:expr) => {
        $apply
    };
    (
        |$engine:ident|
        writes { $($write:ident => $Change:ident $({ $($field:ident: $type:ty),+ })? $(=> $apply:expr)?;)+ }
        reads { $($read:ident($($argument:ident: $argument_type:ty),*) -> $answer:ty $(=> $ask:expr)?;)+ }
    ) => {
        /// A write of the host to the document's style engine, which the render state applies to it.
        pub(crate) enum StyleChange {
            $($Change $({ $($field: <$type as Carried>::Held),+ })?,)+
        }

        impl StyleChange {
            pub(crate) fn apply(self, $engine: &mut StyleEngine) {
                match self {
                    $(Self::$Change $({ $($field),+ })? => {
                        $($(let Some($field) = <$type>::applied($field) else { return };)+)?
                        style_boundary!(@apply $engine $write($($($field),+)?) $($apply)?);
                    })+
                }
            }
        }

        $(
            #[unsafe(export_name = concat!("style_engine_", stringify!($write)))]
            pub unsafe extern "C" fn $write(host: &DocumentHost $($(, $field: <$type as Carried>::Ffi)+)?) {
                abort_on_panic(|| {
                    // SAFETY: Guaranteed by the caller.
                    let change = StyleChange::$Change $({ $($field: unsafe { <$type>::carry($field) }),+ })?;
                    host.queue_change(ArenaChange::Style(change));
                });
            }
        )+

        $(
            #[unsafe(export_name = concat!("style_engine_", stringify!($read)))]
            pub unsafe extern "C" fn $read(
                host: &DocumentHost,
                read: &BegunRead
                $(, $argument: <$argument_type as Carried>::Ffi)*
            ) -> $answer {
                abort_on_panic(|| {
                    // SAFETY: Guaranteed by the caller.
                    $(let $argument = unsafe { <$argument_type>::carry($argument) };)*
                    with_engine(read, host, |$engine| style_boundary!(@apply $engine $read($($argument),*) $($ask)?))
                })
            }
        )+

        /// The C++ declaration of each entry.
        #[cfg(test)]
        fn cpp_declarations() -> impl Iterator<Item = String> {
            let writes = [$(("void", stringify!($write), "", vec![$($((<$type>::CPP, stringify!($field))),+)?])),+];
            let reads = [$((
                <$answer>::CPP,
                stringify!($read),
                ", BegunRead const*",
                vec![$((<$argument_type>::CPP, stringify!($argument))),*],
            )),+];
            writes.into_iter().chain(reads).map(|(answer, entry, read, parameters)| {
                let parameters: String = parameters.iter().map(|(cpp, name)| format!(", {cpp} {name}")).collect();
                format!("{answer} style_engine_{entry}(DocumentHost const*{read}{parameters});")
            })
        }
    };
}

style_boundary! {
    |engine|

    writes {
        set_pseudo_element_style_deferred => SetPseudoElementStyleDeferred { kind: u8, deferred: bool } =>
            if deferred {
                engine.deferred_pseudo_elements |= 1 << kind;
            } else {
                engine.deferred_pseudo_elements &= !(1 << kind);
            };
        set_fold_id_and_class_name_case => SetFoldIdAndClassNameCase { fold: bool };
        set_html_element_namespace => SetHtmlElementNamespace { namespace_atom: StyleAtomID };
        mark_relation_only_style_node => MarkRelationOnlyStyleNode { node: StyleNodeID };
        link_style_nodes_in_dom_order => LinkStyleNodesInDomOrder { links: Box<[u32]> } =>
            engine.link_style_nodes_in_dom_order(&links);
        unlink_style_node_from_dom_order => UnlinkStyleNodeFromDomOrder {
            node: StyleNodeID, parent: Option<StyleNodeID>
        };
        retire_text_style_nodes => RetireTextStyleNodes { nodes: Box<[StyleNodeID]> };
        set_slot_assigned_nodes => SetSlotAssignedNodes { slot: StyleNodeID, assigned: Box<[StyleNodeID]> } =>
            engine.set_slot_assigned_nodes(slot, &assigned);
        set_top_layer_elements => SetTopLayerElements { members: Box<[StyleNodeID]> } =>
            engine.set_top_layer_elements(&members);
        set_text_is_ascii_whitespace => SetTextIsAsciiWhitespace { node: StyleNodeID, value: bool };
        set_text_is_in_user_agent_shadow_tree => SetTextIsInUserAgentShadowTree { node: StyleNodeID, value: bool };
        set_text_is_password_input => SetTextIsPasswordInput { node: StyleNodeID, value: bool };
        set_node_dom_paint_facts => SetNodeDomPaintFacts { node: StyleNodeID, facts: u8 };
        set_element_unique_node_id => SetElementUniqueNodeId { node: StyleNodeID, unique_node_id: i64 };
        set_element_table_spans => SetElementTableSpans {
            node: StyleNodeID, column_span: u32, row_span: u32, raw_column_span: u32
        } =>
            engine.set_element_table_spans(node, TableSpans {
                column_span: u16::try_from(column_span).expect("a column span is clamped to 1000"),
                row_span: u16::try_from(row_span).expect("a row span is clamped to 65534"),
                raw_column_span,
            });
        set_element_id_name => SetElementIdName { node: StyleNodeID, name: StyleAtomID };
        set_shadow_root => SetShadowRoot { shadow_host: StyleNodeID, shadow_root: StyleNodeID };
        record_environment_change => RecordEnvironmentChange;
        record_custom_property_registration_change => RecordCustomPropertyRegistrationChange { name: StyleAtomID };
        flush => Flush => engine.flush_without_document_root();
        finish_sheet_rules_replacement => FinishSheetRulesReplacement { sheet: SheetID, declaration_block: u32 };
        attach_sheet => AttachSheet { sheet: SheetID, tree_scope: TreeScopeID, before_sheet: Option<SheetID> } =>
            engine.attach_sheet_before_sheet(sheet, before_sheet, tree_scope);
        detach_sheet => DetachSheet { sheet: SheetID, tree_scope: TreeScopeID };
        attach_sheet_occurrence => AttachSheetOccurrence {
            sheet: SheetID, tree_scope: TreeScopeID, identity: u64, before: u64, conditions_hold: bool
        };
        detach_sheet_occurrence => DetachSheetOccurrence { tree_scope: TreeScopeID, identity: u64 };
        set_sheet_occurrence_conditions => SetSheetOccurrenceConditions {
            tree_scope: TreeScopeID, identity: u64, conditions_hold: bool
        };
        set_element_part_exposure => SetElementPartExposure { node: StyleNodeID, exposure: StyleAtomID };
        set_element_directionality => SetElementDirectionality { node: StyleNodeID, directionality: StyleAtomID };
        set_element_custom_states => SetElementCustomStates { node: StyleNodeID, states: Box<[StyleAtomID]> } =>
            engine.set_element_custom_states(node, &states);
        end_deferred_geometry_transaction_flush => EndDeferredGeometryTransactionFlush;
        begin_cold_matching_batch => BeginColdMatchingBatch { root: StyleNodeID };
        begin_adaptive_cold_matching_batch => BeginAdaptiveColdMatchingBatch { root: StyleNodeID };
        end_cold_matching_batch => EndColdMatchingBatch;
        record_container_query_input => RecordContainerQueryInput { node: StyleNodeID };
        note_children_explicitly_inherit => NoteChildrenExplicitlyInherit { node: StyleNodeID };
        record_derived_element_style_input => RecordDerivedElementStyleInput {
            node: StyleNodeID, reaction: u8, inherited_style_groups: u8
        };
        record_font_input_changes => RecordFontInputChanges {
            family_name_lengths: Box<[u32]>, family_name_units: Box<[u16]>, font_lists: Box<[u64]>
        } => engine.record_font_input_changes(&family_name_lengths, &family_name_units, &font_lists);
        record_flat_tree_descendant_style_inputs => RecordFlatTreeDescendantStyleInputs {
            root: StyleNodeID, reaction: u8, inherited_style_groups: u8
        };
        consume_element_style_input => ConsumeElementStyleInput { node: StyleNodeID };
        note_style_reaction_applied => NoteStyleReactionApplied {
            node: StyleNodeID, reaction: u8, inherited_style_groups_changed: u8, facts: u32
        };
        set_element_adjustment_facts => SetElementAdjustmentFacts { node: StyleNodeID, facts: u32 };
        set_element_construction_facts => SetElementConstructionFacts { node: StyleNodeID, facts: u32 };
        set_element_box_kind => SetElementBoxKind { node: StyleNodeID, box_kind: ElementBoxKind };
        set_element_replaced_content_input => SetElementReplacedContentInput {
            node: StyleNodeID, kind: u8, present: u8, values: [u32; 4]
        } => engine.set_element_replaced_content_input(node, ReplacedContentInput::from_raw(kind, present, values));
        set_element_heading_level => SetElementHeadingLevel { node: StyleNodeID, level: u8 };
        acknowledge_engine_computed_record => AcknowledgeEngineComputedRecord { node: StyleNodeID };
        abandon_demanded_records => AbandonDemandedRecords { node: StyleNodeID };
        record_transition_baseline => RecordTransitionBaseline {
            node: StyleNodeID, pseudo_kind: u8, style_record: u64
        };
        release_transition_baselines => ReleaseTransitionBaselines;
        pin_style_record => PinStyleRecord { style_record: u64 };
        unpin_style_record => UnpinStyleRecord { style_record: u64 };
        begin_style_record_view_epoch => BeginStyleRecordViewEpoch;
        end_style_record_view_epoch => EndStyleRecordViewEpoch;
        set_tree_scope_uses_document_sheets => SetTreeScopeUsesDocumentSheets { tree_scope: TreeScopeID };
        set_element_custom_property_names => SetElementCustomPropertyNames {
            node: StyleNodeID, name_atoms: Box<[StyleAtomID]>, uses_unnamed: bool, uses_custom_functions: bool
        } => engine.set_element_custom_property_names(node, &name_atoms, uses_unnamed, uses_custom_functions);
        set_element_animation_names => SetElementAnimationNames { node: StyleNodeID, name_atoms: Box<[StyleAtomID]> } =>
            engine.set_element_animation_names(node, &name_atoms);
        set_element_recomputes_on_environment_move => SetElementRecomputesOnEnvironmentMove {
            node: StyleNodeID, recomputes: bool
        };
        set_element_scroll_state => SetElementScrollState {
            node: StyleNodeID, stuck: u8, snapped: u8, scrollable: u8, scrolled: u8
        };
        set_element_size_container_query_facts => SetElementSizeContainerQueryFacts {
            node: StyleNodeID, is_queried_container: bool, depends_on_size_container_query: bool
        };
        note_size_container_needs_evaluation_after_layout => NoteSizeContainerNeedsEvaluationAfterLayout {
            node: StyleNodeID
        };
        record_size_container_query_dependents => RecordSizeContainerQueryDependents { container: StyleNodeID };
        evaluate_size_containers_needing_evaluation_after_layout => EvaluateSizeContainersNeedingEvaluationAfterLayout;
        set_element_associated_pseudo_kind => SetElementAssociatedPseudoKind {
            node: StyleNodeID, pseudo_kind_plus_one: u8
        };
        set_counter_style_environment_identity => SetCounterStyleEnvironmentIdentity {
            tree_scope: TreeScopeID, identity: u64
        };
        set_held_style_record => SetHeldStyleRecord { node: StyleNodeID, style_record: u64 };
        set_element_css_defined_animations => SetElementCssDefinedAnimations {
            node: StyleNodeID, slot: u8, names: Box<[CssString]>, definitions: Box<[FfiAppliedAnimationDefinition]>
        } => engine.set_element_css_defined_animations(node, slot, names, &definitions);
        set_tree_scope_root => SetTreeScopeRoot { tree_scope: TreeScopeID, root: StyleNodeID };
        set_sheet_conditions_hold => SetSheetConditionsHold { sheet: SheetID, conditions_hold: bool };
    }

    reads {
        connected_element_count() -> u32;
        has_suspended_style_pass() -> bool;
        pending_transaction_may_affect_layout_geometry() -> bool;
        defer_pending_transaction_for_geometry_read() -> bool;
        begin_deferred_geometry_transaction_flush() -> bool;
        has_deferred_geometry_transaction() -> bool;
        match_document(root: Option<StyleNodeID>) -> usize =>
            root.and_then(|root| engine.match_document(root).ok()).unwrap_or(usize::MAX);
        layer_index(tree_scope: TreeScopeID, layer: CascadeLayerID) -> u32;
        node_declares_custom_properties(node: Option<StyleNodeID>) -> bool =>
            node.is_some_and(|node| engine.node_declares_custom_properties(node));
        size_query_container_scan_visits(reset: bool) -> u64;
        ensure_random_base_value(node: Option<StyleNodeID>, name: FfiSpan<u16>, element_shared: bool) -> u64 => {
            // SAFETY: The host lends the name for the call.
            let name = unsafe { borrow(name.data, name.size) };
            engine.ensure_random_base_value(node, name, element_shared).to_bits()
        };
    }
}

impl StyleChange {
    /// Whether the change only keeps the engine from reclaiming records: it pins or unpins one, or begins or ends an
    /// epoch of style record views. It changes nothing the paint properties are prepared from.
    pub(crate) fn only_keeps_records_alive(&self) -> bool {
        matches!(
            self,
            Self::BeginStyleRecordViewEpoch
                | Self::EndStyleRecordViewEpoch
                | Self::PinStyleRecord { .. }
                | Self::UnpinStyleRecord { .. }
        )
    }

    /// Whether the change may move a fact the host knows of the render state. One that keeps records alive, notes what
    /// a text spells, picks the pseudo-element whose style is deferred, or begins or ends a cold matching batch never
    /// does: none of them stages a style input or touches a layout box.
    pub(crate) fn may_move_facts(&self) -> bool {
        !self.only_keeps_records_alive()
            && !matches!(
                self,
                Self::SetTextIsAsciiWhitespace { .. }
                    | Self::SetPseudoElementStyleDeferred { .. }
                    | Self::BeginColdMatchingBatch { .. }
                    | Self::BeginAdaptiveColdMatchingBatch { .. }
                    | Self::EndColdMatchingBatch
            )
    }
}

#[cfg(test)]
mod tests {
    /// cbindgen cannot see the entries a macro expands to, so their C++ declarations are written by hand.
    #[test]
    fn style_engine_bridge_declares_each_entry_as_its_row() {
        let header = include_str!("../../../../CSS/StyleEngineBridge.h");
        for declaration in super::cpp_declarations() {
            assert!(
                header.contains(&declaration),
                "StyleEngineBridge.h must declare {declaration}"
            );
        }
    }
}
