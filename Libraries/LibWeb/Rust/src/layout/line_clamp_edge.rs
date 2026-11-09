/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixelRect;
use crate::fast_hash::FastMap;
use crate::layout::formatting_context;
use crate::layout::fragment_overflow;
use crate::layout::fragment_tree::{self, FragmentLink};
use crate::layout::geometry::Frame;
use crate::layout::layout_pass::LayoutPass;
use crate::layout::node_data::NodeSlotId;
use crate::layout::node_facts::{self, NodeFacts};
use crate::layout::used_values::FfiCssPixelSize;
use smallvec::SmallVec;

/// Clips each float in the formatting context of a line-clamp container's `children` at the container's block-end
/// content edge: the clip goes on the float's link, relative to the float's border box. The container's contents use
/// `frame`, and its content box has the physical size `content_size`.
pub(crate) fn clip_floats_at_block_end_content_edge(
    pass: &LayoutPass<'_>,
    container: NodeSlotId,
    frame: Frame,
    content_size: FfiCssPixelSize,
    children: &mut [FragmentLink],
) {
    let mut walk = ClampEdgeWalk {
        pass,
        area_before_block_end_edge: frame.half_plane_before_block_end(content_size),
        content_boxes: FastMap::default(),
    };
    walk.content_boxes.insert(
        container,
        CssPixelRect::new(
            Default::default(),
            Default::default(),
            content_size.width,
            content_size.height,
        ),
    );
    for (index, link) in walk.changed_links_among(children) {
        children[index] = link;
    }
}

struct ClampEdgeWalk<'walk, 'pass> {
    pass: &'walk LayoutPass<'pass>,
    /// Relative to the container's content box, as every rect of the walk is.
    area_before_block_end_edge: CssPixelRect,
    /// The physical content box of each box in the container's formatting context that the walk entered.
    content_boxes: FastMap<NodeSlotId, CssPixelRect>,
}

enum LinkOutcome {
    Unchanged,
    Changed(FragmentLink),
    ContainingBlockNotEntered,
}

impl ClampEdgeWalk<'_, '_> {
    fn changed_links_among(&mut self, links: &[FragmentLink]) -> SmallVec<[(usize, FragmentLink); 2]> {
        let mut changed_links = SmallVec::new();
        // NB: A link can be held beside its containing block, which another of the links places, so it waits for that.
        let mut waiting_links: SmallVec<[usize; 4]> = (0..links.len()).collect();
        loop {
            let waiting_before = waiting_links.len();
            waiting_links.retain(|index| match self.link_with_clipped_floats(&links[*index]) {
                LinkOutcome::Unchanged => false,
                LinkOutcome::Changed(link) => {
                    changed_links.push((*index, link));
                    false
                }
                LinkOutcome::ContainingBlockNotEntered => true,
            });
            if waiting_links.is_empty() || waiting_links.len() == waiting_before {
                return changed_links;
            }
        }
    }

    fn link_with_clipped_floats(&mut self, link: &FragmentLink) -> LinkOutcome {
        let Some(&containing_block) = self.content_boxes.get(&link.placement.containing_block) else {
            return LinkOutcome::ContainingBlockNotEntered;
        };
        if link.placement.shown_as.facts.is_invisible_for_line_clamp {
            return LinkOutcome::Unchanged;
        }
        let fragment = &link.fragment;
        let data = self.pass.arena().data(fragment.node);
        let style = node_facts::node_style_view(data);
        let content_box = CssPixelRect::from_location_and_size(link.offset.into(), fragment.sides.content_size.into())
            .translated_by(containing_block.location());

        if node_facts::node_is_floating(data, style) {
            let border_box = fragment_overflow::physical_border_box_around(content_box, fragment);
            let clip = Some(self.area_before_block_end_edge.translated(-border_box.x, -border_box.y));
            if link.placement.shown_as.facts.clip_decided_by_parent == clip {
                return LinkOutcome::Unchanged;
            }
            let mut link = link.clone();
            link.placement.shown_as.facts.clip_decided_by_parent = clip;
            return LinkOutcome::Changed(link);
        }

        // NB: Line clamping does not reach into a box that establishes a formatting context of its own, and a nested
        //     line-clamp container clips the floats in its formatting context at its own edge.
        if fragment.line_clamp.has_line_clamp_point
            || formatting_context::formatting_context_type_created_by_box(NodeFacts::new(self.pass, fragment.node))
                .is_some()
        {
            return LinkOutcome::Unchanged;
        }
        // NB: An inline box broken into line pieces has no content box of its own to place the links it holds in. A
        //     block can have no children of its own while the container holds floats beside it.
        if !node_facts::node_is_fragmented_inline(data, style) {
            self.content_boxes.insert(fragment.node, content_box);
        }
        if fragment.children.is_empty() {
            return LinkOutcome::Unchanged;
        }
        let changed_children = self.changed_links_among(&fragment.children);
        if changed_children.is_empty() {
            return LinkOutcome::Unchanged;
        }
        let mut children = fragment.children.clone();
        for (index, child) in changed_children {
            children[index] = child;
        }
        LinkOutcome::Changed(FragmentLink {
            fragment: fragment_tree::fragment_with_children(self.pass, fragment, children),
            ..link.clone()
        })
    }
}
