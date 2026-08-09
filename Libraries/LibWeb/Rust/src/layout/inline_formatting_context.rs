/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

const ELLIPSIS_CODE_POINT: u32 = 0x2026;

pub(crate) trait EllipsisFontProvider {
    fn font_glyph_width(&self, font: *const c_void, code_point: u32) -> f32;
    fn font_glyph_id(&self, font: *const c_void, code_point: u32) -> u32;
}

pub(crate) fn apply(line_boxes: &mut [LineBoxData], provider: &impl EllipsisFontProvider) {
    for line in line_boxes {
        if !matches!(line.original_available_inline_size, AvailableSize::Definite(_)) {
            continue;
        }
        let available_inline_size = line.original_available_inline_size.to_px_or_zero();
        if line.inline_length <= available_inline_size || line.fragments.is_empty() {
            continue;
        }

        let mut line_has_visible_content = false;
        for index in 0..line.fragments.len() {
            let fragment_start = line.fragments[index].inline_offset;
            let fragment_end = fragment_start + line.fragments[index].inline_length;
            if fragment_end <= available_inline_size {
                line_has_visible_content = true;
                continue;
            }
            let Some(glyph_data) = &line.fragments[index].glyphs else {
                continue;
            };
            let font = glyph_data.font;
            let ellipsis_inline_size = provider.font_glyph_width(font, ELLIPSIS_CODE_POINT);
            let available_in_fragment = (available_inline_size - fragment_start).raw_value() as f32 / 64.0;
            let max_text_inline_size = available_in_fragment - ellipsis_inline_size;

            let glyphs = &line.fragments[index].glyphs.as_ref().unwrap().glyphs;
            let mut keep_count = 0usize;
            let mut last_kept_end = 0.0f32;
            let mut glyph_block_offset = 0.0f32;
            for glyph in glyphs {
                let glyph_end = glyph.x + glyph.glyph_width;
                if glyph_end > max_text_inline_size && (keep_count > 0 || line_has_visible_content) {
                    break;
                }
                keep_count += 1;
                last_kept_end = glyph_end;
                glyph_block_offset = glyph.y;
            }

            let glyph_data = line.fragments[index].glyphs.as_mut().unwrap();
            glyph_data.glyphs.truncate(keep_count);
            glyph_data.glyphs.push(FfiDrawGlyph {
                x: last_kept_end,
                y: glyph_block_offset,
                length_in_code_units: 1,
                glyph_width: ellipsis_inline_size,
                glyph_id: provider.font_glyph_id(font, ELLIPSIS_CODE_POINT),
                should_paint: true,
            });
            line.fragments[index].inline_length =
                CssPixels::nearest_value_for_f32(last_kept_end + ellipsis_inline_size);
            for later in &mut line.fragments[index + 1..] {
                later.is_fully_truncated = true;
            }
            line.inline_length = available_inline_size;
            line.clamp_static_position_markers_to_inline_length();
            break;
        }
    }
}

pub(crate) fn apply_to_fragments(text_justify: u8, line: &mut LineBoxData, is_last_line: bool) {
    if text_justify == text_justify::NONE || is_last_line || line.has_forced_break {
        return;
    }
    assert!(matches!(
        text_justify,
        text_justify::AUTO | text_justify::INTER_WORD | text_justify::INTER_CHARACTER
    ));

    let excess_inline_space = line.original_available_inline_size.to_px_or_zero() - line.inline_length;
    let mut excess_inline_space_including_whitespace = excess_inline_space;
    let mut whitespace_count = 0usize;
    for fragment in &line.fragments {
        if fragment.is_justifiable_whitespace() {
            whitespace_count += 1;
            excess_inline_space_including_whitespace += fragment.inline_length;
        }
    }
    let justified_space_inline_size = if whitespace_count > 0 {
        excess_inline_space_including_whitespace / whitespace_count
    } else {
        CssPixels::default()
    };

    let mut running_diff = CssPixels::default();
    for fragment in &mut line.fragments {
        fragment.inline_offset += running_diff;
        if fragment.is_justifiable_whitespace() && fragment.inline_length != justified_space_inline_size {
            let diff = justified_space_inline_size - fragment.inline_length;
            running_diff += diff;
            for marker in &mut line.static_position_markers {
                // This intentionally compares against the fragment's already
                // shifted offset, matching the C++ ordering.
                if marker.inline_offset > fragment.inline_offset {
                    marker.inline_offset += diff;
                }
            }
            fragment.inline_length = justified_space_inline_size;
        }
    }
}

pub(crate) const EDGE_TOP: u8 = 1 << 0;
pub(crate) const EDGE_RIGHT: u8 = 1 << 1;
pub(crate) const EDGE_BOTTOM: u8 = 1 << 2;
pub(crate) const EDGE_LEFT: u8 = 1 << 3;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct InlineCssPixelRect {
    pub(crate) x: CssPixels,
    pub(crate) y: CssPixels,
    pub(crate) width: CssPixels,
    pub(crate) height: CssPixels,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct InlineBoxPieceData {
    pub(crate) node: Node,
    pub(crate) first_fragment_index: u32,
    pub(crate) fragment_count: u32,
    pub(crate) border_box_rect: InlineCssPixelRect,
    pub(crate) relpos_delta: FfiCssPixelPoint,
    pub(crate) present_edges: u8,
    pub(crate) is_geometry_only_placeholder: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct StagedPiece {
    pub(crate) piece: InlineBoxPieceData,
    pub(crate) line_index: u32,
    pub(crate) depth: u32,
    pub(crate) discovery_index: usize,
}

pub(crate) fn sort_for_emission(pieces: &mut [StagedPiece]) {
    pieces.sort_by_key(|piece| (piece.line_index, piece.depth, piece.discovery_index));
}

#[derive(Debug)]
struct PerLine {
    line_index: usize,
    contributions_inline_range: Option<(CssPixels, CssPixels)>,
    first_direct_fragment_block_start: Option<CssPixels>,
    max_direct_fragment_block_length: CssPixels,
    fallback_block_start_from_contributions: Option<CssPixels>,
    interrupting_block: Option<((CssPixels, CssPixels), CandidateLineCorners)>,
    first_fragment_index: Option<u32>,
    fragment_count: u32,
}

impl PerLine {
    fn new(line_index: usize) -> Self {
        Self {
            line_index,
            contributions_inline_range: None,
            first_direct_fragment_block_start: None,
            max_direct_fragment_block_length: CssPixels::default(),
            fallback_block_start_from_contributions: None,
            interrupting_block: None,
            first_fragment_index: None,
            fragment_count: 0,
        }
    }
}

#[derive(Debug)]
struct PerNode {
    node: Node,
    parent_index: Option<usize>,
    depth: u32,
    lines: Vec<PerLine>,
    first_line_with_content: Option<usize>,
    last_line_with_content: Option<usize>,
}

fn ensure_line(per_node: &mut PerNode, line_index: usize) -> &mut PerLine {
    let insertion = per_node.lines.partition_point(|line| line.line_index <= line_index);
    if insertion != 0 && per_node.lines[insertion - 1].line_index == line_index {
        return &mut per_node.lines[insertion - 1];
    }
    per_node.lines.insert(insertion, PerLine::new(line_index));
    &mut per_node.lines[insertion]
}

fn note_contribution(line: &mut PerLine, inline_start: CssPixels, inline_end: CssPixels, block_start: CssPixels) {
    if line
        .contributions_inline_range
        .is_none_or(|(current_start, _)| inline_start < current_start)
    {
        line.fallback_block_start_from_contributions = Some(block_start);
    }
    line.contributions_inline_range = Some(
        line.contributions_inline_range
            .map_or((inline_start, inline_end), |(current_start, current_end)| {
                (current_start.min(inline_start), current_end.max(inline_end))
            }),
    );
}

fn nesting_depth(context: &InlineFormattingContext, node: Node) -> u32 {
    let mut depth = 1;
    let mut ancestor = context.nearest_fragmented_inline_ancestor(node);
    while !ancestor.is_invalid() {
        depth += 1;
        ancestor = context.nearest_fragmented_inline_ancestor(ancestor);
    }
    depth
}

fn edge_bits(horizontal: bool, low: bool, high: bool) -> u8 {
    let mut edges = if horizontal {
        EDGE_TOP | EDGE_BOTTOM
    } else {
        EDGE_LEFT | EDGE_RIGHT
    };
    if low {
        edges |= if horizontal { EDGE_LEFT } else { EDGE_TOP };
    }
    if high {
        edges |= if horizontal { EDGE_RIGHT } else { EDGE_BOTTOM };
    }
    edges
}

pub(crate) struct InlineContainingBlockRectCandidate {
    pub(crate) inline_containing_block: Node,
    pub(crate) rect: PhysicalRect,
}

#[derive(Clone, Copy, Debug)]
struct CandidateLineCorners {
    inline_start: CssPixels,
    inline_end: CssPixels,
    block_start: CssPixels,
    block_end: CssPixels,
}

#[derive(Default)]
struct FirstAndLastContentLineCorners {
    first: Option<CandidateLineCorners>,
    last: Option<CandidateLineCorners>,
}

pub(crate) fn compute(
    context: &InlineFormattingContext,
) -> (Vec<InlineBoxPieceData>, Vec<InlineContainingBlockRectCandidate>) {
    let horizontal = context.style(context.containing_block).writing_mode() == writing_mode::HORIZONTAL_TB;
    let collect_inline_containing_block_rects = context.state.has_inline_containing_blocks();
    let container_inline_axis_is_reverse = collect_inline_containing_block_rects
        && context.facts(context.containing_block).inline_axis_is_reverse();
    let mut inline_containing_block_rect_candidates = Vec::<InlineContainingBlockRectCandidate>::new();
    let mut per_nodes = Vec::<PerNode>::new();
    let mut node_to_index = HashMap::<Node, usize>::new();

    let mut committed_fragment_index = 0u32;
    for (line_index, line) in context.line_data().line_boxes.iter().enumerate() {
        for fragment in &line.fragments {
            if fragment.is_fully_truncated {
                continue;
            }
            let fragment_index = committed_fragment_index;
            committed_fragment_index += 1;
            let interrupting = line.has_block_level_box;
            debug_assert!(
                !interrupting || context.style(fragment.style_source).display().is_block_outside(),
                "an interrupting line's fragment must be a block-level box"
            );
            let position = fragment.offset();
            let size = fragment.size();
            let mut inline_start = if horizontal { position.0 } else { position.1 };
            let mut inline_end = inline_start + if horizontal { size.0 } else { size.1 };
            let block_start = if horizontal { position.1 } else { position.0 };
            let block_length = if horizontal { size.1 } else { size.0 };

            if !interrupting && fragment.is_atomic_inline {
                let used = context.used(fragment.layout_node);
                if horizontal {
                    inline_start -= used.margin_left.get() + used.border_box_left(false);
                    inline_end += used.margin_right.get() + used.border_box_right(false);
                } else {
                    inline_start -= used.margin_top.get() + used.border_box_top(false);
                    inline_end += used.margin_bottom.get() + used.border_box_bottom(false);
                }
            }

            let interrupting_block = if interrupting {
                let block_used = context.used(fragment.layout_node);
                let collapsed = block_used.uses_collapsing_borders_model.get();
                let (border_box_inline_low, border_box_block_low, border_box_inline_size, border_box_block_size) =
                    if horizontal {
                        (
                            block_used.border_box_left(collapsed),
                            block_used.border_box_top(collapsed),
                            block_used.border_box_inline_size(collapsed),
                            block_used.border_box_block_size(collapsed),
                        )
                    } else {
                        (
                            block_used.border_box_top(collapsed),
                            block_used.border_box_left(collapsed),
                            block_used.border_box_block_size(collapsed),
                            block_used.border_box_inline_size(collapsed),
                        )
                    };
                let extent_inline_start = inline_start - border_box_inline_low;
                let extent_block_start = block_start - border_box_block_low;
                Some((
                    position,
                    CandidateLineCorners {
                        inline_start: extent_inline_start,
                        inline_end: extent_inline_start + border_box_inline_size,
                        block_start: extent_block_start,
                        block_end: extent_block_start + border_box_block_size,
                    },
                ))
            } else {
                None
            };

            let mut direct = true;
            let mut previous: Option<usize> = None;
            let mut ancestor = context.nearest_fragmented_inline_ancestor(fragment.layout_node);
            while !ancestor.is_invalid() {
                let node_index = if let Some(index) = node_to_index.get(&ancestor) {
                    *index
                } else {
                    let index = per_nodes.len();
                    per_nodes.push(PerNode {
                        node: ancestor,
                        parent_index: None,
                        depth: nesting_depth(context, ancestor),
                        lines: Vec::new(),
                        first_line_with_content: None,
                        last_line_with_content: None,
                    });
                    node_to_index.insert(ancestor, index);
                    index
                };
                if let Some(previous) = previous {
                    per_nodes[previous].parent_index = Some(node_index);
                }
                previous = Some(node_index);
                let per_node = &mut per_nodes[node_index];
                let line = ensure_line(per_node, line_index);
                let first = *line.first_fragment_index.get_or_insert(fragment_index);
                line.fragment_count = fragment_index + 1 - first;
                if let Some(interrupting_block) = interrupting_block {
                    line.interrupting_block.get_or_insert(interrupting_block);
                    ancestor = context.nearest_fragmented_inline_ancestor(ancestor);
                    continue;
                }
                if direct {
                    note_contribution(line, inline_start, inline_end, block_start);
                    line.first_direct_fragment_block_start.get_or_insert(block_start);
                    line.max_direct_fragment_block_length = line.max_direct_fragment_block_length.max(block_length);
                } else if line.fallback_block_start_from_contributions.is_none() {
                    line.fallback_block_start_from_contributions = Some(block_start);
                }
                per_node.first_line_with_content.get_or_insert(line_index);
                per_node.last_line_with_content = Some(line_index);
                direct = false;
                ancestor = context.nearest_fragmented_inline_ancestor(ancestor);
            }
        }
    }

    let without_fragments: Vec<_> = context
        .fragmented_inlines_in_pre_order
        .iter()
        .copied()
        .filter(|node| context.facts(*node).has_dom_node() && !node_to_index.contains_key(node))
        .collect();

    let mut staged = Vec::<StagedPiece>::new();
    let mut deepest_first: Vec<_> = (0..per_nodes.len()).collect();
    deepest_first.sort_by(|left, right| {
        per_nodes[*right]
            .depth
            .cmp(&per_nodes[*left].depth)
            .then_with(|| left.cmp(right))
    });

    for node_index in deepest_first {
        let (node, parent_index, depth, first_line_with_content, last_line_with_content, lines) = {
            let per_node = &mut per_nodes[node_index];
            (
                per_node.node,
                per_node.parent_index,
                per_node.depth,
                per_node.first_line_with_content,
                per_node.last_line_with_content,
                std::mem::take(&mut per_node.lines),
            )
        };
        let used = context.used(node);
        let reversed = context.facts(node).inline_axis_is_reverse();
        let (
            border_padding_low,
            border_padding_high,
            border_padding_block_low,
            border_padding_block_high,
            margin_low,
            margin_high,
        ) = if horizontal {
            (
                used.border_box_left(false),
                used.border_box_right(false),
                used.border_box_top(false),
                used.border_box_bottom(false),
                used.margin_left.get(),
                used.margin_right.get(),
            )
        } else {
            (
                used.border_box_top(false),
                used.border_box_bottom(false),
                used.border_box_left(false),
                used.border_box_right(false),
                used.margin_top.get(),
                used.margin_bottom.get(),
            )
        };

        let node_is_inline_containing_block =
            collect_inline_containing_block_rects && context.state.is_inline_containing_block(node);
        let mut corners = FirstAndLastContentLineCorners::default();

        for line in lines {
            let Some((contributions_inline_start, contributions_inline_end)) = line.contributions_inline_range else {
                if let Some((position, extent)) = line.interrupting_block {
                    if node_is_inline_containing_block {
                        corners.first.get_or_insert(extent);
                        corners.last = Some(extent);
                    }
                    staged.push(StagedPiece {
                        piece: InlineBoxPieceData {
                            node,
                            first_fragment_index: line.first_fragment_index.unwrap_or(0),
                            fragment_count: line.fragment_count,
                            border_box_rect: InlineCssPixelRect {
                                x: position.0,
                                y: position.1,
                                ..Default::default()
                            },
                            relpos_delta: FfiCssPixelPoint::default(),
                            present_edges: edge_bits(horizontal, true, true),
                            is_geometry_only_placeholder: true,
                        },
                        line_index: line.line_index as u32,
                        depth,
                        discovery_index: node_index,
                    });
                }
                continue;
            };
            let first = first_line_with_content == Some(line.line_index);
            let last = last_line_with_content == Some(line.line_index);
            let has_low_edge = if reversed { last } else { first };
            let has_high_edge = if reversed { first } else { last };
            let content_block_start = line
                .first_direct_fragment_block_start
                .or(line.fallback_block_start_from_contributions)
                .unwrap_or_default();
            let content_block_length = if line.first_direct_fragment_block_start.is_some() {
                line.max_direct_fragment_block_length
            } else {
                context.style(node).line_height()
            };
            let border_inline_start = contributions_inline_start
                - if has_low_edge {
                    border_padding_low
                } else {
                    CssPixels::default()
                };
            let border_inline_end = contributions_inline_end
                + if has_high_edge {
                    border_padding_high
                } else {
                    CssPixels::default()
                };
            let border_block_start = content_block_start - border_padding_block_low;
            let border_block_length = content_block_length + border_padding_block_low + border_padding_block_high;
            let rect = if horizontal {
                InlineCssPixelRect {
                    x: border_inline_start,
                    y: border_block_start,
                    width: border_inline_end - border_inline_start,
                    height: border_block_length,
                }
            } else {
                InlineCssPixelRect {
                    x: border_block_start,
                    y: border_inline_start,
                    width: border_block_length,
                    height: border_inline_end - border_inline_start,
                }
            };
            staged.push(StagedPiece {
                piece: InlineBoxPieceData {
                    node,
                    first_fragment_index: line.first_fragment_index.unwrap_or(0),
                    fragment_count: line.fragment_count,
                    border_box_rect: rect,
                    relpos_delta: FfiCssPixelPoint::default(),
                    present_edges: edge_bits(horizontal, has_low_edge, has_high_edge),
                    is_geometry_only_placeholder: false,
                },
                line_index: line.line_index as u32,
                depth,
                discovery_index: node_index,
            });
            if node_is_inline_containing_block {
                let source = CandidateLineCorners {
                    inline_start: border_inline_start,
                    inline_end: border_inline_end,
                    block_start: border_block_start,
                    block_end: border_block_start + border_block_length,
                };
                corners.first.get_or_insert(source);
                corners.last = Some(source);
            }
            if let Some(parent_index) = parent_index {
                let parent_line = ensure_line(&mut per_nodes[parent_index], line.line_index);
                note_contribution(
                    parent_line,
                    border_inline_start - if has_low_edge { margin_low } else { CssPixels::default() },
                    border_inline_end
                        + if has_high_edge {
                            margin_high
                        } else {
                            CssPixels::default()
                        },
                    content_block_start,
                );
            }
        }

        if node_is_inline_containing_block
            && let Some(rect) = padding_box_rect_spanning_first_and_last_content_lines(
                &corners,
                used,
                horizontal,
                reversed,
                container_inline_axis_is_reverse,
            )
        {
            inline_containing_block_rect_candidates.push(InlineContainingBlockRectCandidate {
                inline_containing_block: node,
                rect,
            });
        }
    }

    for (index, node) in without_fragments.into_iter().enumerate() {
        context.used(node);
        let line_height = context.style(node).line_height();
        let placeholder_rect = if horizontal {
            InlineCssPixelRect {
                height: line_height,
                ..Default::default()
            }
        } else {
            InlineCssPixelRect {
                width: line_height,
                ..Default::default()
            }
        };
        if collect_inline_containing_block_rects && context.state.is_inline_containing_block(node) {
            inline_containing_block_rect_candidates.push(InlineContainingBlockRectCandidate {
                inline_containing_block: node,
                rect: PhysicalRect {
                    x: placeholder_rect.x,
                    y: placeholder_rect.y,
                    width: placeholder_rect.width,
                    height: placeholder_rect.height,
                },
            });
        }
        staged.push(StagedPiece {
            piece: InlineBoxPieceData {
                node,
                first_fragment_index: 0,
                fragment_count: 0,
                border_box_rect: placeholder_rect,
                relpos_delta: FfiCssPixelPoint::default(),
                present_edges: edge_bits(horizontal, true, true),
                is_geometry_only_placeholder: true,
            },
            line_index: 0,
            depth: nesting_depth(context, node),
            discovery_index: per_nodes.len() + index,
        });
    }
    sort_for_emission(&mut staged);
    let pieces = staged.into_iter().map(|staged| staged.piece).collect();
    (pieces, inline_containing_block_rect_candidates)
}

fn padding_box_rect_spanning_first_and_last_content_lines(
    corners: &FirstAndLastContentLineCorners,
    used: &UsedValues,
    horizontal: bool,
    inline_axis_is_reverse: bool,
    container_inline_axis_is_reverse: bool,
) -> Option<PhysicalRect> {
    let first = corners.first?;
    let last = corners.last?;

    let (border_inline_low, border_inline_high, border_block_low, border_block_high) = if horizontal {
        (
            used.border_left.get(),
            used.border_right.get(),
            used.border_top.get(),
            used.border_bottom.get(),
        )
    } else {
        (
            used.border_top.get(),
            used.border_bottom.get(),
            used.border_left.get(),
            used.border_right.get(),
        )
    };
    let direction_matches = inline_axis_is_reverse == container_inline_axis_is_reverse;
    let (contracted_inline_low, contracted_inline_high) = if direction_matches {
        (border_inline_low, border_inline_high)
    } else {
        Default::default()
    };

    let block_start = first.block_start + border_block_low;
    let block_size = (last.block_end - border_block_high - block_start).max(CssPixels::default());
    let (inline_low, inline_size) = if !container_inline_axis_is_reverse {
        let start = first.inline_start + contracted_inline_low;
        let end = last.inline_end - contracted_inline_high;
        (start, (end - start).max(CssPixels::default()))
    } else {
        let start = first.inline_end - contracted_inline_high;
        let end = last.inline_start + contracted_inline_low;
        let size = (start - end).max(CssPixels::default());
        (start - size, size)
    };
    Some(if horizontal {
        PhysicalRect {
            x: inline_low,
            y: block_start,
            width: inline_size,
            height: block_size,
        }
    } else {
        PhysicalRect {
            x: block_start,
            y: inline_low,
            width: block_size,
            height: inline_size,
        }
    })
}
pub(crate) struct InlineFormattingContext<'context, 'pass> {
    pub(crate) run: &'context FormattingContextRun<'pass>,
    pub(crate) state: &'pass LayoutState,
    pub(crate) containing_block: Node,
    pub(crate) layout_mode: LayoutMode,
    pub(crate) input: LayoutInput,
    pub(crate) callbacks: FfiLayoutFcCallbacks,
    parent: &'context BlockFormattingContext<'pass>,
    pub(crate) containing_used_values: &'pass UsedValues,
    pub(crate) line_data: &'pass RefCell<LineData>,
    pub(crate) fragmented_inlines_in_pre_order: Vec<Node>,
    pub(crate) automatic_content_inline_size: CssPixels,
    pub(crate) automatic_content_block_size: CssPixels,
    block_axis_float_clearance: Cell<CssPixels>,
}

impl<'context, 'pass> InlineFormattingContext<'context, 'pass> {
    pub(crate) fn new_with_rust_parent(
        run: &'context FormattingContextRun<'pass>,
        state: &'pass LayoutState,
        containing_block: Node,
        layout_mode: LayoutMode,
        input: LayoutInput,
        callbacks: FfiLayoutFcCallbacks,
        parent: &'context BlockFormattingContext<'pass>,
    ) -> Self {
        let containing_used_values = state.used_values(&callbacks, containing_block);
        let line_data = state.line_data_cell(callbacks.slot_index(containing_block));
        Self {
            run,
            state,
            containing_block,
            layout_mode,
            input,
            callbacks,
            parent,
            containing_used_values,
            line_data,
            fragmented_inlines_in_pre_order: Vec::new(),
            automatic_content_inline_size: CssPixels::default(),
            automatic_content_block_size: CssPixels::default(),
            block_axis_float_clearance: Cell::new(CssPixels::default()),
        }
    }

    pub(crate) fn block_axis_float_clearance(&self) -> CssPixels {
        self.block_axis_float_clearance.get()
    }

    pub(crate) fn set_block_axis_float_clearance(&self, clearance: CssPixels) {
        self.block_axis_float_clearance.set(clearance);
    }

    pub(crate) fn style(&self, node: Node) -> StyleValues<'pass> {
        self.state.style_facts(&self.callbacks, node)
    }

    pub(crate) fn facts(&self, node: Node) -> NodeFacts<'_> {
        self.state.node_facts(&self.callbacks, node)
    }

    pub(crate) fn style_source(&self, node: Node) -> Node {
        if self.facts(node).is_text_node() {
            self.parent_node(node)
        } else {
            node
        }
    }

    pub(crate) fn line_data(&self) -> Ref<'_, LineData> {
        self.line_data.borrow()
    }

    pub(crate) fn line_data_mut(&self) -> RefMut<'_, LineData> {
        self.line_data.borrow_mut()
    }

    pub(crate) fn containing_used(&self) -> &'pass UsedValues {
        self.containing_used_values
    }

    pub(crate) fn used(&self, node: Node) -> &'pass UsedValues {
        self.state.used_values(&self.callbacks, node)
    }

    pub(crate) fn create_used_values(
        &self,
        node: Node,
        constraints: ContainingBlockConstraints,
    ) -> &'pass UsedValues {
        self.state.create_used_values(&self.callbacks, node, constraints)
    }

    pub(crate) fn parent_node(&self, node: Node) -> Node {
        self.callbacks.parent(node)
    }

    pub(crate) fn first_child(&self, node: Node) -> Node {
        self.callbacks.first_child(node)
    }

    pub(crate) fn next_sibling(&self, node: Node) -> Node {
        self.callbacks.next_sibling(node)
    }

    pub(crate) fn nearest_fragmented_inline_ancestor(&self, node: Node) -> Node {
        let mut ancestor = self.parent_node(node);
        while !ancestor.is_invalid() {
            // The containing block bounds this context's subtree even when it
            // is itself inline-outside with flow inside (an inline list-item
            // root): ancestors beyond it belong to the enclosing run.
            if ancestor == self.containing_block {
                break;
            }
            let display = self.style(ancestor).display();
            if !display.is_inline_outside() || !display.is_flow_inside() {
                break;
            }
            let facts = self.facts(ancestor);
            if facts.is_fragmented_inline() && !facts.is_floating_or_absolutely_positioned() {
                return ancestor;
            }
            ancestor = self.parent_node(ancestor);
        }
        NodeSlotId::INVALID
    }

    pub(crate) fn text_may_require_bidi_processing(&self, node: Node) -> bool {
        self.callbacks.text_content(node).may_require_bidi_processing
    }

    pub(crate) fn compute_inset(&self, node: Node) {
        let used = self.containing_used();
        crate::layout::compute_inset_native(
            self.state,
            self.callbacks,
            node,
            used.content_inline_size.get(),
            used.content_block_size.get(),
            self.run.box_,
            self.run.treat_block_axis_percentage_insets_as_auto_beyond_root,
        );
    }

    pub(crate) fn parent_commit_pending_margin_before_inline_content(&self) -> CssPixels {
        self.parent.commit_pending_margin_before_inline_content()
    }

    pub(crate) fn intrusion_by_floats_into_containing_block(
        &self,
        block_start: CssPixels,
        block_end: CssPixels,
    ) -> SpaceUsedByFloats {
        let content_box_position_in_bfc_root = self
            .input
            .content_box_position_in_bfc_root
            .expect("float intrusion requires the containing block position in the BFC root");
        let adjustment = self
            .parent
            .block_offset_adjustment_from_pending_ancestor_block_start_margins(self.containing_block);
        let rect = FfiCssPixelRect {
            x: content_box_position_in_bfc_root.x,
            y: content_box_position_in_bfc_root.y + adjustment,
            width: self.containing_used().content_inline_size.get(),
            height: self.containing_used().content_block_size.get(),
        };
        self.parent.intrusion_by_floats_into_rect(rect, block_start, block_end)
    }

    pub(crate) fn leftmost_inline_offset_at(&self, block_offset: CssPixels, line_block_size: CssPixels) -> CssPixels {
        self.intrusion_by_floats_into_containing_block(block_offset, block_offset + line_block_size)
            .left
    }

    pub(crate) fn available_space_for_line(
        &self,
        block_offset: CssPixels,
        line_block_size: CssPixels,
    ) -> crate::layout::AvailableSize {
        if !matches!(self.input.available_space.inline_size, AvailableSize::Definite(_)) {
            return self.input.available_space.inline_size;
        }
        let intrusions = self.intrusion_by_floats_into_containing_block(block_offset, block_offset + line_block_size);
        crate::layout::AvailableSize::definite(
            self.input.available_space.inline_size.to_px_or_zero() - intrusions.left - intrusions.right,
        )
    }

    pub(crate) fn any_floats_intrude_in_block_range(&self, block_start: CssPixels, block_end: CssPixels) -> bool {
        let intrusions = self.intrusion_by_floats_into_containing_block(block_start, block_end);
        intrusions.left > CssPixels::default() || intrusions.right > CssPixels::default()
    }

    pub(crate) fn can_fit_new_line_at_block_offset(&self, block_offset: CssPixels, line_block_size: CssPixels) -> bool {
        if !matches!(self.input.available_space.inline_size, AvailableSize::Definite(_)) {
            return true;
        }
        self.available_space_for_line(block_offset, line_block_size)
            .to_px_or_zero()
            > CssPixels::default()
    }

    pub(crate) fn next_float_band_block_start_after(&self, block_offset: CssPixels) -> Option<CssPixels> {
        let content_box_position_in_bfc_root = self
            .input
            .content_box_position_in_bfc_root
            .expect("float-band lookup requires the containing block position in the BFC root");
        let adjustment = self
            .parent
            .block_offset_adjustment_from_pending_ancestor_block_start_margins(self.containing_block);
        let containing_block_offset_in_root = content_box_position_in_bfc_root.y + adjustment;
        let next = self
            .parent
            .next_float_band_block_start_after(containing_block_offset_in_root + block_offset);
        next.map(|next| next - containing_block_offset_in_root)
    }

    fn layout_inside(&mut self, node: Node, available_space: AvailableSpace) -> DerivedBaselines {
        let input = LayoutInput::new(
            available_space,
            self.input.containing_block_constraints,
            ParticipationInParentFormattingContext::AtomicInline,
        );
        let content_baselines_from_cells = |used: &UsedValues| DerivedBaselines {
            first: used.has_first_baseline.get().then(|| used.first_baseline.get()),
            last: used.has_last_baseline.get().then(|| used.last_baseline.get()),
        };
        match crate::layout::layout_inside_child(self.run, Some(self.parent), None, node, self.layout_mode, input, false)
        {
            crate::layout::ChildLayoutOutcome::Created(result) => result.baselines,
            crate::layout::ChildLayoutOutcome::ReenterCurrent => {
                self.parent.run(self.run, input);
                content_baselines_from_cells(self.used(node))
            }
            crate::layout::ChildLayoutOutcome::Skipped => content_baselines_from_cells(self.used(node)),
        }
    }

    pub(crate) fn dimension_box_on_line(&mut self, node: Node) -> DerivedBaselines {
        let available_space = self.input.available_space;
        let facts = self.facts(node);
        // Any fragmented inline box should have generated line box fragments already.
        if facts.is_fragmented_inline() {
            // SAFETY: The callback table and layout node remain live for this
            // synchronous formatting-context run.
            unsafe {
                (self.callbacks.report_unexpected_fragmented_inline)(
                    self.callbacks.context,
                    self.callbacks.shell(node),
                );
            }
            return DerivedBaselines::default();
        }

        let content_baselines = self.layout_inside(node, available_space);
        debug_assert!(
            self.used(node).has_definite_inline_size.get()
                || self.used(node).inline_size_constraint.get() != SizeConstraint::None,
            "atomic inline-level run left its root's inline size unresolved"
        );
        content_baselines
    }

    fn clear_floating_boxes(&self, node: Node) -> bool {
        self.parent.clear_floating_boxes(
            node,
            Some(self),
            self.input
                .content_box_position_in_bfc_root
                .expect("clearing floats requires the containing block position in the BFC root"),
        )
    }

    fn reset_parent_margin_state(&self) {
        self.parent.reset_margin_state();
    }

    fn text_overflow_applies(&self) -> bool {
        let mut block = self.containing_block;
        if self.facts(block).is_anonymous() {
            block = self.callbacks.non_anonymous_containing_block(block);
        }
        if block.is_invalid() {
            return false;
        }
        let style = self.style(block);
        style.text_overflow() == text_overflow::ELLIPSIS && style.overflow_x() != overflow::VISIBLE
    }

    pub(crate) fn generate_line_boxes(&mut self) {
        self.line_data_mut().line_boxes.clear();
        self.line_data_mut().inline_box_pieces.clear();
        let mut iterator = InlineLevelIterator::new(self);
        self.fragmented_inlines_in_pre_order = iterator.take_visited_fragmented_inlines();
        let mut line_builder = LineBuilder::new(self);

        let mut leading_margin = CssPixels::default();
        let mut leading_border = CssPixels::default();
        let mut leading_padding = CssPixels::default();
        let mut absolute_boxes = Vec::new();

        while let Some(mut item) = iterator.next() {
            let line_starts_with_whitespace = self
                .line_data()
                .line_boxes
                .last()
                .is_none_or(|line| line.is_empty_or_ends_in_whitespace() || line.has_block_level_box);
            if item.is_collapsible_whitespace && line_starts_with_whitespace {
                if self.style(self.style_source(item.node)).text_wrap_mode() == text_wrap_mode::WRAP {
                    let next_inline_size = iterator.next_non_whitespace_sequence_inline_size(self);
                    if next_inline_size > CssPixels::default() {
                        line_builder.prepare_to_append_inline_content();
                        line_builder.break_if_needed(next_inline_size);
                    }
                }
                leading_margin += item.margin_start;
                leading_border += item.border_start;
                leading_padding += item.padding_start;
                continue;
            }

            item.margin_start += leading_margin;
            item.border_start += leading_border;
            item.padding_start += leading_padding;
            leading_margin = CssPixels::default();
            leading_border = CssPixels::default();
            leading_padding = CssPixels::default();

            match item.type_ {
                ItemType::ForcedBreak => {
                    line_builder.break_line(ForcedBreak::Yes, None);
                    if !item.node.is_invalid() && self.clear_floating_boxes(item.node) {
                        line_builder.did_introduce_clearance(self.block_axis_float_clearance.get());
                        self.reset_parent_margin_state();
                    }
                }
                ItemType::Element => {
                    line_builder.prepare_to_append_inline_content();
                    self.compute_inset(item.node);
                    if self.style(self.containing_block).text_wrap_mode() == text_wrap_mode::WRAP {
                        let mut minimum = item.border_box_inline_size();
                        if item.margin_start < CssPixels::default() {
                            minimum += item.margin_start;
                        }
                        if item.margin_end < CssPixels::default() {
                            minimum += item.margin_end;
                        }
                        line_builder.break_if_needed(minimum);
                    }
                    line_builder.append_box(
                        item.node,
                        item.border_start + item.padding_start,
                        item.padding_end + item.border_end,
                        item.margin_start,
                        item.margin_end,
                        item.content_baselines,
                    );
                }
                ItemType::BlockLevelBox => {
                    leading_margin += item.margin_start;
                    leading_border += item.border_start;
                    leading_padding += item.padding_start;
                    line_builder.finish_current_line_before_block_level_box();
                    self.parent.layout_interrupting_block_inside_inline_context(
                        self.run,
                        item.node,
                        self.containing_block,
                        self.input,
                        &mut line_builder,
                    );
                }
                ItemType::AbsolutelyPositionedElement => {
                    if !self.facts(item.node).is_box() {
                        continue;
                    }
                    let preceded = item.preceded_by_unattached_inline_start_edges
                        || item.margin_start != CssPixels::default()
                        || item.border_start != CssPixels::default()
                        || item.padding_start != CssPixels::default();
                    line_builder.append_static_position_marker(item.node, preceded);
                    absolute_boxes.push(item.node);
                }
                ItemType::FloatingElement => {
                    line_builder.commit_pending_margin_before_float();
                    self.create_used_values(item.node, self.input.containing_block_constraints);
                    self.clear_floating_boxes(item.node);
                    line_builder.set_unbreakable_run_inline_size_interrupted_by_float(
                        iterator.next_non_whitespace_sequence_inline_size(self),
                    );
                    self.parent.layout_floating_box(
                        self.run,
                        item.node,
                        self.input,
                        CssPixels::default(),
                        Some(&mut line_builder),
                    );
                }
                ItemType::Text => {
                    line_builder.prepare_to_append_inline_content();
                    if self.style(self.parent_node(item.node)).text_wrap_mode() == text_wrap_mode::WRAP {
                        let is_whitespace =
                            item.is_collapsible_whitespace || iterator.item_is_ascii_whitespace(self, &item);
                        let next_inline_size = if is_whitespace {
                            iterator.next_non_whitespace_sequence_inline_size(self)
                        } else {
                            CssPixels::default()
                        };
                        if is_whitespace
                            && next_inline_size > CssPixels::default()
                            && line_builder.break_if_needed(item.border_box_inline_size() + next_inline_size)
                        {
                            line_builder.set_trailing_whitespace_on_previous_line();
                            continue;
                        }
                        let line_is_empty = self.line_data().line_boxes.last().is_some_and(LineBoxData::is_empty);
                        if !is_whitespace && (item.can_break_before || line_is_empty) {
                            line_builder.break_if_needed(item.border_box_inline_size());
                        }
                    }
                    let line_height = self.style(self.parent_node(item.node)).line_height();
                    line_builder.append_text_chunk(
                        item.node,
                        item.offset_in_node,
                        item.length_in_node,
                        item.border_start + item.padding_start,
                        item.padding_end + item.border_end,
                        item.margin_start,
                        item.margin_end,
                        item.inline_size,
                        line_height,
                        item.glyphs.take().unwrap(),
                    );
                }
            }
        }

        let line_count = self.line_data().line_boxes.len();
        for line_index in 0..line_count {
            self.line_data_mut().line_boxes[line_index].trim_trailing_whitespace(self);
        }
        if self.text_overflow_applies() {
            apply(self.line_data_mut().line_boxes.as_mut_slice(), self);
        }
        let containing_style = self.style(self.containing_block);
        if containing_style.text_align() == text_align::JUSTIFY {
            let line_count = self.line_data().line_boxes.len();
            for index in 0..line_count {
                apply_to_fragments(
                    containing_style.text_justify(),
                    &mut self.line_data_mut().line_boxes[index],
                    index + 1 == line_count,
                );
            }
        }
        line_builder.update_last_line();

        for line_index in 0..self.line_data().line_boxes.len() {
            if self.line_data().line_boxes[line_index].has_block_level_box {
                continue;
            }
            let fragment_count = self.line_data().line_boxes[line_index].fragments.len();
            for fragment_index in 0..fragment_count {
                let fragment = &self.line_data().line_boxes[line_index].fragments[fragment_index];
                if !fragment.is_atomic_inline {
                    continue;
                }
                let (x, y) = fragment.offset();
                crate::layout::place_child(
                    self.state,
                    &self.callbacks,
                    fragment.layout_node,
                    FfiCssPixelPoint { x, y },
                );
            }
        }

        if self.layout_mode == LayoutMode::Normal {
            for box_ in absolute_boxes {
                let mut static_position = StaticPositionRect {
                    rect: Default::default(),
                    inline_alignment: StaticPositionAlignment::Start,
                    block_alignment: StaticPositionAlignment::Start,
                    alignment_derives_from_own_computed_values: false,
                };
                'lines: for line in &self.line_data().line_boxes {
                    for marker in &line.static_position_markers {
                        if marker.box_ != box_ {
                            continue;
                        }
                        if self.facts(box_).display_before_box_type_transformation_is_block_outside() {
                            let block_position = if marker.preceded_by_in_flow_content {
                                line.physical_vertical_end()
                            } else {
                                marker.offset().1
                            };
                            let inline_size = self.input.containing_block_constraints.inline_basis();
                            static_position.rect.offset.inline_offset = CssPixels::default();
                            static_position.rect.offset.block_offset = block_position;
                            static_position.rect.size.inline_size = inline_size;
                        } else {
                            let (x, y) = marker.offset();
                            static_position.rect.offset.inline_offset = x;
                            static_position.rect.offset.block_offset = y;
                        }
                        if containing_style.direction() == direction::RTL {
                            static_position.inline_alignment = StaticPositionAlignment::End;
                        }
                        break 'lines;
                    }
                }
                crate::layout::register_contained_abspos_child(self.state, &self.callbacks, box_, static_position);
            }
        }
        line_builder.remove_last_line_if_empty();
    }

    pub(crate) fn run(&mut self) {
        assert!(self.facts(self.containing_block).children_are_inline());
        self.generate_line_boxes();
        if self.layout_mode == LayoutMode::Normal && !self.state.is_measurement() {
            self.compute_inline_box_pieces();
            self.fold_inline_ancestor_relative_insets_into_line_data();
        }
        self.automatic_content_block_size = {
            let data = self.line_data();
            let lines = &data.line_boxes;
            if lines.iter().any(|line| line.has_block_level_box) {
                lines
                    .last()
                    .map_or(CssPixels::default(), LineBoxData::physical_vertical_end)
            } else {
                lines
                    .iter()
                    .fold(CssPixels::default(), |sum, line| sum + line.physical_vertical_extent())
            }
        };
        self.automatic_content_inline_size = self
            .parent
            .greatest_child_inline_size_including_floats(self.containing_block);
        let baselines = crate::layout::derive_baselines(self.state, &self.callbacks, self.containing_block, false);
        if self.containing_block == self.parent.root_box() {
            self.parent.record_derived_baselines_of_root_box(baselines);
        } else {
            crate::layout::store_derived_baselines(
                self.state.used_values(&self.callbacks, self.containing_block),
                baselines,
            );
        }
    }

    fn compute_inline_box_pieces(&mut self) {
        let (pieces, inline_containing_block_rect_candidates) = compute(self);
        self.line_data_mut().inline_box_pieces = pieces;
        for candidate in inline_containing_block_rect_candidates {
            let relative_inset_chain = self.state.accumulated_relative_insets_from_inline_ancestor_chain(
                &self.callbacks,
                candidate.inline_containing_block,
                self.containing_block,
            );
            let mut rect = candidate.rect;
            rect.x += relative_inset_chain.offset_x;
            rect.y += relative_inset_chain.offset_y;
            self.state
                .used_values_rare_data_for_node_mut(&self.callbacks, candidate.inline_containing_block)
                .inline_containing_block_first_last_rect = Some(rect);
        }
    }

    /// Runs after all line post-processing and after atomic-inline placement,
    /// so everything that reads static offsets has already read them.
    fn fold_inline_ancestor_relative_insets_into_line_data(&self) {
        let mut data = self.line_data_mut();
        // Every inline box that parents fragments gets a piece, so no pieces
        // means no fragment has an inline ancestor and every chain is empty.
        if data.inline_box_pieces.is_empty() {
            return;
        }
        let mut accumulated_relative_offset_by_chain_start = HashMap::<Node, FfiCssPixelPoint>::new();
        let mut accumulated_relative_offset_from = |first_ancestor: Node| -> FfiCssPixelPoint {
            *accumulated_relative_offset_by_chain_start
                .entry(first_ancestor)
                .or_insert_with(|| {
                    let chain = self.state.accumulated_relative_insets_from_inline_ancestor_chain(
                        &self.callbacks,
                        first_ancestor,
                        self.containing_block,
                    );
                    FfiCssPixelPoint {
                        x: chain.offset_x,
                        y: chain.offset_y,
                    }
                })
        };
        for line in &mut data.line_boxes {
            for fragment in &mut line.fragments {
                if fragment.is_fully_truncated {
                    continue;
                }
                fragment.relpos_delta = accumulated_relative_offset_from(self.callbacks.parent(fragment.layout_node));
            }
        }
        for piece in &mut data.inline_box_pieces {
            piece.relpos_delta = accumulated_relative_offset_from(piece.node);
        }
    }
}

impl LineBoxTextProvider for InlineFormattingContext<'_, '_> {
    fn font_glyph_width(&self, font: *const c_void, code_point: u32) -> f32 {
        font_glyph_width(font, code_point)
    }
}

impl EllipsisFontProvider for InlineFormattingContext<'_, '_> {
    fn font_glyph_width(&self, font: *const c_void, code_point: u32) -> f32 {
        <Self as LineBoxTextProvider>::font_glyph_width(self, font, code_point)
    }

    fn font_glyph_id(&self, font: *const c_void, code_point: u32) -> u32 {
        font_glyph_id(font, code_point)
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiCssPixelRect {
    pub x: CssPixels,
    pub y: CssPixels,
    pub width: CssPixels,
    pub height: CssPixels,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct SpaceUsedByFloats {
    pub left: CssPixels,
    pub right: CssPixels,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiLineRecord {
    pub rect: FfiCssPixelRect,
    pub committed_fragment_count: u32,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiCommittedFragment {
    pub layout_node: *mut c_void,
    pub offset: FfiCssPixelPoint,
    pub size: FfiCssPixelPoint,
    pub start: usize,
    pub length_in_code_units: usize,
    pub baseline: CssPixels,
    pub writing_mode: u8,
    pub has_trailing_whitespace: bool,
    pub has_glyph_run: bool,
    pub glyphs: *const FfiDrawGlyph,
    pub glyph_count: usize,
    pub glyph_font: *const c_void,
    pub glyph_text_type: u8,
    pub glyph_run_width: f32,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiInlineBoxPiece {
    pub node: *mut c_void,
    pub first_fragment_index: u32,
    pub fragment_count: u32,
    pub border_box_rect: FfiCssPixelRect,
    pub present_edges: u8,
    pub is_geometry_only_placeholder: bool,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiLineSinkCallbacks {
    pub context: *mut c_void,
    pub begin_line: unsafe extern "C" fn(*mut c_void, FfiLineRecord),
    pub emit_fragment: unsafe extern "C" fn(*mut c_void, FfiCommittedFragment),
    pub emit_inline_box_piece: unsafe extern "C" fn(*mut c_void, FfiInlineBoxPiece),
}

pub(crate) fn line_physical_horizontal_extent(line: &LineBoxData) -> CssPixels {
    if line.has_block_level_box || line.writing_mode == writing_mode::HORIZONTAL_TB {
        return line.inline_length;
    }
    let Some(first) = line.fragments.first() else {
        return CssPixels::default();
    };
    let mut left = first.offset().0;
    let mut right = left + first.physical_horizontal_extent();
    for fragment in &line.fragments[1..] {
        let fragment_left = fragment.offset().0;
        left = left.min(fragment_left);
        right = right.max(fragment_left + fragment.physical_horizontal_extent());
    }
    right - left
}

fn line_rect(line: &LineBoxData, content_inline_size: CssPixels) -> FfiCssPixelRect {
    let Some(first) = line.fragments.first() else {
        return FfiCssPixelRect {
            x: CssPixels::default(),
            y: line.physical_vertical_end() - line.physical_vertical_extent(),
            width: content_inline_size,
            height: line.physical_vertical_extent(),
        };
    };
    let (first_x, first_y) = first.offset();
    let (first_width, first_height) = first.size();
    let mut left = first_x;
    let mut top = first_y;
    let mut right = first_x + first_width;
    let mut bottom = first_y + first_height;
    let mut rect_is_empty = first_width <= CssPixels::default() || first_height <= CssPixels::default();
    for fragment in &line.fragments[1..] {
        let (x, y) = fragment.offset();
        let (width, height) = fragment.size();
        if rect_is_empty {
            left = x;
            top = y;
            right = x + width;
            bottom = y + height;
            rect_is_empty = width <= CssPixels::default() || height <= CssPixels::default();
            continue;
        }
        if width <= CssPixels::default() || height <= CssPixels::default() {
            continue;
        }
        left = left.min(x);
        top = top.min(y);
        right = right.max(x + width);
        bottom = bottom.max(y + height);
    }
    if first.writing_mode == writing_mode::HORIZONTAL_TB {
        top = line.physical_vertical_end() - line.physical_vertical_extent();
        bottom = top + line.physical_vertical_extent();
    }
    FfiCssPixelRect {
        x: left,
        y: top,
        width: right - left,
        height: bottom - top,
    }
}

pub(crate) fn push_line_data(
    state: &crate::layout::LayoutState,
    slot_index: u32,
    content_inline_size: CssPixels,
    callbacks: &FfiLayoutFcCallbacks,
    sink: FfiLineSinkCallbacks,
) -> bool {
    let Some(mut data) = state.line_data_mut_if_present(slot_index) else {
        return false;
    };
    for line in &mut data.line_boxes {
        let committed_fragment_count = line
            .fragments
            .iter()
            .filter(|fragment| !fragment.is_fully_truncated)
            .count() as u32;
        // SAFETY: Sink callbacks consume each POD record synchronously.
        unsafe {
            (sink.begin_line)(
                sink.context,
                FfiLineRecord {
                    rect: line_rect(line, content_inline_size),
                    committed_fragment_count,
                },
            );
        }
        for fragment in &mut line.fragments {
            if fragment.is_fully_truncated {
                continue;
            }
            let (x, y) = fragment.offset();
            let (x, y) = (x + fragment.relpos_delta.x, y + fragment.relpos_delta.y);
            let (width, height) = fragment.size();
            let glyphs = fragment
                .glyphs
                .as_mut()
                .map(|glyph_data| std::mem::take(&mut glyph_data.glyphs));
            let (glyph_pointer, glyph_count, glyph_font, glyph_text_type, glyph_run_width) =
                if let (Some(glyphs), Some(glyph_data)) = (glyphs.as_ref(), fragment.glyphs.as_ref()) {
                    (
                        glyphs.as_ptr(),
                        glyphs.len(),
                        glyph_data.font,
                        glyph_data.text_type,
                        glyph_data.width,
                    )
                } else {
                    (std::ptr::null(), 0, std::ptr::null(), 0, 0.0)
                };
            // SAFETY: Glyph storage stays live through this callback.
            unsafe {
                (sink.emit_fragment)(
                    sink.context,
                    FfiCommittedFragment {
                        layout_node: callbacks.shell(fragment.layout_node),
                        offset: FfiCssPixelPoint { x, y },
                        size: FfiCssPixelPoint { x: width, y: height },
                        start: fragment.start,
                        length_in_code_units: fragment.length_in_code_units,
                        baseline: fragment.baseline,
                        writing_mode: fragment.writing_mode,
                        has_trailing_whitespace: fragment.has_trailing_whitespace,
                        has_glyph_run: glyphs.is_some(),
                        glyphs: glyph_pointer,
                        glyph_count,
                        glyph_font,
                        glyph_text_type,
                        glyph_run_width,
                    },
                );
            }
        }
    }
    for piece in &data.inline_box_pieces {
        // SAFETY: The sink copies the POD piece synchronously.
        unsafe {
            (sink.emit_inline_box_piece)(
                sink.context,
                FfiInlineBoxPiece {
                    node: callbacks.shell(piece.node),
                    first_fragment_index: piece.first_fragment_index,
                    fragment_count: piece.fragment_count,
                    border_box_rect: FfiCssPixelRect {
                        x: piece.border_box_rect.x + piece.relpos_delta.x,
                        y: piece.border_box_rect.y + piece.relpos_delta.y,
                        width: piece.border_box_rect.width,
                        height: piece.border_box_rect.height,
                    },
                    present_edges: piece.present_edges,
                    is_geometry_only_placeholder: piece.is_geometry_only_placeholder,
                },
            );
        }
    }
    true
}
