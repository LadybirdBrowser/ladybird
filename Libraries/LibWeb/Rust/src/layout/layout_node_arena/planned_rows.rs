/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The rows a tree build makes for a planned subtree. The planner read each element's style record once and noted what
//! a row needs to know of it, so stamping a planned row reads the facts the plan carries instead of the record.

use super::*;

/// What a row needs to know of the style record it is stamped with, as the planner read it from the record.
mod planned_style_fact {
    pub(crate) const HAS_ANCHOR_NAMES: u32 = 1 << 0;
    pub(crate) const INSETS_USE_ANCHOR_FUNCTIONS: u32 = 1 << 1;
    pub(crate) const HAS_PRESERVE_3D_TRANSFORM_STYLE: u32 = 1 << 2;
    pub(crate) const HAS_AUTO_CONTENT_VISIBILITY: u32 = 1 << 3;
    pub(crate) const HAS_SCROLL_SNAP_AREA: u32 = 1 << 4;
    pub(crate) const MAKES_SCROLL_CONTAINER: u32 = 1 << 6;
    /// The style may make the box establish a containing block for positioned boxes.
    pub(crate) const MAY_ESTABLISH_POSITIONING_CONTAINING_BLOCKS: u32 = 1 << 7;
    pub(crate) const IS_ABSOLUTELY_POSITIONED: u32 = 1 << 8;
    /// The box has size containment or is a size container, which a box's replaced content facts follow.
    pub(crate) const HAS_SIZE_CONTAINMENT_OR_IS_SIZE_CONTAINER: u32 = 1 << 9;
    /// Where the SVG paint resource kinds the row enrolls start, in the bits above the others.
    pub(crate) const SVG_PAINT_RESOURCE_KINDS_SHIFT: u32 = 16;
}

/// The facts of [`planned_style_fact`] `style` says whatever the kind of the box it styles, for
/// [`planned_style_facts_for_kind`] to complete: [`planned_style_fact::MAKES_SCROLL_CONTAINER`] where its overflow
/// values make a scroll container, and the SVG paint resource kinds of its fill and stroke as if it styled an SVG path.
pub(crate) fn planned_record_style_facts(style: ComputedValuesView<'_>) -> u32 {
    use crate::painting::svg_paint_resources::SvgPaintResourceKind;
    use planned_style_fact::*;
    let mut facts: u32 = 0;
    if !style.anchor().anchor_names.as_slice().is_empty() {
        facts |= HAS_ANCHOR_NAMES;
    }
    if super::super::node_facts::style_insets_use_anchor_functions(style) {
        facts |= INSETS_USE_ANCHOR_FUNCTIONS;
    }
    if style.transform().transform_style == crate::css::css_enums::transform_style::PRESERVE_3D {
        facts |= HAS_PRESERVE_3D_TRANSFORM_STYLE;
    }
    if style.content_visibility() == crate::css::css_enums::content_visibility::AUTO {
        facts |= HAS_AUTO_CONTENT_VISIBILITY;
    }
    if style.misc_interaction().scroll_snap_strictness != crate::css::css_enums::scroll_snap_strictness::NONE {
        facts |= HAS_SCROLL_SNAP_AREA;
    }
    if super::super::node_facts::style_makes_scroll_container(style) {
        facts |= MAKES_SCROLL_CONTAINER;
    }
    if crate::painting::style_queries::style_may_establish_positioning_containing_blocks(style) {
        facts |= MAY_ESTABLISH_POSITIONING_CONTAINING_BLOCKS;
    }
    if style.is_absolutely_positioned() {
        facts |= IS_ABSOLUTELY_POSITIONED;
    }
    if style.has_size_containment() || style.is_size_container() {
        facts |= HAS_SIZE_CONTAINMENT_OR_IS_SIZE_CONTAINER;
    }
    let effects = style.effects();
    let mut kinds = 0u8;
    if crate::painting::css_filter::contains_url(&effects.filter) {
        kinds |= SvgPaintResourceKind::Filter.bit();
    }
    if crate::painting::css_filter::contains_url(&effects.backdrop_filter) {
        kinds |= SvgPaintResourceKind::BackdropFilter.bit();
    }
    let svg = style.inherited_svg();
    if svg.fill.kind == crate::css::computed_value_types::SVG_PAINT_URL {
        kinds |= SvgPaintResourceKind::Fill.bit();
    }
    if svg.stroke.kind == crate::css::computed_value_types::SVG_PAINT_URL {
        kinds |= SvgPaintResourceKind::Stroke.bit();
    }
    facts | (u32::from(kinds) << SVG_PAINT_RESOURCE_KINDS_SHIFT)
}

/// The facts of [`planned_style_fact`] a row of `kind` takes from a style of which [`planned_record_style_facts`]
/// answered `record_facts`.
pub(crate) fn planned_style_facts_for_kind(record_facts: u32, kind: NodeKind) -> u32 {
    use crate::painting::svg_paint_resources::SvgPaintResourceKind;
    use planned_style_fact::*;
    let mut facts = record_facts;
    if kind == NodeKind::Viewport {
        facts |= MAKES_SCROLL_CONTAINER;
    }
    if !crate::painting::node_painting::is_svg_path(kind) {
        let path_kinds = SvgPaintResourceKind::Fill.bit() | SvgPaintResourceKind::Stroke.bit();
        facts &= !(u32::from(path_kinds) << SVG_PAINT_RESOURCE_KINDS_SHIFT);
    }
    facts
}

/// How the rows a planned build made take their pre-order labels from their pre-order positions as the arena adopts
/// them (see [`crate::layout::tree_plan::ChunkColumns::pre_order_positions`]): a row at position `n` among the rows
/// numbered below a name whose rows start at `starts[name]` is labeled `lower + step * (starts[name] + n)`.
pub(crate) struct PlannedPreOrderLabels {
    pub(crate) lower: u64,
    pub(crate) step: u64,
    pub(crate) starts: Vec<u64>,
}

impl PlannedPreOrderLabels {
    fn label(&self, packed_position: u64) -> u64 {
        debug_assert_ne!(packed_position, 0, "a planned row has a pre-order position");
        let (name, position) = crate::layout::tree_plan::unpacked_pre_order_position(packed_position);
        self.lower + self.step * (self.starts[name] + position)
    }
}

/// What [`LayoutNodeArena::allocate_preallocated_row`] took from a row a build made, until the builder's steps that
/// stamp it give it back.
pub(crate) struct PreallocatedRow {
    first_child: NodeSlotId,
    last_child: NodeSlotId,
    item_flags: u32,
}

/// The flags a row takes from the facts of [`planned_style_fact`] its style has.
pub(crate) fn planned_style_flags(style_facts: u32) -> u32 {
    use planned_style_fact::*;
    let mut flags = 0;
    if style_facts & HAS_ANCHOR_NAMES != 0 {
        flags |= NodeFlag::HasAnchorNames as u32;
    }
    if style_facts & INSETS_USE_ANCHOR_FUNCTIONS != 0 {
        flags |= NodeFlag::InsetsUseAnchorFunctions as u32;
    }
    if style_facts & HAS_PRESERVE_3D_TRANSFORM_STYLE != 0 {
        flags |= NodeFlag::HasPreserve3dTransformStyle as u32;
    }
    flags
}

/// What of the facts of [`planned_style_fact`] only tells that a document may have some kind of box, which a build
/// notes once for all rows of a plan (see [`LayoutNodeArena::note_planned_style_may_have_facts`]).
pub(crate) fn planned_style_may_have_facts(style_facts: u32) -> u32 {
    use planned_style_fact::*;
    style_facts & (HAS_AUTO_CONTENT_VISIBILITY | HAS_SCROLL_SNAP_AREA)
}

/// Whether a row stamped completely by a plan, `row`, with a record whose facts are `style_facts` (see
/// [`planned_style_facts_for_kind`]), enrolls for the sync of its replaced content facts.
fn planned_row_enrolls_for_replaced_content_facts(row: &NodeData, style_facts: u32) -> bool {
    let kind = row.kind.get();
    (super::super::node_facts::node_may_have_replaced_content_facts(row)
        || (super::super::node_facts::kind_is_box(kind)
            && style_facts & planned_style_fact::HAS_SIZE_CONTAINMENT_OR_IS_SIZE_CONTAINER != 0))
        && kind != NodeKind::SVGSVGBox
}

/// Whether the build has to note what the style of `row`, an element's row stamped completely by a plan with a record
/// whose facts are `style_facts` (see [`planned_style_facts_for_kind`]), tells the rest of the document with
/// [`LayoutNodeArena::note_planned_element_row_style`], or derive what attaching it derives with
/// [`LayoutNodeArena::note_planned_child_attached_with_derived_facts`]: a row that may establish a containing block, is
/// absolutely positioned, enrolls for a sync, holds SVG paint resources, is a scroll container or is the document
/// element's. Any other row of a planned parent takes nothing from either.
pub(crate) fn planned_element_row_notes_its_style(row: &NodeData, style_facts: u32) -> bool {
    use planned_style_fact::*;
    style_facts
        & (MAY_ESTABLISH_POSITIONING_CONTAINING_BLOCKS
            | IS_ABSOLUTELY_POSITIONED
            | MAKES_SCROLL_CONTAINER
            | (u32::from(u8::MAX) << SVG_PAINT_RESOURCE_KINDS_SHIFT))
        != 0
        || row.flags.get()
            & (NodeFlag::IsDocumentElement as u32
                | NodeFlag::EstablishesAbsolutePositionContainingBlock as u32
                | NodeFlag::EstablishesFixedPositionContainingBlock as u32)
            != 0
        || matches!(row.kind.get(), NodeKind::Viewport | NodeKind::SVGForeignObjectBox)
        || planned_row_enrolls_for_replaced_content_facts(row, style_facts)
}

impl LayoutNodeArena {
    /// Whether the element or text node `style_node` is scrolled, which a row stamped for it notes in its
    /// [`NodeFlag::HasScrollOffset`] flag.
    pub(crate) fn style_node_has_scroll_offset(&self, style_node: StyleNodeID) -> bool {
        let offsets = &self.style_node_tables.element_scroll_offsets;
        !offsets.is_empty()
            && offsets
                .get(&style_node)
                .is_some_and(|offset| *offset != CssPixelPoint::default())
    }

    /// Notes what the style of `slot`, a row a plan stamped completely for the principal box of an element with a
    /// record whose facts are `style_facts` (see [`planned_style_facts_for_kind`]), tells the rest of the document, as stamping
    /// it does, but for what [`planned_style_may_have_facts`] says of it: the build binds the row and pins its record
    /// with the other rows of its plan (see [`Self::bind_planned_rows`]). A row for which
    /// [`planned_element_row_notes_its_style`] answers false has nothing to note.
    pub(crate) fn note_planned_element_row_style(&mut self, slot: NodeSlotId, style_facts: u32) {
        use planned_style_fact::*;
        let kind = self.data(slot).kind.get();
        debug_assert_ne!(kind, NodeKind::FrameSetBox, "the builder stamps a frameset's row");
        // As the build enrolls a row it stamps, once it has its style.
        if planned_row_enrolls_for_replaced_content_facts(self.data(slot), style_facts) {
            self.enroll_node_for_replaced_content_facts_sync(slot);
        }
        let svg_paint_resource_kinds = (style_facts >> SVG_PAINT_RESOURCE_KINDS_SHIFT) as u8;
        if svg_paint_resource_kinds != 0 {
            self.svg_paint_resources
                .set_enrolled_kinds(slot, svg_paint_resource_kinds);
        }

        // As the build notes the style of a row it made.
        if self.data(slot).flags.get() & NodeFlag::IsDocumentElement as u32 != 0 {
            let viewport = self.bound_viewport_row();
            if !viewport.is_invalid() {
                self.note_built_scroll_container(viewport);
            }
        } else if style_facts & MAKES_SCROLL_CONTAINER != 0 {
            self.note_built_scroll_container(slot);
        }
    }

    /// Notes what the styles of rows a plan stamped tell the rest of the document where it is no more than that a
    /// document may have some kind of box: `facts` are what [`planned_style_may_have_facts`] said of them.
    pub(crate) fn note_planned_style_may_have_facts(&mut self, facts: u32) {
        use planned_style_fact::*;
        self.may_have_auto_content_visibility |= facts & HAS_AUTO_CONTENT_VISIBILITY != 0;
        self.may_have_scroll_snap_areas |= facts & HAS_SCROLL_SNAP_AREA != 0;
    }

    /// Binds the rows of `rows`, each a row a build stamped completely for the element or text node it is listed with,
    /// in order, as [`Self::bind_planned_row`] binds one.
    pub(crate) fn bind_planned_rows(&mut self, rows: &[(StyleNodeID, NodeSlotId)]) {
        let mut position = 0;
        while position < rows.len() {
            position += self.bind_planned_rows_of_unbound_nodes(&rows[position..]);
            // NB: A node bound to a row before, or carried by one, has its rows taken over one by one.
            if let Some(&(style_node, slot)) = rows.get(position) {
                self.bind_planned_row(slot, style_node);
                position += 1;
            }
        }
    }

    /// Binds the rows of `rows` as [`Self::bind_planned_rows`] does, as long as no row carries their nodes and they
    /// are bound to none, which leaves no row to take over. Answers how many it bound.
    fn bind_planned_rows_of_unbound_nodes(&mut self, rows: &[(StyleNodeID, NodeSlotId)]) -> usize {
        let mut elements = self.bound_rows.elements.writer();
        let mut texts = self.bound_rows.texts.writer();
        // NB: The host hears of the boxes nodes gain where it mirrors them. A node bound to a row loses none.
        let mut box_presence = self.box_presence_host.is_some().then(|| {
            self.queued_box_presence
                .as_mut()
                .expect("boxes change only in a change that queues what the host hears of them")
        });
        for (bound, &(style_node, slot)) in rows.iter().enumerate() {
            let index = slot.slot_index() as usize;
            debug_assert_eq!(self.style_nodes[index], Some(style_node));
            let (column, node_index) = match style_node.text_index() {
                Some(text_index) => (&mut texts, text_index as usize),
                None => (&mut elements, style_node.element_slot()),
            };
            let head = self.first_rows_by_style_node.head_mut(style_node);
            if !head.is_invalid() || column.get(node_index).is_some_and(|row| !row.is_invalid()) {
                return bound;
            }
            *head = slot;
            self.next_rows_with_same_style_node[index] = NodeSlotId::INVALID;
            column.set(node_index, slot);
            if let Some(box_presence) = &mut box_presence {
                box_presence.push(BoundNode::Identity(style_node));
            }
        }
        rows.len()
    }

    /// Owes the host the image resources of rows a build stamped, `owed[range]`, in order, as
    /// [`Self::owe_image_resources`] owes those of one row, sharing the list with the build. The caller notes the rows
    /// that own the provider of their image with [`Self::await_owned_image_provider`].
    pub(crate) fn owe_planned_image_resources(
        &mut self,
        owed: &std::sync::Arc<OwedImageResourcesEntries>,
        range: std::ops::Range<usize>,
    ) {
        self.image_resources_owed_to_host
            .stretches
            .push(OwedImageResourcesStretch::Planned(owed.clone(), range));
    }

    /// Records the layout algorithm the MathML element a planned row is built for selects.
    pub(crate) fn note_planned_math_layout_kind(
        &mut self,
        slot: NodeSlotId,
        kind: super::super::math_formatting_context::MathLayoutKind,
    ) {
        self.math_layout_kinds.insert(slot, kind);
    }

    /// Notes that `child`, a row of a planned subtree, is a child of `parent`, which the build linked it under, as
    /// [`Self::insert_child`] notes an attached child, except that its own children count as attached after it, and
    /// that pre-order labels and paint order damage are left to [`Self::finish_planned_subtree`]. A child stamped with
    /// `style_facts` (see [`planned_style_facts_for_kind`]) is not asked what the facts already say.
    pub(crate) fn note_planned_child_attached(
        &mut self,
        parent: NodeSlotId,
        child: NodeSlotId,
        style_facts: Option<u32>,
        parent_is_planned: bool,
    ) {
        use planned_style_fact::*;
        debug_assert_eq!(self.data(child).parent.get(), parent);
        let (may_establish_positioning_containing_blocks, may_be_absolutely_positioned) =
            style_facts.map_or((true, true), |facts| {
                (
                    facts & MAY_ESTABLISH_POSITIONING_CONTAINING_BLOCKS != 0,
                    facts & IS_ABSOLUTELY_POSITIONED != 0,
                )
            });
        // NB: A parent the build planned is as new as a slot the arena grew by: no layout pass has seen it.
        self.note_child_attached_with(
            parent,
            child,
            may_establish_positioning_containing_blocks,
            may_be_absolutely_positioned,
            false,
            !parent_is_planned,
        );
    }

    /// How many chunks of rows the arena has.
    pub(crate) fn chunk_count(&self) -> usize {
        self.chunks.len()
    }

    /// Takes over the rows a tree build made in free slots it took from the arena (see [`Self::take_free_slots`]),
    /// each with the slot it names.
    pub(crate) fn adopt_reused_rows(
        &mut self,
        reused: Vec<crate::layout::tree_plan::ReusedRows>,
        labels: Option<&PlannedPreOrderLabels>,
    ) {
        // OPTIMIZATION: The free slots are older than a fork of the arena that is alive, so each column the rows are
        //               written to saves what they held: the logs take their room once.
        let rows = reused.iter().map(|group| group.slots.len()).sum();
        if rows != 0 {
            self.slot_metadata.reserve_log(rows);
            self.style_records.reserve_log(rows);
            self.image_resources_owed_records.reserve_log(rows);
            self.style_record_pins.reserve_log(rows);
            self.style_nodes.reserve_log(rows);
            self.next_rows_with_same_style_node.reserve_log(rows);
            self.pre_order_labels.reserve_log(rows);
        }
        for group in reused {
            for (offset, &slot) in group.slots.iter().enumerate() {
                debug_assert_eq!(group.rows.len(), group.slots.len());
                let index = slot.slot_index();
                let metadata = self.metadata_mut(index);
                assert!(!metadata.occupied(), "a tree build filled a slot that is live");
                metadata.generation = slot.generation();
                metadata.set_occupied(true);
                *self.data_mut(index) = group.rows[offset].clone();
                // NB: A chunk the arena had is not marked for publication as a new one is.
                self.write_shape(slot).mark_identity();
                self.adopt_row_columns(slot, &group.columns[offset]);
                if let Some(labels) = labels {
                    self.set_node_pre_order_label(slot, labels.label(group.columns[offset].pre_order_position));
                }
                self.note_tree_build_allocation(slot);
            }
            self.live_count = self
                .live_count
                .checked_add(group.slots.len() as u32)
                .expect("layout node arena live count overflowed");
        }
    }

    /// Takes over the chunks a tree build filled, in the order of their indices, which follow the arena's chunks without
    /// a gap, each as it is: the build wrote the rows where the arena keeps them, and what the arena keeps beside them
    /// a column at a time. Every row a chunk holds is live in its first generation; the slots no row was put in, and
    /// those left over at the end of the arena's last chunk, go to the free list.
    pub(crate) fn adopt_filled_chunks(
        &mut self,
        filled: Vec<crate::layout::tree_plan::FilledChunk>,
        labels: Option<&PlannedPreOrderLabels>,
    ) {
        use crate::layout::tree_plan::FRESH_ROW_GENERATION;
        let Some(first_chunk) = filled.first().map(|chunk| chunk.index as usize) else {
            return;
        };
        assert_eq!(
            first_chunk,
            self.chunks.len(),
            "a tree build fills the chunks after the arena's"
        );
        let end_chunk = first_chunk + filled.len();
        let first_slot = first_chunk * SLOTS_PER_CHUNK;
        let end = end_chunk * SLOTS_PER_CHUNK;
        assert!(
            end <= MAX_NODE_SLOT_COUNT as usize,
            "layout node arena exhausted its slot index space"
        );
        let previous_end = self.slot_metadata.len();
        for index in (previous_end..first_slot).rev() {
            self.free_list.push(free_slot_entry(index as u32, 0));
        }
        self.slot_metadata.reserve_for_len(end);
        self.style_records.reserve_for_len(end);
        self.style_record_pins.reserve_for_len(end);
        self.style_nodes.reserve_for_len(end);
        self.pre_order_labels.reserve_for_len(end);
        self.slot_metadata.resize_with_default(first_slot);
        self.style_records.resize_with_default(first_slot);
        self.style_record_pins.resize_with_default(first_slot);
        self.style_nodes.resize_with_default(first_slot);
        self.pre_order_labels.resize_with_default(first_slot);
        let mut pre_order_labels = [0; SLOTS_PER_CHUNK];
        let mut slot_metadata = [SlotMetadata::default(); SLOTS_PER_CHUNK];
        let mut style_record_pins = [ArenaStylePin::None; SLOTS_PER_CHUNK];
        for (expected_index, chunk) in (first_chunk..end_chunk).zip(filled) {
            assert_eq!(
                chunk.index as usize, expected_index,
                "the filled chunks follow each other"
            );
            let chunk_slot = expected_index * SLOTS_PER_CHUNK;
            self.chunks.push(chunk.rows);
            self.shape_writes.add_chunk(expected_index);
            for (offset, metadata) in slot_metadata.iter_mut().enumerate() {
                *metadata = SlotMetadata {
                    generation: if offset < chunk.used { FRESH_ROW_GENERATION } else { 0 },
                    state: if offset < chunk.used { slot_state::OCCUPIED } else { 0 },
                };
            }
            self.slot_metadata.extend_from_slice(&slot_metadata);
            let records = &chunk.columns.style_records;
            let mut uninstalled_records = 0;
            for (pin, &record) in style_record_pins.iter_mut().zip(records) {
                *pin = if record == 0 {
                    ArenaStylePin::None
                } else {
                    uninstalled_records += u32::from(crate::css::style::names_an_uninstalled_record(record));
                    ArenaStylePin::Published
                };
            }
            self.rows_naming_uninstalled_records += uninstalled_records;
            self.style_records.extend_from_slice(records);
            self.style_record_pins.extend_from_slice(&style_record_pins);
            self.style_nodes.extend_from_slice(&chunk.columns.style_nodes);
            if let Some(labels) = labels {
                for (label, &position) in pre_order_labels[..chunk.used]
                    .iter_mut()
                    .zip(&chunk.columns.pre_order_positions)
                {
                    *label = labels.label(position);
                }
                pre_order_labels[chunk.used..].fill(0);
            }
            self.pre_order_labels.extend_from_slice(&pre_order_labels);
            self.unique_node_ids_mut().publish_new_chunk(
                chunk_slot,
                FRESH_ROW_GENERATION,
                &chunk.columns.unique_node_ids,
            );
            self.live_count = self
                .live_count
                .checked_add(chunk.used as u32)
                .expect("layout node arena live count overflowed");
            for index in chunk_slot..chunk_slot + chunk.used {
                self.note_tree_build_allocation(NodeSlotId::new(index as u32, FRESH_ROW_GENERATION));
            }
            for index in (chunk_slot + chunk.used..chunk_slot + SLOTS_PER_CHUNK).rev() {
                self.free_list.push(free_slot_entry(index as u32, 0));
            }
        }
        self.image_resources_owed_records.resize_with_default(end);
        self.style_records_pinned_by_host.resize_with_default(end);
        self.next_rows_with_same_style_node.resize_with_default(end);
        self.next_rows_built_for_same_node.resize_with_default(end);
        self.shape_writes.note_identity();
        self.shape_writes.note_style();
    }

    /// Gives `rows`, the rows of generated text a planned build made and adopted, what stamping such a row gives it: each
    /// joins the rows carrying the generator it answers by, and is synced before layout. Each row of `texts` spells its
    /// text.
    pub(crate) fn commit_planned_generated_text_rows(&mut self, rows: &[NodeSlotId], texts: &[(NodeSlotId, Vec<u16>)]) {
        for &row in rows {
            let index = row.slot_index() as usize;
            let generator = self.style_nodes[index].expect("a generated text row answers by its generator");
            let head = self.first_rows_by_style_node.head_mut(generator);
            self.next_rows_with_same_style_node[index] = std::mem::replace(head, row);
        }
        for (row, text) in texts {
            self.set_generated_text(*row, ak::Utf16String::from_utf16(text));
        }
        self.enroll_built_text_rows_for_content_sync(rows.iter().copied());
    }

    /// Links `previous` and `next`, rows a planned build made, as siblings, `next` after `previous`.
    pub(crate) fn link_planned_siblings(&mut self, previous: NodeSlotId, next: NodeSlotId) {
        self.write_shape(previous).set_next_sibling(next);
        self.write_shape(next).set_previous_sibling(previous);
    }

    /// Gives `slot`, a row a build stamped completely, what the build wrote for it beside the row, but for the chain of
    /// rows carrying its node, which [`Self::bind_planned_row`] links it into.
    fn adopt_row_columns(&mut self, slot: NodeSlotId, columns: &crate::layout::tree_plan::RowColumns) {
        let index = slot.slot_index() as usize;
        if columns.style_record != 0 {
            self.replace_style_record(index, columns.style_record);
            self.style_record_pins[index] = ArenaStylePin::Published;
        }
        self.style_nodes[index] = columns.style_node;
        if columns.unique_node_id != 0 {
            self.unique_node_ids_mut().publish(slot, columns.unique_node_id);
        }
    }

    /// Binds `slot`, a row a build stamped completely for the element or text node `style_node`, to that node, as
    /// stamping a row binds it: it joins the rows carrying the node, and the row the node was bound to before, if any,
    /// joins the ring of rows built for the node and keeps its style readable while the host holds it.
    pub(crate) fn bind_planned_row(&mut self, slot: NodeSlotId, style_node: StyleNodeID) {
        let index = slot.slot_index() as usize;
        debug_assert_eq!(self.style_nodes[index], Some(style_node));
        let head = self.first_rows_by_style_node.head_mut(style_node);
        self.next_rows_with_same_style_node[index] = std::mem::replace(head, slot);
        self.take_over_rows_of_bound_node(slot);
    }

    /// Links `first_child` to `last_child`, rows a planned build made and linked under `parent`, as `parent`'s
    /// children; `parent` had none.
    pub(crate) fn link_planned_children(
        &mut self,
        parent: NodeSlotId,
        first_child: NodeSlotId,
        last_child: NodeSlotId,
    ) {
        assert!(self.data(parent).first_child.get().is_invalid());
        let mut data = self.write_shape(parent);
        data.set_first_child(first_child);
        data.set_last_child(last_child);
    }

    /// Hands `slot`, a row a planned build made and linked, to the next allocation, for the builder's own steps to
    /// stamp it as they stamp a row they allocate: the row keeps its place among its siblings, loses what the build
    /// wrote besides, and holds no children until [`Self::give_back_planned_row`] gives back the ones answered here, as
    /// a row the builder makes has none yet.
    pub(crate) fn allocate_preallocated_row(&mut self, slot: NodeSlotId) -> PreallocatedRow {
        assert!(self.preallocated_row.is_none());
        let taken = {
            let mut data = self.write_shape(slot);
            let taken = PreallocatedRow {
                first_child: data.first_child.get(),
                last_child: data.last_child.get(),
                item_flags: data.flags.get() & (NodeFlag::IsFlexItem as u32 | NodeFlag::IsGridItem as u32),
            };
            data.set_first_child(NodeSlotId::INVALID);
            data.set_last_child(NodeSlotId::INVALID);
            data.set_kind(NodeKind::Unset);
            data.set_flags(0);
            data.set_generated_for(0);
            taken
        };
        self.preallocated_row = Some(slot);
        taken
    }

    /// Gives a row the builder's steps stamped the children [`Self::allocate_preallocated_row`] took from it, and the
    /// flex or grid item flag the build derived for it, for `keep_item_flags`.
    pub(crate) fn give_back_planned_row(&mut self, slot: NodeSlotId, taken: PreallocatedRow, keep_item_flags: bool) {
        assert!(
            self.preallocated_row.is_none(),
            "the builder's steps took the preallocated row"
        );
        if keep_item_flags && taken.item_flags != 0 {
            let mut data = self.write_shape(slot);
            let flags = data.flags.get();
            data.set_flags(flags | taken.item_flags);
        }
        if taken.first_child.is_invalid() {
            return;
        }
        self.link_planned_children(slot, taken.first_child, taken.last_child);
    }

    /// Notes that `child`, a row of a planned subtree whose build derived its ancestor facts and flex or grid item
    /// flags, is a child of `parent`, which the build linked it under, as [`Self::note_planned_child_attached`] notes
    /// it, but for what the build derived and for the boxes it escapes if it is absolutely positioned: answers whether
    /// it may be, for the caller to mark those with [`Self::mark_nodes_escaped_by_planned_out_of_flow_box`] once every
    /// row above it is attached.
    pub(crate) fn note_planned_child_attached_with_derived_facts(
        &mut self,
        parent: NodeSlotId,
        child: NodeSlotId,
        style_facts: Option<u32>,
        parent_is_planned: bool,
    ) -> bool {
        use planned_style_fact::*;
        debug_assert_eq!(self.data(child).parent.get(), parent);
        if !parent_is_planned {
            self.note_child_attached_to_built_parent(parent);
        }
        let data = self.data(child);
        let kind = data.kind.get();
        // NB: A text neither establishes a containing block nor is positioned.
        if super::super::node_facts::kind_is_text(kind) {
            return false;
        }
        let (may_establish_positioning_containing_blocks, may_be_absolutely_positioned) =
            style_facts.map_or((true, true), |facts| {
                (
                    facts & MAY_ESTABLISH_POSITIONING_CONTAINING_BLOCKS != 0,
                    facts & IS_ABSOLUTELY_POSITIONED != 0,
                )
            });
        if may_establish_positioning_containing_blocks
            || matches!(kind, NodeKind::Viewport | NodeKind::SVGForeignObjectBox)
            || data.flags.get()
                & (NodeFlag::EstablishesAbsolutePositionContainingBlock as u32
                    | NodeFlag::EstablishesFixedPositionContainingBlock as u32)
                != 0
        {
            self.derive_containing_block_establishment_flags(child);
        }

        // A box that becomes a flex or grid item no longer lets the commits of the boxes whose committed fragments hold
        // it be skipped, as setting the flag notes. Only the boxes above the build's root hold any.
        if !parent_is_planned
            && self.data(child).flags.get() & (NodeFlag::IsFlexItem as u32 | NodeFlag::IsGridItem as u32) != 0
        {
            self.paintable_rows.invalidate_committed_geometry(child.slot_index());
            self.invalidate_committed_geometry_of_ancestors(child);
        }
        may_be_absolutely_positioned
    }

    /// Marks the boxes `node`, a row of a planned subtree, escapes if it is absolutely positioned, as attaching it
    /// does. Its children are attached after it, and each marks what it escapes itself.
    pub(crate) fn mark_nodes_escaped_by_planned_out_of_flow_box(&mut self, node: NodeSlotId) {
        self.mark_nodes_escaped_by_attached_out_of_flow_box(node);
    }

    /// Whether `row` is an anonymous box that the child list it is in made around entries of the list, and which a build
    /// of that list makes again: a wrapper around a run of inline boxes, a box a button or fieldset lays its contents
    /// out in, or a box table fixup made. The anonymous boxes that belong to one entry are not: a table's wrapper, the
    /// inline box a text renders in below a `display: contents` element, the box of a writing suggestion, and every
    /// box of a pseudo-element.
    pub(crate) fn is_list_closure_wrapper(&self, row: NodeSlotId) -> bool {
        let data = self.data(row);
        let kind = data.kind.get();
        if !crate::layout::node_facts::has_flag(data, NodeFlag::Anonymous)
            || data.generated_for.get() != 0
            || kind == NodeKind::InlineNode
            || crate::layout::node_facts::kind_is_text(kind)
        {
            return false;
        }
        if kind == NodeKind::TableWrapper {
            let table = data.first_child.get();
            return !table.is_invalid() && self.is_list_made_table(table);
        }
        true
    }

    /// Whether `table`, the table box in a table wrapper, is one table fixup made around parts of a table in a list.
    fn is_list_made_table(&self, table: NodeSlotId) -> bool {
        let data = self.data(table);
        crate::layout::node_facts::has_flag(data, NodeFlag::Anonymous) && data.generated_for.get() == 0
    }

    /// The top row of the entry of a child list the box `row` is the box of: the wrapper table fixup made around it
    /// for a table, the anonymous inline box a text renders in below a `display: contents` element, or the row.
    pub(crate) fn list_entry_top_row(&self, row: NodeSlotId) -> NodeSlotId {
        let parent = self.data(row).parent.get();
        if parent.is_invalid() {
            return row;
        }
        let parent_data = self.data(parent);
        let is_entry_wrapper = match parent_data.kind.get() {
            NodeKind::TableWrapper => parent_data.first_child.get() == row && !self.is_list_made_table(row),
            NodeKind::InlineNode => {
                crate::layout::node_facts::has_flag(parent_data, NodeFlag::Anonymous)
                    && self.is_display_contents_text_wrapper(parent)
            }
            _ => false,
        };
        if is_entry_wrapper {
            return parent;
        }
        row
    }

    /// Whether `row` is in the child list of `root`: its parent is `root`, or an anonymous box the list made in it.
    pub(crate) fn is_in_child_list_of(&self, row: NodeSlotId, root: NodeSlotId) -> bool {
        let mut ancestor = self.data(row).parent.get();
        while ancestor != root {
            if ancestor.is_invalid() || !self.is_list_closure_wrapper(ancestor) {
                return false;
            }
            ancestor = self.data(ancestor).parent.get();
        }
        true
    }

    /// Whether `row` is the anonymous inline box a text renders in below a `display: contents` element.
    pub(crate) fn is_display_contents_text_wrapper(&self, row: NodeSlotId) -> bool {
        self.display_contents_text_wrappers.contains_key(&row)
    }

    /// Links `row` as a child list planned again places it, writing only the links that change.
    pub(crate) fn link_planned_row(&mut self, row: NodeSlotId, links: PlannedLinks) {
        let data = self.data(row);
        if data.parent.get() == links.parent
            && data.previous_sibling.get() == links.previous_sibling
            && data.next_sibling.get() == links.next_sibling
            && links
                .children
                .is_none_or(|(first, last)| data.first_child.get() == first && data.last_child.get() == last)
        {
            return;
        }
        let mut shape = self.write_shape(row);
        shape.set_parent(links.parent);
        shape.set_previous_sibling(links.previous_sibling);
        shape.set_next_sibling(links.next_sibling);
        if let Some((first, last)) = links.children {
            shape.set_first_child(first);
            shape.set_last_child(last);
        }
    }

    /// Notes that `child`, a box a child list planned again kept, is attached to `parent` now, under which it was not
    /// before, as attaching it notes it: what it derives from its new ancestors, and what its subtree damages in paint
    /// order.
    pub(crate) fn note_kept_child_moved(&mut self, parent: NodeSlotId, child: NodeSlotId) {
        self.note_layout_subtree_attached(child);
        self.note_child_attached(parent, child);
    }

    /// Notes that the child list of `parent`, which keeps its box, was built again around some of the boxes it had:
    /// the box and its ancestors lay out again, as removing a child from it would have them do.
    pub(crate) fn note_child_list_restructured(&mut self, parent: NodeSlotId) {
        self.invalidate_self_and_ancestors(
            parent,
            AncestorInvalidation::StructuralChange,
            AncestorIntrinsicSizes::Reset,
        );
    }

    /// Labels the rows `rows`, the rows between two rows that keep their labels in pre-order, each with a label between
    /// `lower` and `upper`, nearer to `upper` unless `upper` ends the labels of the subtree they are in. Answers whether
    /// they fit there.
    pub(crate) fn label_rows_between(
        &mut self,
        rows: &[NodeSlotId],
        lower: u64,
        upper: u64,
        toward_upper: bool,
    ) -> bool {
        if lower >= upper {
            return false;
        }
        let count = rows.len() as u64;
        if count == 0 {
            return true;
        }
        let stride = ((upper - lower) / (count + 1)).min(MAXIMUM_PRE_ORDER_LABEL_STRIDE);
        if stride < 2 {
            return false;
        }
        for (index, &row) in rows.iter().enumerate() {
            let position = index as u64 + 1;
            let label = if toward_upper {
                upper - stride * (count + 1 - position)
            } else {
                lower + stride * position
            };
            self.set_node_pre_order_label(row, label);
        }
        true
    }

    /// Labels every row of the subtree of `root` again, or of the nearest ancestor whose labels leave room enough.
    pub(crate) fn spread_pre_order_labels_below(&mut self, root: NodeSlotId) {
        let mut ancestor = root;
        loop {
            let parent = self.data(ancestor).parent.get();
            if parent.is_invalid() {
                self.set_node_pre_order_label(ancestor, 0);
                let spread_succeeded = self.spread_pre_order_labels_evenly_over_descendants(ancestor, 0, u64::MAX);
                assert!(spread_succeeded, "pre-order label space exhausted");
                return;
            }
            let lower = self.node_pre_order_label(ancestor);
            let upper = self.pre_order_label_of_subtree_successor(ancestor);
            if self.spread_pre_order_labels_evenly_over_descendants(ancestor, lower, upper) {
                return;
            }
            ancestor = parent;
        }
    }

    /// The label of the last row of the subtree of `node` in pre-order.
    pub(crate) fn last_pre_order_label_of_subtree(&self, node: NodeSlotId) -> u64 {
        let mut current = node;
        loop {
            let last_child = self.data(current).last_child.get();
            if last_child.is_invalid() {
                return self.node_pre_order_label(current);
            }
            current = last_child;
        }
    }
}

/// Where a child list planned again places a row: its parent and siblings, and for a box of the list's own, its children.
#[derive(Clone, Copy)]
pub(crate) struct PlannedLinks {
    pub(crate) parent: NodeSlotId,
    pub(crate) previous_sibling: NodeSlotId,
    pub(crate) next_sibling: NodeSlotId,
    pub(crate) children: Option<(NodeSlotId, NodeSlotId)>,
}
