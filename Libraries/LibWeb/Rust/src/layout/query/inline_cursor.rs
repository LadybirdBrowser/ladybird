/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Caret positions on the lines layout produced: the line a position is on and its caret there. Geometry is in the
//! pre-transform space of absolute rects; a caller maps it into the viewport through the context the line content is
//! painted in.

use crate::css::css_enums::writing_mode;
use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::layout::RenderedTextBoundary;
use crate::layout::inline_content::LineRecord;
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::layout::node_facts;
use crate::painting::paint_read::{PaintRead, PaintRow};
use crate::painting::paintable_data::{FragmentRecord, SELECTION_STATE_START_AND_END};
use crate::painting::paintable_geometry::{self, LineRootPositions};
use crate::painting::paintable_rows::with_inline_pieces;
use crate::painting::text_fragment::{self, CaretMatch};
use crate::painting::{node_painting, paint_order};

/// A line of the line root `root`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct InlineLine {
    pub(crate) root: NodeSlotId,
    pub(crate) index: u32,
}

/// Where on its line a position is: in the fragment with this index in the line root's fragments, or on a line that
/// has none.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct LinePosition {
    pub(crate) line: InlineLine,
    pub(crate) fragment: Option<u32>,
}

#[derive(Clone, Copy)]
pub(crate) enum CaretPosition {
    /// `offset` in the DOM text whose layout node is `text`. Where one line ends and the next starts at `offset`,
    /// `downstream` picks the next.
    Text {
        text: NodeSlotId,
        offset: usize,
        downstream: bool,
    },
    BesideAtomicInline {
        node: NodeSlotId,
        after: bool,
    },
    /// Before the <br> `node`, at the end of the line it ends.
    BeforeForcedBreak {
        node: NodeSlotId,
    },
    /// At the start of the box `node`, which lays out no line content around the position.
    StartOfBox {
        node: NodeSlotId,
    },
}

pub(crate) struct CaretRect {
    pub(crate) rect: CssPixelRect,
    /// The box whose caret color the caret takes.
    pub(crate) style_source: NodeSlotId,
    /// The line root whose line holds the caret, or the box at whose start it is.
    pub(crate) owner: NodeSlotId,
    /// The node of the line content beside the caret.
    pub(crate) node: NodeSlotId,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum LineEdge {
    Start,
    End,
    Left,
    Right,
}

/// A line of a text node: the DOM offsets it renders, with the whitespace hanging at its soft wrap between `end_offset`
/// and `end_offset_with_trailing_whitespace`. A line that a preserved newline ends with nothing on it has no fragments.
pub(crate) struct TextLine {
    pub(crate) line: InlineLine,
    pub(crate) start_offset: usize,
    pub(crate) end_offset: usize,
    pub(crate) end_offset_with_trailing_whitespace: usize,
    pub(crate) has_fragments: bool,
}

pub(crate) struct InlineCursor<'a, A: PaintRead> {
    arena: &'a A,
}

impl<'a, A: PaintRead> InlineCursor<'a, A> {
    pub(crate) fn new(arena: &'a A) -> Self {
        Self { arena }
    }

    /// The line `offset` in the DOM text whose layout node is `text` is on.
    pub(crate) fn line_of(&self, text: NodeSlotId, offset: usize, downstream: bool) -> Option<LinePosition> {
        let arena = self.arena;
        let fragments = arena.text_fragments(text);
        let nodes = fragments.as_slice();
        let mut fallback = None;
        let mut direct = None;
        text_fragment::for_each_fragment_of_nodes(arena, nodes, |root, index, fragment| {
            // A caret at the end of the text in front of a writing suggestion is in front of the suggestion, whichever
            // way it leans: The text after the caret starts only after the suggestion.
            match text_fragment::caret_match(fragment, offset, downstream) {
                CaretMatch::None => true,
                CaretMatch::SoftWrapFallback if !arena.text_precedes_writing_suggestion(fragment.layout_node) => {
                    fallback = fallback.or(Some((root, index, fragment.line_index)));
                    true
                }
                CaretMatch::SoftWrapFallback | CaretMatch::Direct => {
                    direct = Some((root, index, fragment.line_index));
                    false
                }
            }
        });
        if let Some((root, fragment, index)) = direct.or(fallback) {
            return Some(LinePosition {
                line: InlineLine { root, index },
                fragment: Some(fragment),
            });
        }
        // A preserved newline that ends a line with nothing on it gives the line no fragment.
        nodes.iter().find_map(|&node| {
            let root = text_fragment::containing_block_paintable_of_node(arena, node)?;
            let side = arena.committed_side_data(root);
            let index = side.lines().iter().position(|line| {
                line.fragment_count == 0
                    && line.forced_break.is_some_and(|position| {
                        nodes.contains(&position.node)
                            && arena.dom_offset_for_rendered_text_offset(
                                position.node,
                                position.rendered_offset as usize,
                                RenderedTextBoundary::Start,
                            ) == offset
                    })
            })?;
            Some(LinePosition {
                line: InlineLine {
                    root,
                    index: index as u32,
                },
                fragment: None,
            })
        })
    }

    pub(crate) fn caret_rect(&self, position: CaretPosition) -> Option<CaretRect> {
        match position {
            CaretPosition::Text {
                text,
                offset,
                downstream,
            } => {
                let position = self.line_of(text, offset, downstream)?;
                match position.fragment {
                    Some(index) => {
                        let side = self.arena.committed_side_data(position.line.root);
                        let fragment = &side.fragments()[index as usize];
                        Some(self.caret_rect_in_fragment(position.line.root, fragment, offset, downstream))
                    }
                    None => self.caret_rect_on_empty_line(position.line),
                }
            }
            CaretPosition::BesideAtomicInline { node, after } => self.caret_rect_beside_atomic_inline(node, after),
            CaretPosition::BeforeForcedBreak { node } => self.caret_rect_before_forced_break(node),
            CaretPosition::StartOfBox { node } => self.caret_rect_at_start_of_box(node),
        }
    }

    /// The caret at `offset` in the DOM text of the text fragment `fragment` of the line root `root`.
    pub(crate) fn caret_rect_in_fragment(
        &self,
        root: NodeSlotId,
        fragment: &FragmentRecord,
        offset: usize,
        downstream: bool,
    ) -> CaretRect {
        let arena = self.arena;
        let mut rect = CssPixelRect::default();
        if let Some(offsets) =
            text_fragment::compute_selection_offsets(arena, fragment, SELECTION_STATE_START_AND_END, offset, offset)
        {
            rect = text_fragment::rect_for_selection_offsets(arena, fragment, offsets);
            if let Some(coordinate) = fragment
                .glyph_run
                .as_ref()
                .and_then(|run| run.cluster_geometry().x_for_offset(offsets.start, downstream))
            {
                let origin = text_fragment::absolute_rect(arena, fragment);
                let coordinate = CssPixels::nearest_value_for_f32(coordinate);
                if text_fragment::is_horizontal(fragment) {
                    rect.x = origin.x + coordinate;
                } else {
                    rect.y = origin.y + coordinate;
                }
            }
        }
        CaretRect {
            rect,
            style_source: fragment.style_source,
            owner: root,
            node: fragment.layout_node,
        }
    }

    fn line_rect(&self, line: InlineLine, record: &LineRecord) -> CssPixelRect {
        let position = LineRootPositions::new(self.arena, line.root).of_fragment(record.box_fragment);
        CssPixelRect::from(record.rect).translated_by(position)
    }

    /// A caret as tall as the font of `style_source`, at the inline start of `area` and where layout puts text across a
    /// line of `line_height` from its block start, or across its block extent: the smaller half of the leading is on
    /// the ascent side.
    fn caret_rect_at_inline_start(
        &self,
        area: CssPixelRect,
        line_height: Option<CssPixels>,
        style_source: NodeSlotId,
    ) -> Option<CssPixelRect> {
        let style = self.arena.node_style_if_live(style_source)?;
        let horizontal = style.writing_mode() == writing_mode::HORIZONTAL_TB;
        let line_height = line_height.unwrap_or(if horizontal { area.height } else { area.width });
        let font_height = CssPixels::from_integer((style.font_ascent() + style.font_descent()) as i64);
        let height = font_height.min(line_height);
        let leading = line_height - height;
        let leading_above = leading.scaled(0.5).floor();
        let one = CssPixels::from_integer(1);
        let at_end = style.inline_axis_is_reverse();
        Some(if horizontal {
            CssPixelRect {
                x: if at_end { area.right() - one } else { area.x },
                y: area.y + leading_above,
                width: one,
                height,
            }
        } else {
            CssPixelRect {
                x: area.x + leading - leading_above,
                y: if at_end { area.bottom() - one } else { area.y },
                width: height,
                height: one,
            }
        })
    }

    fn caret_rect_on_empty_line(&self, line: InlineLine) -> Option<CaretRect> {
        let side = self.arena.committed_side_data(line.root);
        let record = side.lines().get(line.index as usize)?;
        let node = record.forced_break?.node;
        let style_source = if self.arena.node_kind_if_live(node) == Some(NodeKind::BreakNode) {
            node
        } else {
            self.arena.node_parent_if_live(node)?
        };
        Some(CaretRect {
            rect: self.caret_rect_at_inline_start(self.line_rect(line, record), None, style_source)?,
            style_source,
            owner: line.root,
            node,
        })
    }

    // INTEROP: A caret beside an atomic inline spans its line box, with the inline coordinate at the box's edge.
    fn caret_rect_beside_atomic_inline(&self, node: NodeSlotId, after: bool) -> Option<CaretRect> {
        let arena = self.arena;
        let owner = text_fragment::containing_block_paintable_of_node(arena, node)?;
        let side = arena.committed_side_data(owner);
        let fragment = side
            .fragments()
            .iter()
            .find(|fragment| fragment.layout_node == node && fragment.is_atomic_inline)?;
        let fragment_rect = text_fragment::absolute_rect(arena, fragment);
        let item_rect = paintable_geometry::absolute_border_box_rect(arena, node);
        let mut rect = text_fragment::absolute_line_box_rect(arena, owner, fragment);
        // The line box is unpositioned; move its block-axis extent with the atomic box and its inline ancestors.
        rect.x += item_rect.x - fragment_rect.x + fragment.relpos_delta.x;
        rect.y += item_rect.y - fragment_rect.y + fragment.relpos_delta.y;
        let at_end = after != arena.node_style_if_live(node)?.inline_axis_is_reverse();
        let thickness = CssPixels::from_integer(1);
        if text_fragment::is_horizontal(fragment) {
            rect.x = if at_end { item_rect.right() } else { item_rect.x };
            rect.width = thickness;
        } else {
            rect.y = if at_end { item_rect.bottom() } else { item_rect.y };
            rect.height = thickness;
        }
        Some(CaretRect {
            rect,
            style_source: node,
            owner,
            node,
        })
    }

    fn caret_rect_before_forced_break(&self, node: NodeSlotId) -> Option<CaretRect> {
        let root = text_fragment::containing_block_paintable_of_node(self.arena, node)?;
        let side = self.arena.committed_side_data(root);
        let index = side
            .lines()
            .iter()
            .position(|line| line.forced_break.is_some_and(|position| position.node == node))?;
        let record = &side.lines()[index];
        if record.fragment_count == 0 {
            return self.caret_rect_on_empty_line(InlineLine {
                root,
                index: index as u32,
            });
        }
        let last = &side.fragments()[(record.first_fragment + record.fragment_count - 1) as usize];
        if last.is_atomic_inline {
            return self.caret_rect_beside_atomic_inline(last.layout_node, true);
        }
        Some(self.caret_rect_in_fragment(root, last, last.dom_end_offset_with_trailing_whitespace, false))
    }

    fn caret_rect_at_start_of_box(&self, node: NodeSlotId) -> Option<CaretRect> {
        let arena = self.arena;
        if !arena.paintable_row_is_populated(node) {
            return None;
        }
        // An inline box with content has no caret of its own: interrupting blocks leave only placeholder pieces, but they
        // are content too.
        if node_painting::is_fragmented_inline(arena, node) {
            let mut has_content = paint_order::first_paint_child(arena, node).is_some();
            with_inline_pieces(arena, node, |piece| {
                has_content |= !piece.is_geometry_only_placeholder;
                !has_content
            });
            if has_content {
                return None;
            }
        }
        let area = paintable_geometry::absolute_rect(self.arena, node);
        let line_height = self.arena.node_style_if_live(node)?.line_height();
        Some(CaretRect {
            rect: self.caret_rect_at_inline_start(area, Some(line_height), node)?,
            style_source: node,
            owner: node,
            node,
        })
    }

    /// The next line after `line` in its line root, or the previous one, that holds a caret position.
    pub(crate) fn adjacent_line(&self, line: InlineLine, forward: bool) -> Option<InlineLine> {
        let side = self.arena.committed_side_data(line.root);
        let mut index = line.index as usize;
        loop {
            index = if forward { index + 1 } else { index.checked_sub(1)? };
            let record = side.lines().get(index)?;
            if (record.fragment_count > 0 && !record.has_block_level_box) || record.forced_break.is_some() {
                return Some(InlineLine {
                    root: line.root,
                    index: index as u32,
                });
            }
        }
    }

    fn line_is_horizontal(&self, line: InlineLine) -> bool {
        let side = self.arena.committed_side_data(line.root);
        side.lines()
            .get(line.index as usize)
            .is_none_or(|record| record.writing_mode == writing_mode::HORIZONTAL_TB)
    }

    /// The middle of `line` along the block axis.
    pub(crate) fn line_block_coordinate(&self, line: InlineLine) -> CssPixels {
        let side = self.arena.committed_side_data(line.root);
        let Some(record) = side.lines().get(line.index as usize) else {
            return CssPixels::default();
        };
        let rect = self.line_rect(line, record);
        let center = rect.center();
        if record.writing_mode == writing_mode::HORIZONTAL_TB {
            center.y
        } else {
            center.x
        }
    }

    /// The inline coordinate of the caret at `position` on `line`, which a move to another line keeps.
    pub(crate) fn caret_inline_coordinate(&self, line: InlineLine, position: CaretPosition) -> Option<CssPixels> {
        let caret = self.caret_rect(position)?;
        let root = paintable_geometry::absolute_line_root_position(self.arena, line.root, line.index as usize);
        let (coordinate, root_coordinate) = if self.line_is_horizontal(line) {
            (caret.rect.x, root.x)
        } else {
            (caret.rect.y, root.y)
        };
        // INTEROP: Vertical navigation takes the pixel-rounded leading edge of a one-pixel caret centered on the text
        //          boundary, which floors the coordinate in the line root.
        Some((coordinate - root_coordinate).floor() + root_coordinate)
    }

    /// The fragments of `line` that `accepts` takes, in line order.
    fn fragments_on_line<'b>(
        &self,
        side: &'b crate::painting::paintable_data::CommittedSideData,
        line: InlineLine,
        accepts: impl Fn(&FragmentRecord) -> bool,
    ) -> impl Iterator<Item = &'b FragmentRecord> {
        let range = side.lines().get(line.index as usize).map_or(0..0, |record| {
            record.first_fragment as usize..(record.first_fragment + record.fragment_count) as usize
        });
        side.fragments()[range].iter().filter(move |fragment| accepts(fragment))
    }

    /// The position in front of the <br> or preserved newline that ends `line`.
    fn position_before_forced_break(&self, line: InlineLine) -> Option<CaretPosition> {
        let side = self.arena.committed_side_data(line.root);
        let position = side.lines().get(line.index as usize)?.forced_break?;
        if self.arena.node_kind_if_live(position.node) == Some(NodeKind::BreakNode) {
            return Some(CaretPosition::BeforeForcedBreak { node: position.node });
        }
        Some(CaretPosition::Text {
            text: position.node,
            offset: self.arena.dom_offset_for_rendered_text_offset(
                position.node,
                position.rendered_offset as usize,
                RenderedTextBoundary::Start,
            ),
            downstream: true,
        })
    }

    /// The text position at `offset` in `fragment`, leaning to the line of the fragment.
    fn text_position_in_fragment(&self, fragment: &FragmentRecord, offset: usize) -> CaretPosition {
        let downstream = self
            .line_of(fragment.layout_node, offset, true)
            .is_none_or(|position| position.line.index == fragment.line_index);
        CaretPosition::Text {
            text: fragment.layout_node,
            offset,
            downstream,
        }
    }

    /// The caret position on `line` nearest the inline coordinate `x`, in the fragment `accepts` takes that is the last
    /// to start before `x`, or else the first, or in front of the line's forced break where it takes none.
    pub(crate) fn position_at_inline_coordinate(
        &self,
        line: InlineLine,
        x: CssPixels,
        accepts: impl Fn(&FragmentRecord) -> bool,
    ) -> Option<CaretPosition> {
        let arena = self.arena;
        let side = arena.committed_side_data(line.root);
        let horizontal = self.line_is_horizontal(line);
        let inline_start = |rect: CssPixelRect| if horizontal { rect.x } else { rect.y };
        let mut chosen = None;
        for fragment in self.fragments_on_line(&side, line, accepts) {
            let rect = text_fragment::absolute_rect(arena, fragment);
            if chosen.is_none() || inline_start(rect) <= x {
                chosen = Some((fragment, rect));
            }
        }
        let Some((fragment, rect)) = chosen else {
            return self.position_before_forced_break(line);
        };
        let mut point = rect.center();
        if horizontal {
            point.x = rect.x.max(x);
        } else {
            point.y = rect.y.max(x);
        }
        let offset = text_fragment::index_in_node_for_point(arena, fragment, point);
        Some(self.text_position_in_fragment(fragment, offset))
    }

    /// The caret position at `edge` of `line`, among the fragments `accepts` takes, or in front of the line's forced
    /// break where it takes none.
    pub(crate) fn line_edge(
        &self,
        line: InlineLine,
        edge: LineEdge,
        accepts: impl Fn(&FragmentRecord) -> bool,
    ) -> Option<CaretPosition> {
        let arena = self.arena;
        let side = arena.committed_side_data(line.root);
        let fragments = self.fragments_on_line(&side, line, accepts);
        let rect = |fragment: &&FragmentRecord| text_fragment::absolute_rect(arena, fragment);
        let fragment = match edge {
            LineEdge::Start => fragments.min_by_key(|fragment| fragment.dom_start_offset_in_node),
            LineEdge::End => fragments.max_by_key(|fragment| fragment.dom_end_offset_with_trailing_whitespace),
            LineEdge::Left => fragments.min_by_key(|fragment| {
                let rect = rect(fragment);
                if self.line_is_horizontal(line) { rect.x } else { rect.y }
            }),
            LineEdge::Right => fragments.max_by_key(|fragment| {
                let rect = rect(fragment);
                if self.line_is_horizontal(line) {
                    rect.right()
                } else {
                    rect.bottom()
                }
            }),
        };
        let Some(fragment) = fragment else {
            return self.position_before_forced_break(line);
        };
        let physical_edge = text_fragment::physical_edge_offset(arena, fragment, edge == LineEdge::Left);
        let offset = match (edge, physical_edge) {
            (LineEdge::Left | LineEdge::Right, Some((offset, at_cluster_end))) => {
                return Some(CaretPosition::Text {
                    text: fragment.layout_node,
                    offset,
                    downstream: !at_cluster_end,
                });
            }
            (LineEdge::End, _) => fragment.dom_end_offset_with_trailing_whitespace,
            _ => fragment.dom_start_offset_in_node,
        };
        Some(self.text_position_in_fragment(fragment, offset))
    }

    /// The lines of the DOM text whose layout node is `text`, in DOM order.
    pub(crate) fn lines_of_text(&self, text: NodeSlotId) -> Vec<TextLine> {
        let arena = self.arena;
        let fragments = arena.text_fragments(text);
        let nodes = fragments.as_slice();
        let mut lines: Vec<TextLine> = Vec::new();
        text_fragment::for_each_fragment_of_nodes(arena, nodes, |root, _, fragment| {
            let line = InlineLine {
                root,
                index: fragment.line_index,
            };
            match lines.last_mut() {
                Some(last) if last.has_fragments && last.line == line => {
                    last.start_offset = last.start_offset.min(fragment.dom_start_offset_in_node);
                    last.end_offset = last.end_offset.max(fragment.dom_end_offset_in_node);
                    last.end_offset_with_trailing_whitespace = last
                        .end_offset_with_trailing_whitespace
                        .max(fragment.dom_end_offset_with_trailing_whitespace);
                }
                _ => lines.push(TextLine {
                    line,
                    start_offset: fragment.dom_start_offset_in_node,
                    end_offset: fragment.dom_end_offset_in_node,
                    end_offset_with_trailing_whitespace: fragment.dom_end_offset_with_trailing_whitespace,
                    has_fragments: true,
                }),
            }
            true
        });
        let mut roots: Vec<NodeSlotId> = nodes
            .iter()
            .filter_map(|&node| text_fragment::containing_block_paintable_of_node(arena, node))
            .collect();
        roots.dedup();
        for root in roots {
            for (index, record) in arena.committed_side_data(root).lines().iter().enumerate() {
                if let Some(position) = record.forced_break
                    && record.fragment_count == 0
                    && nodes.contains(&position.node)
                {
                    let offset = arena.dom_offset_for_rendered_text_offset(
                        position.node,
                        position.rendered_offset as usize,
                        RenderedTextBoundary::Start,
                    );
                    lines.push(TextLine {
                        line: InlineLine {
                            root,
                            index: index as u32,
                        },
                        start_offset: offset,
                        end_offset: offset,
                        end_offset_with_trailing_whitespace: offset,
                        has_fragments: false,
                    });
                }
            }
        }
        lines.sort_by_key(|line| line.start_offset);
        lines
    }
}

/// The caret positions a position between the children of the box `container` can take, in the order to try them:
/// beside an atomic inline child, at the end of the text `before` (whose DOM text is `before_length` long) or at the
/// start of the text `after`, before a <br>, or else at the start of the box. `before` and `after` are the boxes of the
/// children around the position, or `NodeSlotId::INVALID`.
pub(crate) fn caret_positions_between_children(
    arena: &impl PaintRead,
    container: NodeSlotId,
    before: NodeSlotId,
    before_length: usize,
    after: NodeSlotId,
) -> impl Iterator<Item = CaretPosition> {
    let is_atomic_inline = |node| arena.node(node).is_some_and(|row| row.is_atomic_inline());
    let is_text = |node| arena.node_kind_if_live(node).is_some_and(node_facts::kind_is_text);
    let is_break = |node| arena.node_kind_if_live(node) == Some(NodeKind::BreakNode);
    [
        is_atomic_inline(before).then_some(CaretPosition::BesideAtomicInline {
            node: before,
            after: true,
        }),
        is_atomic_inline(after).then_some(CaretPosition::BesideAtomicInline {
            node: after,
            after: false,
        }),
        is_text(before).then_some(CaretPosition::Text {
            text: before,
            offset: before_length,
            downstream: true,
        }),
        is_text(after).then_some(CaretPosition::Text {
            text: after,
            offset: 0,
            downstream: true,
        }),
        is_break(after).then_some(CaretPosition::BeforeForcedBreak { node: after }),
        Some(CaretPosition::StartOfBox { node: container }),
    ]
    .into_iter()
    .flatten()
}
