/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Caret positions on the lines layout produced: the line a position is on and its caret there. Geometry is in the
//! pre-transform space of absolute rects; a caller maps it into the viewport through the context the line content is
//! painted in.

use crate::css::css_enums::{white_space_collapse, writing_mode};
use crate::css::css_pixels::{CssPixelPoint, CssPixelRect, CssPixels};
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
#[repr(u8)]
pub enum LineEdge {
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

#[derive(Clone, Copy)]
struct LineExtent {
    inline_start: CssPixels,
    inline_end: CssPixels,
    block_start: CssPixels,
    block_end: CssPixels,
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

    /// The line the caret at `position` is on. A position at the start of a box with no line content is on none.
    pub(crate) fn line_of_position(&self, position: CaretPosition) -> Option<InlineLine> {
        match position {
            CaretPosition::Text {
                text,
                offset,
                downstream,
            } => Some(self.line_of(text, offset, downstream)?.line),
            CaretPosition::BesideAtomicInline { node, .. } => {
                let root = text_fragment::containing_block_paintable_of_node(self.arena, node)?;
                let side = self.arena.committed_side_data(root);
                let fragment = side
                    .fragments()
                    .iter()
                    .find(|fragment| fragment.layout_node == node && fragment.is_atomic_inline)?;
                Some(InlineLine {
                    root,
                    index: fragment.line_index,
                })
            }
            CaretPosition::BeforeForcedBreak { node } => self.line_ended_by_forced_break(node),
            CaretPosition::StartOfBox { .. } => None,
        }
    }

    /// The line that holds `line` outside the atomic inline boxes its line root is in.
    pub(crate) fn line_outside_atomic_inlines(&self, mut line: InlineLine) -> InlineLine {
        while self.arena.node(line.root).is_some_and(|row| row.is_atomic_inline()) {
            let Some(outer_line) = self.line_of_position(CaretPosition::BesideAtomicInline {
                node: line.root,
                after: false,
            }) else {
                break;
            };
            line = outer_line;
        }
        line
    }

    /// The line the <br> `node` ends.
    fn line_ended_by_forced_break(&self, node: NodeSlotId) -> Option<InlineLine> {
        let root = text_fragment::containing_block_paintable_of_node(self.arena, node)?;
        let side = self.arena.committed_side_data(root);
        let index = side
            .lines()
            .iter()
            .position(|line| line.forced_break.is_some_and(|position| position.node == node))?;
        Some(InlineLine {
            root,
            index: index as u32,
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
        let line = self.line_ended_by_forced_break(node)?;
        let side = self.arena.committed_side_data(line.root);
        let record = &side.lines()[line.index as usize];
        if record.fragment_count == 0 {
            return self.caret_rect_on_empty_line(line);
        }
        let last = &side.fragments()[(record.first_fragment + record.fragment_count - 1) as usize];
        if last.is_atomic_inline {
            return self.caret_rect_beside_atomic_inline(last.layout_node, true);
        }
        Some(self.caret_rect_in_fragment(line.root, last, last.dom_end_offset_with_trailing_whitespace, false))
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

    /// The inline coordinate of the caret at `position`, which a move to another line keeps.
    pub(crate) fn inline_coordinate_of(&self, position: CaretPosition) -> Option<CssPixels> {
        if let Some(line) = self.line_of_position(position) {
            return self.caret_inline_coordinate(line, position);
        }
        let caret = self.caret_rect(position)?;
        let horizontal = self
            .arena
            .node_style_if_live(caret.style_source)
            .is_none_or(|style| style.writing_mode() == writing_mode::HORIZONTAL_TB);
        Some(if horizontal { caret.rect.x } else { caret.rect.y })
    }

    /// The fragments of `line` that `accepts` takes, in line order. The block ellipsis is left out, since it shows none
    /// of the text whose offsets it carries.
    fn fragments_on_line<'b>(
        &self,
        side: &'b crate::painting::paintable_data::CommittedSideData,
        line: InlineLine,
        accepts: impl Fn(&FragmentRecord) -> bool,
    ) -> impl Iterator<Item = &'b FragmentRecord> {
        let range = side.lines().get(line.index as usize).map_or(0..0, |record| {
            record.first_fragment as usize..(record.first_fragment + record.fragment_count) as usize
        });
        side.fragments()[range]
            .iter()
            .filter(move |fragment| !fragment.is_block_ellipsis && accepts(fragment))
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

    /// The extents of the lines of `root` along the inline axis and along the block direction, with the block coordinate
    /// growing in the block direction, and the map from a point to those coordinates.
    fn line_extents(&self, root: NodeSlotId) -> (Vec<LineExtent>, impl Fn(CssPixelPoint) -> (CssPixels, CssPixels)) {
        let side = self.arena.committed_side_data(root);
        let (horizontal, block_axis_is_reverse) = match side.lines().first().map(|record| record.writing_mode) {
            Some(writing_mode::VERTICAL_RL | writing_mode::SIDEWAYS_RL) => (false, true),
            Some(writing_mode::HORIZONTAL_TB) | None => (true, false),
            _ => (false, false),
        };
        let logical = move |point: CssPixelPoint| match (horizontal, block_axis_is_reverse) {
            (true, _) => (point.x, point.y),
            (false, false) => (point.y, point.x),
            (false, true) => (point.y, -point.x),
        };
        let positions = LineRootPositions::new(self.arena, root);
        let extents = side
            .lines()
            .iter()
            .map(|record| {
                let rect = CssPixelRect::from(record.rect).translated_by(positions.of_fragment(record.box_fragment));
                let (inline_start, block_start) = logical(rect.location());
                let (inline_end, block_end) = logical(CssPixelPoint::new(rect.right(), rect.bottom()));
                LineExtent {
                    inline_start,
                    inline_end,
                    block_start: block_start.min(block_end),
                    block_end: block_start.max(block_end),
                }
            })
            .collect();
        // A point covers the pixel after it, which is before it in a reversed block direction.
        let logical_point = move |point: CssPixelPoint| {
            let (inline, block) = logical(point);
            (
                inline,
                if block_axis_is_reverse {
                    block - CssPixels::from_integer(1)
                } else {
                    block
                },
            )
        };
        (extents, logical_point)
    }

    /// The columns the lines of `extents` lie in, as the inline extent of their lines and the index of their first line.
    /// A column is a run of lines that goes on in the block direction.
    fn columns(extents: &[LineExtent]) -> Vec<(CssPixels, CssPixels, usize)> {
        let mut columns: Vec<(CssPixels, CssPixels, usize)> = Vec::new();
        for (index, extent) in extents.iter().enumerate() {
            match columns.last_mut() {
                Some(column) if index > 0 && extent.block_start >= extents[index - 1].block_start => {
                    column.0 = column.0.min(extent.inline_start);
                    column.1 = column.1.max(extent.inline_end);
                }
                _ => columns.push((extent.inline_start, extent.inline_end, index)),
            }
        }
        columns
    }

    /// `x`, an inline coordinate a move between lines keeps, moved from the column of the line root it falls in to the
    /// column of `line`, so that it keeps its place in the column.
    fn inline_coordinate_in_column_of(&self, line: InlineLine, x: CssPixels) -> CssPixels {
        let columns = Self::columns(&self.line_extents(line.root).0);
        if columns.len() < 2 {
            return x;
        }
        let Some(&(source_start, _, _)) = columns.iter().find(|&&(start, end, _)| start <= x && x < end) else {
            return x;
        };
        let target = columns.partition_point(|&(_, _, first_line)| first_line <= line.index as usize) - 1;
        x + columns[target].0 - source_start
    }

    /// The caret position for `point` among the lines of `root`: on the first line of the column nearest the point that
    /// ends past it in the block direction, or at the start of the column's first line before it, or at the end of its
    /// last line past it.
    pub(crate) fn position_for_point(
        &self,
        root: NodeSlotId,
        point: CssPixelPoint,
        accepts: &dyn Fn(&FragmentRecord) -> bool,
    ) -> Option<CaretPosition> {
        let (extents, logical) = self.line_extents(root);
        let (inline, block) = logical(point);
        let side = self.arena.committed_side_data(root);
        let holds_caret = |index: usize| {
            let record = &side.lines()[index];
            (record.fragment_count > 0 && !record.has_block_level_box) || record.forced_break.is_some()
        };
        let columns = Self::columns(&extents);
        let distance_to_column = |&(start, end, _): &(CssPixels, CssPixels, usize)| {
            if inline < start {
                start - inline
            } else if inline > end {
                inline - end
            } else {
                CssPixels::default()
            }
        };
        let column = columns
            .iter()
            .enumerate()
            .min_by_key(|(_, column)| distance_to_column(column))
            .map(|(index, _)| index)?;
        let column_end = columns.get(column + 1).map_or(extents.len(), |next| next.2);
        let mut lines = (columns[column].2..column_end).filter(|&index| holds_caret(index));
        let first = lines.next()?;
        let line = |index: usize| InlineLine {
            root,
            index: index as u32,
        };
        if block < extents[first].block_start {
            return self.line_edge(line(first), LineEdge::Start, accepts);
        }
        let mut last = first;
        for index in std::iter::once(first).chain(lines) {
            if block < extents[index].block_end {
                return self
                    .position_in_float_beside_empty_line(root, line(index), extents[index], inline, &logical)
                    .or_else(|| self.position_on_line(line(index), point, accepts));
            }
            last = index;
        }
        self.line_edge(line(last), LineEdge::End, accepts)
    }

    // A line with no content of its own, such as one a <br> ends, holds the floats placed on it, which take a point over
    // their inline extent.
    fn position_in_float_beside_empty_line(
        &self,
        root: NodeSlotId,
        line: InlineLine,
        extent: LineExtent,
        inline: CssPixels,
        logical: &impl Fn(CssPixelPoint) -> (CssPixels, CssPixels),
    ) -> Option<CaretPosition> {
        let arena = self.arena;
        if arena.committed_side_data(root).lines()[line.index as usize].fragment_count > 0 {
            return None;
        }
        let float = arena.children(root).find(|&child| {
            if !arena.node(child).is_some_and(|row| row.is_floating()) || !arena.paintable_row_is_populated(child) {
                return false;
            }
            let rect = paintable_geometry::absolute_rect(arena, child);
            let (inline_start, block_start) = logical(rect.location());
            let (inline_end, block_end) = logical(CssPixelPoint::new(rect.right(), rect.bottom()));
            inline_start.min(inline_end) <= inline
                && inline < inline_start.max(inline_end)
                && block_start.min(block_end) < extent.block_end
                && extent.block_start < block_start.max(block_end)
        })?;
        Some(CaretPosition::StartOfBox { node: float })
    }

    /// The caret position on `line` nearest the inline coordinate `x`, which a move between lines kept, as for a point
    /// at the block start of the line.
    pub(crate) fn position_at_inline_coordinate(
        &self,
        line: InlineLine,
        x: CssPixels,
        accepts: impl Fn(&FragmentRecord) -> bool,
    ) -> Option<CaretPosition> {
        let side = self.arena.committed_side_data(line.root);
        let record = side.lines().get(line.index as usize)?;
        let rect = self.line_rect(line, record);
        let x = self.inline_coordinate_in_column_of(line, x);
        let point = match record.writing_mode {
            writing_mode::HORIZONTAL_TB => CssPixelPoint::new(x, rect.y),
            writing_mode::VERTICAL_RL | writing_mode::SIDEWAYS_RL => {
                CssPixelPoint::new(rect.right() - CssPixels::from_integer(1), x)
            }
            _ => CssPixelPoint::new(rect.x, x),
        };
        self.position_on_line(line, point, &accepts)
    }

    /// The caret position for `point` on `line`, in the fragment `accepts` takes that is the last to start before the
    /// point along the line, or else the first, or in front of the line's forced break where it takes none.
    fn position_on_line(
        &self,
        line: InlineLine,
        point: CssPixelPoint,
        accepts: &dyn Fn(&FragmentRecord) -> bool,
    ) -> Option<CaretPosition> {
        let arena = self.arena;
        let side = arena.committed_side_data(line.root);
        let horizontal = self.line_is_horizontal(line);
        let x = if horizontal { point.x } else { point.y };
        let inline_start = |rect: CssPixelRect| if horizontal { rect.x } else { rect.y };
        let mut chosen = None;
        for fragment in self.fragments_on_line(&side, line, accepts) {
            let rect = text_fragment::absolute_rect(arena, fragment);
            if chosen.is_none() || inline_start(rect) <= x {
                chosen = Some((fragment, rect));
            }
        }
        let Some((mut fragment, mut rect)) = chosen else {
            return self.position_before_forced_break(line);
        };
        // INTEROP: A point in the gap between two atomic inlines goes to the one after it.
        let inline_end = |rect: CssPixelRect| if horizontal { rect.right() } else { rect.bottom() };
        if fragment.is_atomic_inline
            && x >= inline_end(rect)
            && let Some(next) = self
                .fragments_on_line(&side, line, accepts)
                .find(|next| inline_start(text_fragment::absolute_rect(arena, next)) > x)
            && next.is_atomic_inline
        {
            fragment = next;
            rect = text_fragment::absolute_rect(arena, next);
        }
        // INTEROP: Past the end of a line, the position is at its end, after whitespace that hangs there, but before
        //          collapsible whitespace, which the line removes.
        if let Some(last) = self.fragments_on_line(&side, line, accepts).last()
            && !last.is_atomic_inline
        {
            let last_rect = text_fragment::absolute_rect(arena, last);
            let past_line_end = match (horizontal, last.direction == crate::css::css_enums::direction::LTR) {
                (true, true) => x >= last_rect.right(),
                (true, false) => x < last_rect.x,
                (false, true) => x >= last_rect.bottom(),
                (false, false) => x < last_rect.y,
            };
            if past_line_end {
                let collapsible = matches!(
                    last.white_space_collapse,
                    white_space_collapse::COLLAPSE | white_space_collapse::PRESERVE_BREAKS
                );
                let offset = if collapsible {
                    last.dom_end_offset_in_node
                } else {
                    last.dom_end_offset_with_trailing_whitespace
                };
                return Some(self.text_position_in_fragment(last, offset));
            }
        }
        if fragment.is_atomic_inline {
            return Some(self.position_for_point_at_atomic_inline(fragment.layout_node, line, point, accepts));
        }
        let mut point_in_fragment = point;
        if horizontal {
            point_in_fragment.x = rect.x.max(x);
        } else {
            point_in_fragment.y = rect.y.max(x);
        }
        let offset = text_fragment::index_in_node_for_point(arena, fragment, point_in_fragment);
        Some(self.text_position_in_fragment(fragment, offset))
    }

    /// The caret position for `point` at the atomic inline `node` on `line`. Replaced content takes it beside itself, on
    /// the side of the half it is over or of the line it is past, and so does a text control for a point outside it.
    /// Other boxes take it in their own lines, or at their start where they have no content.
    fn position_for_point_at_atomic_inline(
        &self,
        node: NodeSlotId,
        line: InlineLine,
        point: CssPixelPoint,
        accepts: &dyn Fn(&FragmentRecord) -> bool,
    ) -> CaretPosition {
        let arena = self.arena;
        let style = arena.node_style_if_live(node);
        let horizontal = style.is_none_or(|style| style.writing_mode() == writing_mode::HORIZONTAL_TB);
        let reverse = style.is_some_and(|style| style.inline_axis_is_reverse());
        let block_axis_is_reverse = style.is_some_and(|style| style.block_axis_is_reverse());
        let beside = |after| CaretPosition::BesideAtomicInline { node, after };
        // Whether the point is past `rect` in the block direction, if it is outside it that way. A point covers the
        // pixel after it, which is before it in a reversed block direction.
        let block_side = |rect: CssPixelRect| match (horizontal, block_axis_is_reverse) {
            (true, _) if point.y < rect.y => Some(false),
            (true, _) if point.y >= rect.bottom() => Some(true),
            (false, false) if point.x < rect.x => Some(false),
            (false, false) if point.x >= rect.right() => Some(true),
            (false, true) if point.x > rect.right() => Some(false),
            (false, true) if point.x <= rect.x => Some(true),
            _ => None,
        };
        let rect = paintable_geometry::absolute_border_box_rect(arena, node);
        let (inline, inline_start, inline_end) = if horizontal {
            (point.x, rect.x, rect.right())
        } else {
            (point.y, rect.y, rect.bottom())
        };
        let past_middle = (inline - inline_start > (inline_end - inline_start).scaled(0.5)) != reverse;
        if crate::painting::style_queries::is_replaced_box(arena, node) {
            let side = self.arena.committed_side_data(line.root);
            let line_rect = self.line_rect(line, &side.lines()[line.index as usize]);
            return beside(block_side(line_rect).unwrap_or(past_middle));
        }
        if matches!(
            arena.node_kind_if_live(node),
            Some(NodeKind::TextAreaBox | NodeKind::TextInputBox)
        ) {
            if inline < inline_start || inline >= inline_end {
                return beside((inline >= inline_end) != reverse);
            }
            return beside(block_side(rect).unwrap_or(past_middle));
        }
        if node_painting::has_lines(arena, node)
            && let Some(position) = self.position_for_point(node, point, accepts)
        {
            return position;
        }
        if arena.children(node).next().is_none() {
            return CaretPosition::StartOfBox { node };
        }
        beside(past_middle)
    }

    /// The caret position at `edge` of `line`, among the fragments `accepts` takes, or in front of the line's forced
    /// break where it takes none. The logical edges are those of the first and last fragment in line order.
    pub(crate) fn line_edge(
        &self,
        line: InlineLine,
        edge: LineEdge,
        accepts: impl Fn(&FragmentRecord) -> bool,
    ) -> Option<CaretPosition> {
        let arena = self.arena;
        let side = arena.committed_side_data(line.root);
        let mut fragments = self.fragments_on_line(&side, line, accepts);
        let horizontal = self.line_is_horizontal(line);
        let rect = |fragment: &&FragmentRecord| text_fragment::absolute_rect(arena, fragment);
        let fragment = match edge {
            LineEdge::Start => fragments.next(),
            LineEdge::End => fragments.last(),
            LineEdge::Left => fragments.min_by_key(|fragment| {
                let rect = rect(fragment);
                if horizontal { rect.x } else { rect.y }
            }),
            LineEdge::Right => fragments.max_by_key(|fragment| {
                let rect = rect(fragment);
                if horizontal { rect.right() } else { rect.bottom() }
            }),
        };
        let Some(fragment) = fragment else {
            return self.position_before_forced_break(line);
        };
        if fragment.is_atomic_inline {
            let reverse = arena
                .node_style_if_live(fragment.layout_node)
                .is_some_and(|style| style.inline_axis_is_reverse());
            return Some(CaretPosition::BesideAtomicInline {
                node: fragment.layout_node,
                after: match edge {
                    LineEdge::Start => false,
                    LineEdge::End => true,
                    LineEdge::Left => reverse,
                    LineEdge::Right => !reverse,
                },
            });
        }
        let offset = match (
            edge,
            text_fragment::physical_edge_offset(arena, fragment, edge == LineEdge::Left),
        ) {
            (LineEdge::Left | LineEdge::Right, Some((offset, at_cluster_end))) => {
                return Some(CaretPosition::Text {
                    text: fragment.layout_node,
                    offset,
                    downstream: !at_cluster_end,
                });
            }
            (LineEdge::End, _) if self.only_collapsible_whitespace_follows(fragment) => fragment.dom_end_offset_in_node,
            (LineEdge::End, _) => fragment.dom_end_offset_with_trailing_whitespace,
            _ => fragment.dom_start_offset_in_node,
        };
        Some(self.text_position_in_fragment(fragment, offset))
    }

    // INTEROP: Collapsed whitespace at the end of the text of a block is not a caret stop, unlike whitespace that hangs
    //          at a soft wrap.
    fn only_collapsible_whitespace_follows(&self, fragment: &FragmentRecord) -> bool {
        if !matches!(
            fragment.white_space_collapse,
            white_space_collapse::COLLAPSE | white_space_collapse::PRESERVE_BREAKS
        ) {
            return false;
        }
        let end = fragment.start + fragment.length_in_code_units;
        let is_space_or_tab = |unit: &u16| *unit == u16::from(b' ') || *unit == u16::from(b'\t');
        self.arena.rendered_text(fragment.layout_node).is_some_and(|rendered| {
            rendered
                .text
                .get(end..)
                .is_none_or(|rest| rest.iter().all(is_space_or_tab))
        })
    }

    /// The position nearest `offset` in the DOM text whose layout node is `text` that a line holds: the end of the
    /// rendered text before it, as for a position in collapsed whitespace after it, or else the start of the first line.
    pub(crate) fn nearest_text_position_on_a_line(&self, text: NodeSlotId, offset: usize) -> Option<CaretPosition> {
        let lines = self.lines_of_text(text);
        let (offset, downstream) = lines
            .iter()
            .rev()
            .find(|line| line.end_offset_with_trailing_whitespace <= offset)
            .map(|line| (line.end_offset_with_trailing_whitespace, false))
            .or_else(|| lines.first().map(|line| (line.start_offset, true)))?;
        Some(CaretPosition::Text {
            text,
            offset,
            downstream,
        })
    }

    /// The caret position at the start of the first line of the DOM text whose layout node is `text`, or at the end of
    /// its last line.
    pub(crate) fn text_edge(&self, text: NodeSlotId, end: bool) -> Option<CaretPosition> {
        let lines = self.lines_of_text(text);
        let (offset, downstream) = if end {
            (lines.last()?.end_offset, true)
        } else {
            let first = lines.first()?;
            let second_line_starts_later = lines
                .get(1)
                .is_none_or(|second| second.start_offset > first.start_offset);
            (first.start_offset, second_line_starts_later)
        };
        Some(CaretPosition::Text {
            text,
            offset,
            downstream,
        })
    }

    /// The first line of the DOM text whose layout node is `text`, or its last, where it holds rendered text of it.
    pub(crate) fn rendered_text_edge_line(&self, text: NodeSlotId, end: bool) -> Option<InlineLine> {
        let lines = self.lines_of_text(text);
        let line = if end { lines.last() } else { lines.first() }?;
        line.has_fragments.then_some(line.line)
    }

    /// The caret position one grapheme right of (`forward`) or left of `offset` in the DOM text whose layout node is
    /// `text`, where the grapheme boundary that way is `grapheme_boundary`. Where one line ends and the next starts at
    /// a soft wrap, the offset has a caret position on each line, and a step visits both.
    pub(crate) fn position_after_character_step(
        &self,
        text: NodeSlotId,
        offset: usize,
        downstream: bool,
        forward: bool,
        grapheme_boundary: Option<usize>,
    ) -> Option<CaretPosition> {
        let lines = self.lines_of_text(text);
        let ends_one_line_and_starts_the_next = |offset: usize| {
            lines
                .windows(2)
                .any(|pair| pair[0].end_offset_with_trailing_whitespace == offset && pair[1].start_offset == offset)
        };
        if downstream != forward && ends_one_line_and_starts_the_next(offset) {
            return Some(CaretPosition::Text {
                text,
                offset,
                downstream: forward,
            });
        }
        let offset = grapheme_boundary?;
        let is_in_whitespace_hanging_at_line_end = lines
            .iter()
            .any(|line| line.end_offset < offset && offset <= line.end_offset_with_trailing_whitespace);
        let stays_on_earlier_line =
            forward && (is_in_whitespace_hanging_at_line_end || ends_one_line_and_starts_the_next(offset));
        Some(CaretPosition::Text {
            text,
            offset,
            downstream: !stays_on_earlier_line,
        })
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
