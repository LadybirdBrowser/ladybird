/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::delta::VisualContextTreeDelta;
use super::dirty::VisualContextUpdateScope;
use super::reconcile::{NodeAction, NodeEvent};
use crate::css::style::tree::StyleNodeID;
use crate::fast_hash::FastMap;
use crate::layout::node_data::NodeSlotId;

/// One line of a visual context trace: the box it is about, named by its element where it has one, and what it says.
#[derive(Clone)]
pub(crate) struct TraceLine {
    pub(crate) indented: bool,
    slot: Option<NodeSlotId>,
    pub(crate) element: Option<StyleNodeID>,
    pub(crate) name_without_element: &'static str,
    pub(crate) text: String,
}

/// What the updates of the visual context tree did since a test began tracing them.
#[derive(Clone, Default)]
pub(crate) struct VisualContextTrace {
    pub(crate) lines: Vec<TraceLine>,
    /// The elements of the boxes removed since the last update, which the update releases the nodes of.
    removed_box_elements: FastMap<NodeSlotId, Option<StyleNodeID>>,
}

fn scope_name(scope: VisualContextUpdateScope) -> &'static str {
    match scope {
        VisualContextUpdateScope::DirtyPath => "DIRTY-PATH",
        VisualContextUpdateScope::EveryBox => "EVERY-BOX",
        VisualContextUpdateScope::FreshTree => "FRESH-TREE",
    }
}

fn action_name(action: NodeAction) -> &'static str {
    match action {
        NodeAction::Allocated { freed_slot: false } => "ALLOCATED",
        NodeAction::Allocated { freed_slot: true } => "ALLOCATED (freed slot)",
        NodeAction::Repurposed => "REPURPOSED",
        NodeAction::Updated => "UPDATED",
        NodeAction::Released => "RELEASED",
    }
}

impl VisualContextTrace {
    pub(crate) fn note_removed_box(&mut self, slot: NodeSlotId, element: Option<StyleNodeID>) {
        self.removed_box_elements.insert(slot, element);
    }

    /// Opens the block of an update, whose header the update finishes once it knows its scope.
    pub(crate) fn begin_update(&mut self) -> usize {
        self.lines.push(TraceLine {
            indented: false,
            slot: None,
            element: None,
            name_without_element: "",
            text: String::new(),
        });
        self.lines.len() - 1
    }

    /// Adds what an update did to the nodes of the box in `slot`, which the update names once it is over.
    pub(crate) fn note_box_events(&mut self, slot: NodeSlotId, events: &[NodeEvent]) {
        for event in events {
            self.lines.push(TraceLine {
                indented: true,
                slot: Some(slot),
                element: None,
                name_without_element: "(anonymous)",
                text: format!(" {} {}", event.role, action_name(event.action)),
            });
        }
    }

    /// Adds the nodes an update released of the box in `slot`, which was removed.
    pub(crate) fn note_removed_box_events(&mut self, slot: NodeSlotId, events: &[NodeEvent]) {
        let element = self.removed_box_elements.get(&slot).copied().flatten();
        for event in events {
            self.lines.push(TraceLine {
                indented: true,
                slot: None,
                element,
                name_without_element: "(removed)",
                text: format!(" {} {} (removed box)", event.role, action_name(event.action)),
            });
        }
    }

    /// Finishes the block `header` opened: the scope the update asked for and the one it ran, and whether it gave the
    /// tree a new structural epoch or needs a new recording. `name` answers the element of a box the block is about,
    /// and whether it is the viewport.
    pub(crate) fn finish_update(
        &mut self,
        header: usize,
        asked: VisualContextUpdateScope,
        ran: VisualContextUpdateScope,
        delta: &VisualContextTreeDelta,
        name: impl Fn(NodeSlotId) -> (Option<StyleNodeID>, bool),
    ) {
        self.removed_box_elements.clear();
        for line in &mut self.lines[header..] {
            if let Some(slot) = line.slot.take() {
                let (element, is_viewport) = name(slot);
                if is_viewport {
                    line.name_without_element = "@viewport";
                } else {
                    line.element = element;
                }
            }
        }
        self.lines[header].text = if asked == ran {
            format!("update {}", scope_name(ran))
        } else {
            format!("update {} -> {}", scope_name(asked), scope_name(ran))
        };
        for (applies, text) in [
            (delta.structural_epoch_changed, "epoch NEW"),
            (delta.requires_display_list_recording, "recording NEEDED"),
        ] {
            if applies {
                self.lines.push(TraceLine {
                    indented: true,
                    slot: None,
                    element: None,
                    name_without_element: "",
                    text: text.into(),
                });
            }
        }
    }
}
