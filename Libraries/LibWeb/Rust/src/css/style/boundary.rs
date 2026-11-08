/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The host's plain writes and reads of a document's style engine, one row each in the table below. A write row names
//! the [`StyleChange`] its entry queues for the document's render state, which applies the change in the order the host
//! made them; a read is answered in place while the host waits. A row of `writes` or `reads` is exported as
//! `style_engine_` and its name, and does what the engine method of that name does unless the row says otherwise.
//!
//! cbindgen does not expand macros, so the C++ declarations of the entries are written by hand in
//! `CSS/StyleEngineBridge.h`, and a test checks each against its row.

use super::ReplacedContentInput;
use super::StyleEngine;
use super::animations::HeldAppliedAnimationDefinition;
use super::bridge::{ElementBoxKind, FfiAppliedAnimationDefinition, borrow};
use super::bridge::{FfiFrameSetBorder, FfiFrameSetDimension};
use super::engine_calls::EngineWrite;
use super::engine_calls::with_engine;
use super::index::StyleAtomID;
use super::pipeline::rule_store::{CascadeLayerID, SheetID};
use super::tree::{FrameSetLayout, StyleNodeID, TableSpans, TreeScopeID};
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
    (): () as "void" = |value| value;
    bool: bool as "bool" = |value| value;
    u8: u8 as "u8" = |value| value;
    u32: u32 as "u32" = |value| value;
    u64: u64 as "u64" = |value| value;
    i64: u64 as "u64" = |value| value as i64;
    usize: usize as "size_t" = |value| value;
    StyleAtomID: u32 as "StyleAtomID" = |atom| StyleAtomID(atom);
    TreeScopeID: u32 as "TreeScopeID" = |tree_scope| TreeScopeID(tree_scope);
    CascadeLayerID: u32 as "u32" = |layer| CascadeLayerID(layer);
    ElementBoxKind: ElementBoxKind as "u8" = |box_kind| box_kind;
    Option<StyleNodeID>: u32 as "StyleNodeID" = |node| StyleNodeID::from_raw(node);
    Option<SheetID>: u32 as "SheetID" = |sheet| sheet.checked_sub(1).map(SheetID);
    [u32; 4]: *const [u32; 4] as "u32 const*" = |values| unsafe { *values };
    Box<[u16]>: FfiSpan<u16> as "ReadonlySpan<u16>" = |span| unsafe { borrow(span.data, span.size) }.into();
    // The names of fly strings the host lends, as their raw identities, which a change copies.
    Box<[CssString]>: FfiSpan<usize> as "ReadonlySpan<FlatPtr>" =
        |span| unsafe { borrow(span.data, span.size) }.iter().map(|&raw| unsafe { CssString::from_borrowed_raw(raw) }).collect();
    Box<[u32]>: FfiSpan<u32> as "ReadonlySpan<u32>" = |span| unsafe { borrow(span.data, span.size) }.into();
    Box<[u64]>: FfiSpan<u64> as "ReadonlySpan<u64>" = |span| unsafe { borrow(span.data, span.size) }.into();
    Box<[FfiFrameSetDimension]>: FfiSpan<FfiFrameSetDimension> as "ReadonlySpan<FfiFrameSetDimension>" =
        |span| unsafe { borrow(span.data, span.size) }.into();
    Box<[FfiFrameSetBorder]>: FfiSpan<FfiFrameSetBorder> as "ReadonlySpan<FfiFrameSetBorder>" =
        |span| unsafe { borrow(span.data, span.size) }.into();
    Box<[StyleAtomID]>: FfiSpan<u32> as "ReadonlySpan<StyleAtomID>" =
        |span| unsafe { borrow(span.data, span.size) }.iter().copied().map(StyleAtomID).collect();
    Box<[StyleNodeID]>: FfiSpan<u32> as "ReadonlySpan<StyleNodeID>" =
        |span| unsafe { borrow(span.data, span.size) }.iter().copied().filter_map(StyleNodeID::from_raw).collect();
    Box<[HeldAppliedAnimationDefinition]>: FfiSpan<FfiAppliedAnimationDefinition>
        as "ReadonlySpan<FfiAppliedAnimationDefinition>" =
        |span| unsafe { borrow(span.data, span.size) }.iter().map(|&definition| unsafe { HeldAppliedAnimationDefinition::retained(definition) }).collect();
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

/// A record the host holds, which the change holds a reference to of its own. A change naming none has nothing to apply.
impl Carried for std::sync::Arc<super::pipeline::Record> {
    type Ffi = *const std::ffi::c_void;
    type Held = Option<Self>;
    #[cfg(test)]
    const CPP: &'static str = "void const*";

    unsafe fn carry(record: *const std::ffi::c_void) -> Option<Self> {
        // SAFETY: The host holds a reference to the record for the call.
        unsafe { super::bridge::retain_host_record(record) }
    }

    fn applied(held: Option<Self>) -> Option<Self> {
        held
    }
}

/// The record a host's element installed, or none, which the change holds a reference to of its own.
pub(crate) struct HeldHostRecord(Option<std::sync::Arc<super::pipeline::Record>>);

impl Carried for HeldHostRecord {
    type Ffi = *const std::ffi::c_void;
    type Held = Self;
    #[cfg(test)]
    const CPP: &'static str = "void const*";

    unsafe fn carry(record: *const std::ffi::c_void) -> Self {
        // SAFETY: The host holds a reference to the record for the call.
        Self(unsafe { super::bridge::retain_host_record(record) })
    }

    fn applied(held: Self) -> Option<Self> {
        Some(held)
    }
}

/// The record a host's pseudo-element let go of as it installed another, or none, which the change holds a reference
/// to until it applies: what the render side reads of the pseudo-element by its identity until then, as the layout
/// node's move, stays the record it names.
pub(crate) struct ReleasedHostRecord(
    #[allow(dead_code, reason = "held until the change applies")] Option<std::sync::Arc<super::pipeline::Record>>,
);

impl Carried for ReleasedHostRecord {
    type Ffi = *const std::ffi::c_void;
    type Held = Self;
    #[cfg(test)]
    const CPP: &'static str = "void const*";

    unsafe fn carry(record: *const std::ffi::c_void) -> Self {
        // SAFETY: The host holds a reference to the record for the call.
        Self(unsafe { super::bridge::retain_host_record(record) })
    }

    fn applied(held: Self) -> Option<Self> {
        Some(held)
    }
}

/// The fills of the faces that loaded into the frozen cascades that waited on them. The host lends each cascade a load
/// filled faces into followed by the cascade the newer @font-face table resolves its request to, both live for the
/// call, and the change carries what filling the first toward the second takes.
pub(crate) struct LoadedFontFaceFills {
    /// The addresses of the cascades the fills fill in, as the styles naming them hold them.
    pub(crate) font_lists: Box<[u64]>,
    pub(crate) fills: Vec<libgfx_rust::font::FrozenFontFills>,
}

impl Carried for LoadedFontFaceFills {
    type Ffi = FfiSpan<u64>;
    type Held = Self;
    #[cfg(test)]
    const CPP: &'static str = "ReadonlySpan<u64>";

    unsafe fn carry(span: FfiSpan<u64>) -> Self {
        // SAFETY: The host lends the addresses for the call.
        let addresses = unsafe { borrow(span.data, span.size) };
        let mut font_lists = Vec::new();
        let mut fills = Vec::new();
        for &[list, resolved] in addresses.as_chunks::<2>().0 {
            // SAFETY: The host keeps both cascades live for the call.
            let fill = unsafe {
                libgfx_rust::font::FrozenFontFills::toward_cascade(
                    list as usize as *const std::ffi::c_void,
                    resolved as usize as *const std::ffi::c_void,
                )
            };
            if let Some(fill) = fill {
                font_lists.push(list);
                fills.push(fill);
            }
        }
        Self {
            font_lists: font_lists.into(),
            fills,
        }
    }

    fn applied(held: Self) -> Option<Self> {
        Some(held)
    }
}

macro_rules! style_boundary {
    (@apply $engine:ident $method:ident($($value:ident),*)) => {
        $engine.$method($($value),*)
    };
    (@apply $engine:ident $method:ident($($value:ident),*) $apply:expr) => {
        $apply
    };
    (
        |$engine:ident|
        writes {
            $($write:ident => $(@$published:ident)? $Change:ident $({ $($field:ident: $type:ty),+ })? $(=> $apply:expr)?;)+
        }
        |$host:ident|
        host_writes { $($host_write:ident { $($host_field:ident: $host_type:ty),+ } => $host_apply:expr;)+ }
        reads { $($read:ident($($argument:ident: $argument_type:ty),*) -> $answer:ty $(=> $ask:expr)?;)+ }
    ) => {
        /// A write of the host or of Rust to the document's style engine, which the render state applies to it.
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
                    // NB: A change that reads the projection rows as the host wrote them takes what it wrote first.
                    $(let _ = stringify!($published); host.publish_element_writes();)?
                    host.queue_change(ArenaChange::Style(change));
                });
            }
        )+

        $(
            #[unsafe(export_name = concat!("style_engine_", stringify!($host_write)))]
            pub unsafe extern "C" fn $host_write($host: &DocumentHost $(, $host_field: <$host_type as Carried>::Ffi)+) {
                abort_on_panic(|| {
                    // SAFETY: Guaranteed by the caller.
                    $(let Some($host_field) = <$host_type>::applied(unsafe { <$host_type>::carry($host_field) }) else {
                        return;
                    };)+
                    $host_apply
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
            let host_writes = [$((
                "void",
                stringify!($host_write),
                "",
                vec![$((<$host_type>::CPP, stringify!($host_field))),+],
            )),+];
            let reads = [$((
                <$answer>::CPP,
                stringify!($read),
                ", BegunRead const*",
                vec![$((<$argument_type>::CPP, stringify!($argument))),*],
            )),+];
            writes.into_iter().chain(host_writes).chain(reads).map(|(answer, entry, read, parameters)| {
                let parameters: String = parameters.iter().map(|(cpp, name)| format!(", {cpp} {name}")).collect();
                format!("{answer} style_engine_{entry}(DocumentHost const*{read}{parameters});")
            })
        }
    };
}

style_boundary! {
    |engine|

    writes {
        set_highlight_observing_elements => SetHighlightObservingElements { kind: u8, nodes: Box<[StyleNodeID]> } =>
            engine.set_highlight_observing_elements(kind, &nodes);
        set_custom_highlight_observing_elements => SetCustomHighlightObservingElements {
            nodes: Box<[StyleNodeID]>, names: Box<[StyleAtomID]>
        } => engine.set_custom_highlight_observing_elements(&nodes, &names);
        set_fold_id_and_class_name_case => SetFoldIdAndClassNameCase { fold: bool };
        set_html_element_namespace => SetHtmlElementNamespace { namespace_atom: StyleAtomID };
        mark_relation_only_style_node => MarkRelationOnlyStyleNode { node: StyleNodeID };
        link_style_nodes_in_dom_order => LinkStyleNodesInDomOrder { links: Box<[u32]> } =>
            engine.link_style_nodes_in_dom_order(&links);
        unlink_style_node_from_dom_order => UnlinkStyleNodeFromDomOrder {
            node: StyleNodeID, parent: Option<StyleNodeID>
        };
        set_top_layer_elements => SetTopLayerElements { members: Box<[StyleNodeID]> } =>
            engine.set_top_layer_elements(&members);
        set_text_is_ascii_whitespace => SetTextIsAsciiWhitespace { node: StyleNodeID, value: bool };
        set_node_dom_paint_facts => SetNodeDomPaintFacts { node: StyleNodeID, facts: u8 };
        set_element_unique_node_id => SetElementUniqueNodeId { node: StyleNodeID, unique_node_id: i64 };
        set_element_table_spans => SetElementTableSpans {
            node: StyleNodeID, column_span: u32, row_span: u32
        } =>
            engine.set_element_table_spans(node, TableSpans {
                column_span: u16::try_from(column_span).expect("a column span is clamped to 1000"),
                row_span: u16::try_from(row_span).expect("a row span is clamped to 65534"),
            });
        set_element_frameset_layout => SetElementFrameSetLayout {
            node: StyleNodeID,
            columns: Box<[FfiFrameSetDimension]>,
            rows: Box<[FfiFrameSetDimension]>,
            borders: Box<[FfiFrameSetBorder]>,
            border_thickness: u32
        } =>
            {
                let (column_borders, row_borders) = borders.split_at(columns.len());
                let (column_borders, row_borders) = (column_borders.into(), row_borders.into());
                engine.set_element_frameset_layout(node, FrameSetLayout { columns, rows, column_borders, row_borders, border_thickness });
            };
        set_element_id_name => SetElementIdName { node: StyleNodeID, name: StyleAtomID };
        record_environment_change => RecordEnvironmentChange { changes: u8 };
        record_custom_property_registration_change => RecordCustomPropertyRegistrationChange => engine.record_custom_property_registration_change();
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
        end_deferred_geometry_transaction_flush => EndDeferredGeometryTransactionFlush;
        record_container_query_input => RecordContainerQueryInput { node: StyleNodeID };
        record_derived_element_style_input => RecordDerivedElementStyleInput {
            node: StyleNodeID, reaction: u8, inherited_style_groups: u8
        };
        record_font_input_changes => RecordFontInputChanges {
            family_name_lengths: Box<[u32]>, family_name_units: Box<[u16]>, font_lists: Box<[u64]>
        } => engine.record_font_input_changes(&family_name_lengths, &family_name_units, &font_lists);
        fill_loaded_font_faces => FillLoadedFontFaces { fills: LoadedFontFaceFills };
        consume_element_style_input => ConsumeElementStyleInput { node: StyleNodeID };
        set_element_replaced_content_input => SetElementReplacedContentInput {
            node: StyleNodeID, kind: u8, present: u8, values: [u32; 4]
        } => engine.set_element_replaced_content_input(node, ReplacedContentInput::from_raw(kind, present, values));
        record_transition_baseline => RecordTransitionBaseline {
            node: StyleNodeID, pseudo_kind: u8, style_record: std::sync::Arc<super::pipeline::Record>
        };
        release_transition_baselines => ReleaseTransitionBaselines;
        begin_style_record_view_epoch => BeginStyleRecordViewEpoch;
        end_style_record_view_epoch => EndStyleRecordViewEpoch;
        set_tree_scope_uses_document_sheets => SetTreeScopeUsesDocumentSheets { tree_scope: TreeScopeID };
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
        evaluate_size_containers_needing_evaluation_after_layout => @published
            EvaluateSizeContainersNeedingEvaluationAfterLayout;
        set_counter_style_environment_identity => SetCounterStyleEnvironmentIdentity {
            tree_scope: TreeScopeID, identity: u64
        };
        set_held_style_record => SetHeldStyleRecord { node: StyleNodeID, record: HeldHostRecord }
            => engine.set_held_style_record(node, record.0);
        set_held_pseudo_style_record => SetHeldPseudoStyleRecord {
            node: StyleNodeID, pseudo_kind: u8, highlight_name: StyleAtomID, style_record: u64, released: ReleasedHostRecord
        } => {
            let target = super::pipeline::PseudoElementTarget {
                kind: pseudo_kind,
                highlight_name,
            };
            engine.set_held_pseudo_style_record(node, target, style_record);
            drop(released);
        };
        set_element_css_defined_animations => SetElementCssDefinedAnimations {
            node: StyleNodeID, slot: u8, names: Box<[CssString]>, definitions: Box<[HeldAppliedAnimationDefinition]>
        } => engine.set_element_css_defined_animations(node, slot, names, definitions);
        set_sheet_conditions_hold => SetSheetConditionsHold { sheet: SheetID, conditions_hold: bool };
    }

    // The writes of the host's projection rows, which the host makes on its own thread as its DOM changes, and the
    // render owner takes at the host's next barrier (see `pipeline::projection::ProjectionWriter`).
    |host|
    host_writes {
        set_shadow_root { shadow_host: StyleNodeID, shadow_root: StyleNodeID } => {
            let attached = host.engine_memo().projection.borrow_mut().writes().set_shadow_root(shadow_host, shadow_root);
            host.queue_change(ArenaChange::Engine(EngineWrite::ShadowRoot { host: shadow_host, attached }));
        };
        retire_text_style_nodes { nodes: Box<[StyleNodeID]> } => {
            host.engine_memo().projection.borrow_mut().retire_texts(nodes.iter().copied());
            host.queue_change(ArenaChange::Engine(EngineWrite::RetireTexts(nodes)));
        };
        set_slot_assigned_nodes { slot: StyleNodeID, assigned: Box<[StyleNodeID]> } => {
            host.engine_memo().projection.borrow_mut().writes().set_slot_assigned_nodes(slot, &assigned);
            // NB: The hover's chain reads the slot's assigned nodes as the host wrote them.
            host.publish_element_writes();
            host.queue_change(ArenaChange::Engine(EngineWrite::SlotAssignedNodes));
        };
        set_tree_scope_root { tree_scope: TreeScopeID, root: StyleNodeID } => {
            // NB: A shadow root is a node selectors reach and nothing publishes features for, so this is where it gets
            //     a row of its own.
            if tree_scope != TreeScopeID::DOCUMENT {
                host.engine_memo().projection.borrow_mut().ensure_facts(root);
            }
            host.queue_change(ArenaChange::Engine(EngineWrite::TreeScopeRoot { tree_scope, root }));
        };
        set_element_part_exposure { node: StyleNodeID, exposure: StyleAtomID } =>
            host.engine_memo().projection.borrow_mut().writes().set_element_part_exposure(node, exposure);
        set_element_directionality { node: StyleNodeID, directionality: StyleAtomID } =>
            host.engine_memo().projection.borrow_mut().writes().set_element_directionality(node, directionality);
        set_element_custom_states { node: StyleNodeID, states: Box<[StyleAtomID]> } =>
            host.engine_memo().projection.borrow_mut().writes().set_element_custom_states(node, &states);
        set_element_heading_level { node: StyleNodeID, level: u8 } =>
            host.engine_memo().projection.borrow_mut().writes().set_element_heading_level(node, level);
        set_element_backing_pseudo_kind { node: StyleNodeID, pseudo_kind_plus_one: u8 } => {
            let memo = host.engine_memo();
            memo.projection.borrow_mut().writes().set_element_backing_pseudo_kind(node, pseudo_kind_plus_one);
            if pseudo_kind_plus_one != 0 {
                memo.note_backing_element(node);
            }
        };
        set_element_adjustment_facts { node: StyleNodeID, facts: u32 } => {
            let moved = host.engine_memo().projection.borrow_mut().writes().set_element_adjustment_facts(node, facts);
            // NB: The adjustments read whether the element is rendered in the top layer, where alone its ::backdrop
            //     exists, so a move of it is a style input of the element like a container query answer. The type
            //     facts are fixed from the element's arrival on, and whether it has animations is no input of its
            //     style.
            if moved & super::bridge::element_adjustment_fact::RENDERED_IN_TOP_LAYER != 0 {
                host.publish_element_writes();
                host.queue_change(ArenaChange::Engine(EngineWrite::TopLayerMove { node }));
            }
        };
        set_element_construction_facts { node: StyleNodeID, facts: u32 } =>
            host.engine_memo().projection.borrow_mut().writes().set_element_construction_facts(node, facts);
        set_element_box_kind { node: StyleNodeID, box_kind: ElementBoxKind } => {
            if host.engine_memo().projection.borrow_mut().writes().set_element_box_kind(node, box_kind) {
                host.publish_element_writes();
                host.queue_change(ArenaChange::Engine(EngineWrite::BoxKindChange { node }));
            }
        };
    }

    reads {
        connected_element_count() -> u32;
        custom_property_environment_is_engine_made(identity: u64) -> bool =>
            engine.custom_property_environments.is_engine_made(identity);
        pending_transaction_may_affect_layout_geometry() -> bool;
        defer_pending_transaction_for_geometry_read() -> ();
        begin_deferred_geometry_transaction_flush() -> bool;
        has_deferred_geometry_transaction() -> bool;
        cold_pass(root: Option<StyleNodeID>, verify: bool, threads: u32) -> u64 => root.map_or(0, |root| engine.run_cold_pass(root, verify, threads));
        cold_pass_report(index: u32) -> u64 => engine.cold_pass_report(index);
        live_report(root: Option<StyleNodeID>, verify: bool, index: u32) -> u64 =>
            root.map_or(0, |root| engine.retained.live_report(root, verify, index));
        style_counter(index: u32) -> u64 => engine.retained.live_counters.get(index);
        reset_style_counters() -> bool => {
            engine.retained.live_counters.reset();
            true
        };
        last_pass_is_full() -> bool => engine.retained.live_counters.last_pass_is_full();
        last_pass_size(root: Option<StyleNodeID>, changed: bool) -> u32 => root.map_or(0, |root| {
            let retained = &mut engine.retained;
            retained.live_counters.last_pass_elements(&retained.projection, root, changed).len() as u32
        });
        last_pass_element(root: Option<StyleNodeID>, changed: bool, index: u32) -> u32 => root.map_or(0, |root| {
            let retained = &mut engine.retained;
            let elements = retained.live_counters.last_pass_elements(&retained.projection, root, changed);
            elements.get(index as usize).map_or(0, |node| node.raw())
        });
        animated_style_samples(rebuilt_every_group: bool, reset: bool) -> u64 =>
            engine.retained.live_counters.animated_samples(rebuilt_every_group, reset);
        layer_index(tree_scope: TreeScopeID, layer: CascadeLayerID) -> u32;
        size_query_container_scan_visits(reset: bool) -> u64;
        ensure_random_base_value(node: Option<StyleNodeID>, name: FfiSpan<u16>, element_shared: bool) -> u64 => {
            // SAFETY: The host lends the name for the call.
            let name = unsafe { borrow(name.data, name.size) };
            engine.ensure_random_base_value(node, name, element_shared).to_bits()
        };
    }
}

impl StyleChange {
    /// Whether the change only keeps the engine from reclaiming records: it notes the record a pseudo-element holds, or
    /// begins or ends an epoch of style record views. It changes nothing the paint properties are prepared from.
    pub(crate) fn only_keeps_records_alive(&self) -> bool {
        matches!(
            self,
            Self::BeginStyleRecordViewEpoch | Self::EndStyleRecordViewEpoch | Self::SetHeldPseudoStyleRecord { .. }
        )
    }

    /// Whether the change may move a fact the host knows of the render state. One that keeps records alive or notes
    /// what a text spells never does: neither stages a style input or touches a layout box.
    pub(crate) fn may_move_facts(&self) -> bool {
        !self.only_keeps_records_alive() && !matches!(self, Self::SetTextIsAsciiWhitespace { .. })
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
