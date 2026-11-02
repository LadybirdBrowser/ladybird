/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The style records the render side installs in the boxes of their elements itself, as the host's install of each
//! row would, ahead of the host that installs the records on the elements.

use super::{ArenaStylePin, LayoutNodeArena, LayoutStyleRef, NodeKind, NodeSlotId};
use crate::css::style::bridge::{
    FfiStyleDelta, FfiStyleInvalidationField, LayoutTreeRebuildRoot, element_adjustment_fact,
};
use crate::css::style::bridge::{REBUILD_LEVEL, RELAYOUT_LEVEL, REPAINT_LEVEL};
use crate::css::style::hover_lane::hover_row_generated_for;
use crate::css::style::tree::StyleNodeID;
use crate::layout::node_data::NodeFlag;
use crate::layout::tree_mutation::HostCalls;
use crate::layout::tree_update_marks::{FfiLayoutTreeUpdateMark, layout_tree_update_reuse_reason};
use smallvec::SmallVec;

/// A box the render side styled with a style record, which the host adopts without publishing it again.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct StyledBox {
    pub(crate) slot: NodeSlotId,
    pub(crate) style_record: u64,
}

/// An element whose rows the render side installed in all the boxes they style, with the record it installed for the
/// element itself: the host marks no relayout and no repaint for its install.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct StyledElement {
    pub(crate) node: StyleNodeID,
    pub(crate) style_record: u64,
}

/// What the render side installed of a style update's style rows: the elements, by element, and the boxes, by
/// box, with the records it pinned for the host, those the boxes moved from and those they moved to, which the host
/// reads until it ends the transaction, and the elements whose layout tree rebuild it marked, by element.
#[derive(Default)]
pub(crate) struct StyledRows {
    pub(crate) elements: Vec<StyledElement>,
    pub(crate) boxes: Vec<StyledBox>,
    pub(crate) pinned_records: Vec<u64>,
    pub(crate) rebuilt: Vec<StyleNodeID>,
}

/// How the boxes of an element whose style record a transaction installed take it.
#[derive(Clone, Copy)]
pub(crate) enum ElementInstall {
    /// The render side installs the records of the element and its pseudo-elements in their boxes, where it can install
    /// all of them.
    Boxes,
    /// The layout tree is built again from the root the record move names, which the render side marks where it can.
    Rebuild(LayoutTreeRebuildRoot),
    /// Only the host installs the element's records: its boxes take the composition of its animations, or appear or go.
    Host,
}

/// What installing one row asks of the render side.
enum RowInstall {
    /// The row moves no box: its element keeps its record.
    Nothing,
    /// The row moves the record of the box `slot`.
    Box(NodeSlotId),
}

impl LayoutNodeArena {
    /// The box `row`, a style row, moves, where the render side installs it as the host would, or none where only
    /// the host installs it. The render side installs a record in a plain box that shows the record the row moves from:
    /// replaced content, form controls, list items, tables and SVG take facts from their element as the host styles
    /// them, and a box that shows a style of the arena's, or an animation sample, is styled in a way of its own.
    fn style_row_install(&self, row: &FfiStyleDelta, node: StyleNodeID, generated_for: u8) -> Option<RowInstall> {
        if row.composed_by_the_host {
            return None;
        }
        if row.old_style_record == row.new_style_record && generated_for == 0 {
            return Some(RowInstall::Nothing);
        }
        if row.old_style_record == 0 || row.new_style_record == 0 {
            return None;
        }
        if row.record_damage & FfiStyleInvalidationField::LevelMask as u32 >= REBUILD_LEVEL {
            return None;
        }
        let slot = match generated_for {
            0 => self.bound_row(node),
            generated_for => self.bound_pseudo_element_row(node, generated_for),
        };
        if slot.is_invalid() {
            return None;
        }
        let parent = self.data(slot).parent.get();
        let plain = matches!(
            self.data(slot).kind.get(),
            NodeKind::Box | NodeKind::BlockContainer | NodeKind::InlineNode
        ) && (parent.is_invalid() || self.data(parent).kind.get() != NodeKind::TableWrapper);
        let shows_old_record = self.style_records[slot.slot_index() as usize] == row.old_style_record
            && !matches!(
                self.style_record_pins[slot.slot_index() as usize],
                ArenaStylePin::Derived | ArenaStylePin::Sampled
            );
        (plain && shows_old_record).then_some(RowInstall::Box(slot))
    }

    /// Installs the style records `rows`, a style update's, in the boxes of their elements and pseudo-elements,
    /// as the host's install of each row does: the record, the styles the box's anonymous descendants inherit, the
    /// relayout, the visual contexts and the repaint the move asks for, and the styles viewport propagation gives the
    /// viewport. It installs an element's rows where it installs all of them, marks the layout tree rebuild of an element
    /// whose boxes are built again where it marks it as the host would, and leaves the host the other elements, and
    /// those `element_install` leaves to it. Answers what it installed.
    pub(crate) fn install_styles(
        &mut self,
        host_calls: HostCalls<'_>,
        rows: &[FfiStyleDelta],
        element_install: impl Fn(StyleNodeID) -> ElementInstall,
    ) -> StyledRows {
        let mut styled = StyledRows::default();
        let mut propagates_to_viewport = false;
        // An element's row is followed by the rows of its pseudo-elements.
        let mut rest = rows;
        while let Some((first, after)) = rest.split_first() {
            let group_length = 1 + after
                .iter()
                .take_while(|row| row.style_node == first.style_node && row.pseudo_kind != u8::MAX)
                .count();
            let (group, next) = rest.split_at(group_length);
            rest = next;
            let Some(node) = StyleNodeID::from_raw(first.style_node) else {
                continue;
            };
            if first.pseudo_kind != u8::MAX {
                continue;
            }
            match element_install(node) {
                ElementInstall::Boxes => {}
                ElementInstall::Rebuild(root) => {
                    // NB: As the host's install of a row that rebuilds, the boxes built again take the records, and a
                    //     box kept in place takes them as the build reaches it.
                    if self.mark_layout_tree_rebuild(node, root) {
                        styled.rebuilt.push(node);
                        styled.elements.push(StyledElement {
                            node,
                            style_record: first.new_style_record,
                        });
                    }
                    continue;
                }
                ElementInstall::Host => continue,
            }
            let installs: Option<Vec<_>> = group
                .iter()
                .map(|row| {
                    let generated_for = hover_row_generated_for(row)?;
                    Some((row, generated_for, self.style_row_install(row, node, generated_for)?))
                })
                .collect();
            let Some(installs) = installs else {
                continue;
            };
            let repaint = group_repaint(group);
            for (row, generated_for, install) in installs {
                let RowInstall::Box(slot) = install else {
                    continue;
                };
                self.install_style_row(host_calls, row, node, slot, repaint);
                self.with_style_engine(|engine| {
                    engine.pin_layout_style_record(row.old_style_record);
                    engine.pin_layout_style_record(row.new_style_record);
                });
                styled
                    .pinned_records
                    .extend([row.old_style_record, row.new_style_record]);
                propagates_to_viewport |= generated_for == 0
                    && self.data(slot).flags.get() & (NodeFlag::IsDocumentElement as u32 | NodeFlag::IsBody as u32)
                        != 0;
                styled.boxes.push(StyledBox {
                    slot,
                    style_record: row.new_style_record,
                });
            }
            styled.elements.push(StyledElement {
                node,
                style_record: first.new_style_record,
            });
        }
        if propagates_to_viewport {
            let viewport = self.bound_viewport_row();
            if !viewport.is_invalid() {
                crate::layout::viewport_propagation::propagate_root_styles_to_viewport(
                    host_calls,
                    self,
                    viewport,
                    &crate::layout::viewport_propagation::viewport_propagation_facts(self),
                );
            }
        }
        styled.elements.sort_unstable_by_key(|element| element.node);
        styled.rebuilt.sort_unstable();
        styled.boxes.sort_unstable_by_key(|styled| styled.slot.slot_index());
        styled
    }

    /// Marks the layout tree rebuild from `root` a record move of `node` asks for, as the host marks it for a style
    /// change, where the element has a box that is not placed in the top layer, nor the viewport's, and none of the
    /// elements it marks is an SVG element or a child of one, which lends its resources to the elements that reference
    /// it. Answers whether it marked it; the host marks the others.
    fn mark_layout_tree_rebuild(&mut self, node: StyleNodeID, root: LayoutTreeRebuildRoot) -> bool {
        let row = self.bound_row(node);
        if row.is_invalid()
            || self.element_adjustment_facts(Some(node)) & element_adjustment_fact::RENDERED_IN_TOP_LAYER != 0
            || self.data(row).flags.get() & (NodeFlag::IsDocumentElement as u32 | NodeFlag::IsBody as u32) != 0
        {
            return false;
        }
        // The box of an element is placed in the top layer, or is the viewport's child, where the anonymous boxes
        // around it are.
        let mut placed = row;
        loop {
            let parent = self.data(placed).parent.get();
            if parent.is_invalid() || self.data(parent).kind.get() == NodeKind::Viewport {
                return false;
            }
            if self.data(parent).flags.get() & NodeFlag::Anonymous as u32 == 0 {
                break;
            }
            placed = parent;
        }
        let parent_or_shadow_host =
            |element: StyleNodeID| self.with_style_store(|engine| engine.parent_element_or_host(element));
        let (target, reuse_reason) = match root {
            LayoutTreeRebuildRoot::PSEUDO_ELEMENTS => (node, layout_tree_update_reuse_reason::PSEUDO_ELEMENT_CHANGE),
            LayoutTreeRebuildRoot::SELF | LayoutTreeRebuildRoot::SELF_UNLESS_DOCUMENT_ELEMENT_OR_BODY => (node, 0),
            LayoutTreeRebuildRoot::PARENT => (parent_or_shadow_host(node).unwrap_or(node), 0),
            _ => return false,
        };
        // A display: contents element has its parent built again with it.
        let mut marked: SmallVec<[StyleNodeID; 2]> = SmallVec::new();
        let mut current = Some(target);
        while let Some(element) = current {
            marked.push(element);
            let displays_contents = self.with_style_store(|engine| {
                engine
                    .element_published_box_facts(element)
                    .is_some_and(|facts| facts.display.is_contents())
            });
            current = if displays_contents {
                parent_or_shadow_host(element)
            } else {
                None
            };
        }
        let is_svg = |element: StyleNodeID| {
            self.element_adjustment_facts(Some(element)) & element_adjustment_fact::IS_SVG_ELEMENT != 0
        };
        if marked.iter().any(|&element| {
            is_svg(element)
                || self
                    .with_style_store(|engine| engine.dom_parent(element))
                    .is_some_and(is_svg)
        }) {
            return false;
        }
        for element in marked {
            self.mark_layout_tree_update(Some(element), FfiLayoutTreeUpdateMark::style_change(reuse_reason));
        }
        true
    }

    /// Installs the record `row` moves to in `slot`, the box of `node` or of its pseudo-element it styles, and repaints
    /// the box as `repaint` says.
    fn install_style_row(
        &mut self,
        host_calls: HostCalls<'_>,
        row: &FfiStyleDelta,
        node: StyleNodeID,
        slot: NodeSlotId,
        repaint: Repaint,
    ) {
        let previous_payloads = self.data(slot).style.get();
        let payloads = self.with_style_engine(|engine| {
            engine
                .style_record_payloads(row.new_style_record)
                .map(LayoutStyleRef::of_groups)
        });
        self.install_style_record(slot, row.new_style_record, payloads, Some(previous_payloads));
        self.set_node_flag(slot, NodeFlag::HasAnimatedOpacityOrTransform, false);
        self.reinherit_anonymous_descendants(HostCalls(host_calls.0), slot);
        if row.record_damage & FfiStyleInvalidationField::LevelMask as u32 >= RELAYOUT_LEVEL {
            self.mark_row_for_relayout_after_style_change(node, slot);
        }
        self.note_style_visual_context_moves(slot, row.record_damage);
        if row.record_damage & FfiStyleInvalidationField::RepaintTextDecorations as u32 != 0
            && crate::painting::paint_read::GeometryRead::paintable_row_is_populated(self, slot)
        {
            self.push_propagated_text_decoration_damage(slot);
        }
        match repaint {
            Repaint::None => {}
            Repaint::Draw => crate::layout::tree_update_marks::repaint(self, slot, false),
            Repaint::DrawAndHitTest => crate::layout::tree_update_marks::repaint(self, slot, true),
        }
    }
}

/// How the render side repaints the boxes of an element whose rows it installs.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Repaint {
    None,
    Draw,
    DrawAndHitTest,
}

/// How the moves of `group`, an element's row and those of its pseudo-elements, repaint the boxes they style. As the
/// host's install of the rows, it reads their damages as one: a move that only updates the values of the element's
/// visual contexts, as an opacity or transform change under `will-change` does, patches the visual context node and
/// repaints no box.
fn group_repaint(group: &[FfiStyleDelta]) -> Repaint {
    use crate::css::style::pipeline::main_effects::record_damage_hit_tests_again;
    if !group
        .iter()
        .any(|row| row.record_damage & FfiStyleInvalidationField::LevelMask as u32 >= REPAINT_LEVEL)
    {
        return Repaint::None;
    }
    if group.iter().any(|row| record_damage_hit_tests_again(row.record_damage)) {
        Repaint::DrawAndHitTest
    } else {
        Repaint::Draw
    }
}

#[cfg(test)]
mod tests {
    use super::{Repaint, group_repaint};
    use crate::css::style::bridge::{FfiStyleDelta, FfiStyleInvalidationField};

    fn row(pseudo_kind: u8, record_damage: u32) -> FfiStyleDelta {
        FfiStyleDelta {
            style_node: 1,
            old_style_record: 1,
            new_style_record: 2,
            reaction: 0,
            inherited_style_groups: 0,
            pseudo_kind,
            explicitly_inherited_groups: 0,
            record_damage: record_damage | FfiStyleInvalidationField::EngineComputed as u32,
            owes_an_animation_plan: false,
            owes_a_transition_step: false,
            composed_by_the_host: false,
        }
    }

    #[test]
    fn only_moves_that_repaint_repaint_the_boxes_of_their_element() {
        let repaint = 1;
        let relayout = 2;
        let visual_contexts = |level: u32| level << FfiStyleInvalidationField::VisualContextShift as u32;
        let element = u8::MAX;
        let before = 0;
        assert_eq!(group_repaint(&[row(element, 0)]), Repaint::None);
        // An opacity or transform change under will-change only updates the values of the element's visual contexts.
        assert_eq!(group_repaint(&[row(element, visual_contexts(1))]), Repaint::None);
        assert_eq!(group_repaint(&[row(element, repaint)]), Repaint::Draw);
        assert_eq!(
            group_repaint(&[row(element, repaint | visual_contexts(2))]),
            Repaint::DrawAndHitTest
        );
        assert_eq!(group_repaint(&[row(element, relayout)]), Repaint::DrawAndHitTest);
        // The damages of an element's moves and those of its pseudo-elements repaint all of their boxes.
        assert_eq!(
            group_repaint(&[row(element, visual_contexts(1)), row(before, repaint)]),
            Repaint::Draw
        );
        assert_eq!(
            group_repaint(&[
                row(element, repaint),
                row(before, FfiStyleInvalidationField::AffectsHitTesting as u32)
            ]),
            Repaint::DrawAndHitTest
        );
    }
}
