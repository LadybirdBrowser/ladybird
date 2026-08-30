/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::layout::node_data::{NodeFlag, NodeSlotId};
use crate::layout::node_facts;
use crate::painting::display_list::commands::{DisplayListGlyph, FontResourceId};
use crate::painting::display_list::recorder::GlyphRunForRecording;
use crate::painting::paintable_data::{FragmentRecord, SELECTION_STATE_NONE, SELECTION_STATE_START_AND_END};
use crate::painting::record::PaintRecorder;
use crate::painting::text_fragment::{self, SelectionOffsets};
use libgfx_rust::{Color, FloatPoint, IntRect, Orientation};

#[derive(Clone, Copy)]
pub(crate) struct ShadowLayer {
    pub color: u32,
    pub offset_x: CssPixels,
    pub offset_y: CssPixels,
    pub blur_radius: CssPixels,
}

pub(crate) struct SpanTextDecoration {
    pub lines: [u8; 8],
    pub line_count: u32,
    pub style: u8,
    pub color: u32,
}

pub(crate) struct RenderSpan {
    pub fragment_index: u32,
    pub start_code_unit: usize,
    pub end_code_unit: usize,
    pub text_color: u32,
    pub background_color: u32,
    pub shadow_layers: Vec<ShadowLayer>,
    pub selection_offsets: Option<SelectionOffsets>,
    pub selection_text_decoration: Option<SpanTextDecoration>,
}

pub(crate) struct SelectionStyleAnswer {
    pub facts: crate::painting::host::FfiSelectionStyleFacts,
    pub shadows: Vec<ShadowLayer>,
}

fn selection_offsets_for_fragment(
    recorder: &mut PaintRecorder<'_>,
    fragment: &FragmentRecord,
) -> Option<SelectionOffsets> {
    let drop_degenerate = |offsets: Option<SelectionOffsets>| offsets.filter(|offsets| offsets.start != offsets.end);
    let control = recorder.text_control_selection(fragment.layout_node);
    if control.has_selection {
        return drop_degenerate(text_fragment::compute_selection_offsets(
            recorder.layout_arena,
            fragment,
            SELECTION_STATE_START_AND_END,
            control.start,
            control.end,
        ));
    }
    if fragment.selection_state == SELECTION_STATE_NONE {
        return None;
    }
    let range = recorder.paint_state.selection?;
    drop_degenerate(text_fragment::compute_selection_offsets(
        recorder.layout_arena,
        fragment,
        fragment.selection_state,
        range.start_offset,
        range.end_offset,
    ))
}

fn compute_render_spans(
    recorder: &mut PaintRecorder<'_>,
    block: NodeSlotId,
    owned_fragment_indices: &[u32],
) -> Vec<RenderSpan> {
    let arena = recorder.layout_arena;
    let layout_arena = recorder.layout_arena;
    let mut spans: Vec<RenderSpan> = Vec::new();
    for &fragment_index in owned_fragment_indices {
        let fragment = &layout_arena.paintable_side_data(block).fragments[fragment_index as usize];
        if text_fragment::is_block_level_box(arena, fragment) {
            continue;
        }
        if !arena
            .node_kind_if_live(fragment.layout_node)
            .is_some_and(node_facts::kind_is_text)
        {
            continue;
        }
        let Some(parent) = arena.node_parent_if_live(fragment.layout_node) else {
            continue;
        };
        let Some(parent_style) = arena.node_style_if_live(parent) else {
            continue;
        };
        let parent_retains_animated_content =
            arena.node_flags_if_live(parent) & NodeFlag::HasAnimatedOpacityOrTransform as u32 != 0;
        if parent_style.visibility() != crate::css::css_enums::visibility::VISIBLE
            || (parent_style.effects().opacity == 0.0 && !parent_retains_animated_content)
        {
            continue;
        }
        let inherited_text = parent_style.inherited_text();
        let text_color = inherited_text.webkit_text_fill_color;
        let base_shadows = || -> Vec<ShadowLayer> {
            inherited_text
                .text_shadow
                .as_slice()
                .iter()
                .map(|shadow| ShadowLayer {
                    color: shadow.color,
                    offset_x: CssPixels::from_raw(shadow.offset_x),
                    offset_y: CssPixels::from_raw(shadow.offset_y),
                    blur_radius: CssPixels::from_raw(shadow.blur_radius),
                })
                .collect()
        };

        let Some(selection_offsets) = selection_offsets_for_fragment(recorder, fragment) else {
            spans.push(RenderSpan {
                fragment_index,
                start_code_unit: 0,
                end_code_unit: fragment.length_in_code_units,
                text_color,
                background_color: 0,
                shadow_layers: base_shadows(),
                selection_offsets: None,
                selection_text_decoration: None,
            });
            continue;
        };

        let SelectionOffsets {
            start: selection_start,
            end: selection_end,
        } = selection_offsets;
        let answer = recorder.selection_style(fragment.layout_node);
        let facts = &answer.facts;
        let selection_text_color = if facts.text_color.has_value {
            facts.text_color.value.0
        } else {
            text_color
        };

        if selection_start > 0 {
            spans.push(RenderSpan {
                fragment_index,
                start_code_unit: 0,
                end_code_unit: selection_start,
                text_color,
                background_color: 0,
                shadow_layers: base_shadows(),
                selection_offsets: Some(selection_offsets),
                selection_text_decoration: None,
            });
        }

        if selection_start < selection_end {
            spans.push(RenderSpan {
                fragment_index,
                start_code_unit: selection_start,
                end_code_unit: selection_end,
                text_color: selection_text_color,
                background_color: facts.background_color.0,
                shadow_layers: if facts.has_text_shadow {
                    answer.shadows.clone()
                } else {
                    base_shadows()
                },
                selection_offsets: Some(selection_offsets),
                selection_text_decoration: facts.has_text_decoration.then_some(SpanTextDecoration {
                    lines: facts.text_decoration_lines,
                    line_count: facts.text_decoration_line_count,
                    style: facts.text_decoration_style,
                    color: facts.text_decoration_color.0,
                }),
            });
        }

        if selection_end < fragment.length_in_code_units {
            spans.push(RenderSpan {
                fragment_index,
                start_code_unit: selection_end,
                end_code_unit: fragment.length_in_code_units,
                text_color,
                background_color: 0,
                shadow_layers: base_shadows(),
                selection_offsets: Some(selection_offsets),
                selection_text_decoration: None,
            });
        }
    }
    spans
}

fn glyphs_of(run: &crate::painting::paintable_data::GlyphRunRecord) -> Vec<DisplayListGlyph> {
    run.glyphs
        .iter()
        .map(|glyph| DisplayListGlyph {
            position: FloatPoint { x: glyph.x, y: glyph.y },
            glyph_id: glyph.glyph_id,
        })
        .collect()
}

pub(crate) struct GlyphRunEmission {
    pub glyphs: Vec<DisplayListGlyph>,
    pub baseline_start: FloatPoint,
    pub orientation: Orientation,
    pub glyph_bounding_rect: IntRect,
}

pub(crate) fn glyph_run_emission(
    fragment: &crate::painting::paintable_data::FragmentRecord,
    run: &crate::painting::paintable_data::GlyphRunRecord,
    fragment_absolute_rect: CssPixelRect,
    scale: f64,
) -> GlyphRunEmission {
    let blob_bounds = run.retained.bounding_box(scale as f32);
    let baseline_start = FloatPoint {
        x: (fragment_absolute_rect.x.to_float() as f64 * scale) as f32,
        y: ((fragment_absolute_rect.y.to_float() + fragment.baseline.to_float()) as f64 * scale) as f32,
    };
    let orientation = if fragment.writing_mode == crate::css::css_enums::writing_mode::HORIZONTAL_TB {
        Orientation::Horizontal
    } else {
        Orientation::Vertical
    };
    let glyph_bounding_rect = IntRect::new(
        (blob_bounds[0] + baseline_start.x).round_ties_even() as i32,
        (blob_bounds[1] + baseline_start.y).round_ties_even() as i32,
        blob_bounds[2].round_ties_even() as i32,
        blob_bounds[3].round_ties_even() as i32,
    );
    GlyphRunEmission {
        glyphs: glyphs_of(run),
        baseline_start,
        orientation,
        glyph_bounding_rect,
    }
}

pub(crate) fn paint_fragments_foreground(
    recorder: &mut PaintRecorder<'_>,
    block: NodeSlotId,
    owner: Option<NodeSlotId>,
) {
    let filter = crate::painting::fragment_ownership::effective_filter(recorder.layout_arena, owner.unwrap_or(block));
    let fragment_count = recorder.layout_arena.paintable_side_data(block).fragments.len();
    let mut owned_fragment_indices = Vec::with_capacity(fragment_count);
    filter.for_each_owned_fragment_index(fragment_count, |index| owned_fragment_indices.push(index as u32));
    let spans = compute_render_spans(recorder, block, &owned_fragment_indices);

    for span in &spans {
        if Color(span.background_color).alpha() > 0 {
            let selection_rect = selection_rect(recorder, block, span);
            let converter = recorder.converter;
            recorder.recorder.fill_rect(
                converter.rounded_device_rect(selection_rect),
                Color(span.background_color),
            );
        }
    }

    for span in &spans {
        paint_text_shadow(recorder, block, span);
    }

    for span in &spans {
        let sets = crate::painting::record::paint::text_decoration::decoration_sets_for_span(recorder, block, span);
        paint_text_fragment(recorder, block, span, &sets);
    }
}

fn selection_rect(recorder: &PaintRecorder<'_>, block: NodeSlotId, span: &RenderSpan) -> CssPixelRect {
    let Some(offsets) = span.selection_offsets else {
        return CssPixelRect::default();
    };
    let fragment = &recorder.layout_arena.paintable_side_data(block).fragments[span.fragment_index as usize];
    text_fragment::rect_for_selection_offsets(recorder.layout_arena, fragment, offsets, || {
        text_fragment::first_available_font(recorder.layout_arena, fragment)
    })
}

fn paint_text_shadow(recorder: &mut PaintRecorder<'_>, block: NodeSlotId, span: &RenderSpan) {
    if span.shadow_layers.is_empty() {
        return;
    }
    let fragment = &recorder.layout_arena.paintable_side_data(block).fragments[span.fragment_index as usize];
    let Some(run) = &fragment.glyph_run else {
        return;
    };
    if run.glyphs.is_empty() {
        return;
    }

    // If this is a partial span, slice the glyph run to only include the relevant glyphs.
    let glyphs = glyphs_of(run);
    let mut span_glyphs = glyphs.as_slice();
    if span.start_code_unit != 0 || span.end_code_unit != fragment.length_in_code_units {
        let mut start_glyph = 0usize;
        let mut glyph_count = 0usize;
        let mut code_unit_offset = 0usize;
        for (i, glyph) in run.glyphs.iter().enumerate() {
            if code_unit_offset == span.start_code_unit {
                start_glyph = i;
            }
            code_unit_offset += glyph.length_in_code_units;
            if code_unit_offset == span.end_code_unit {
                glyph_count = i - start_glyph + 1;
                break;
            }
        }
        if glyph_count > 0 {
            span_glyphs = &glyphs[start_glyph..start_glyph + glyph_count];
        }
    }

    let converter = recorder.converter;
    let scale = recorder.inputs.device_pixels_per_css_pixel;
    let fragment_width = converter.enclosing_device_pixels(fragment.size.width);
    let fragment_height = converter.enclosing_device_pixels(fragment.size.height);
    let fragment_baseline = converter.rounded_device_pixels(fragment.baseline);
    let fragment_absolute_rect = text_fragment::absolute_rect(recorder.layout_arena, fragment);
    let font_id = recorder.register_font(run.font.as_raw());

    // Shadow layers are ordered front-to-back, so we paint them in reverse.
    for layer in span.shadow_layers.iter().rev() {
        let blur_radius = converter.rounded_device_pixels(layer.blur_radius);
        // Space around the painted text to allow it to blur.
        let margin = blur_radius * 2;
        let text_rect = IntRect::new(margin, margin, fragment_width, fragment_height);
        let bounding_rect = IntRect::new(
            0,
            0,
            text_rect.width + margin + margin,
            text_rect.height + margin + margin,
        );

        // FIXME: this is close but not quite perfect. non integer scale values can be offset by tiny amounts.
        let css_margin = layer.blur_radius * 2;
        let draw_location = FloatPoint {
            x: (fragment_absolute_rect.x + layer.offset_x - css_margin).to_float() * scale as f32,
            y: (fragment_absolute_rect.y + layer.offset_y - css_margin).to_float() * scale as f32,
        };
        recorder.recorder.paint_text_shadow(
            blur_radius,
            bounding_rect,
            IntRect::new(
                text_rect.x,
                text_rect.y + fragment_baseline,
                text_rect.width,
                text_rect.height,
            ),
            GlyphRunForRecording {
                font_id: FontResourceId(font_id),
                glyphs: span_glyphs,
            },
            scale,
            Color(layer.color),
            draw_location,
        );
    }
}

fn paint_text_fragment(
    recorder: &mut PaintRecorder<'_>,
    block: NodeSlotId,
    span: &RenderSpan,
    decoration_sets: &[crate::painting::record::paint::text_decoration::TextDecorationSet],
) {
    if span.start_code_unit == span.end_code_unit {
        return;
    }
    let fragment = &recorder.layout_arena.paintable_side_data(block).fragments[span.fragment_index as usize];
    if recorder.inputs.should_show_line_box_borders {
        let converter = recorder.converter;
        let fragment_absolute_rect = text_fragment::absolute_rect(recorder.layout_arena, fragment);
        let fragment_absolute_device_rect = converter.enclosing_device_rect(fragment_absolute_rect);
        recorder
            .recorder
            .draw_rect(fragment_absolute_device_rect, Color::from_rgb(0, 255, 0), false);
        let one = crate::css::css_pixels::CssPixels::from_integer(1);
        let baseline_start = converter.rounded_device_point(
            fragment_absolute_rect
                .location()
                .translated(crate::css::css_pixels::CssPixels::default(), fragment.baseline),
        );
        let baseline_end = converter.rounded_device_point(
            crate::css::css_pixels::CssPixelPoint::new(fragment_absolute_rect.right(), fragment_absolute_rect.y)
                .translated(-one, fragment.baseline),
        );
        recorder.recorder.draw_line(
            baseline_start,
            baseline_end,
            Color::from_rgb(255, 0, 0),
            1,
            libgfx_rust::LineStyle::Solid,
            Color::TRANSPARENT,
        );
    }
    let Some(run) = &fragment.glyph_run else {
        return;
    };
    let converter = recorder.converter;
    let scale = recorder.inputs.device_pixels_per_css_pixel;
    let fragment_absolute_rect = text_fragment::absolute_rect(recorder.layout_arena, fragment);
    let fragment_device_rect = converter.enclosing_device_rect(fragment_absolute_rect);
    let font_id = recorder.register_font(run.font.as_raw());
    let GlyphRunEmission {
        glyphs,
        baseline_start,
        orientation,
        glyph_bounding_rect,
    } = glyph_run_emission(fragment, run, fragment_absolute_rect, scale);
    let run_for_recording = GlyphRunForRecording {
        font_id: FontResourceId(font_id),
        glyphs: &glyphs,
    };

    // Paint text, clipped to span range if not full fragment.
    let is_full_fragment = span.start_code_unit == 0 && span.end_code_unit == fragment.length_in_code_units;
    let mut decoration_box = fragment_absolute_rect;
    if is_full_fragment {
        recorder.recorder.draw_glyph_run(
            baseline_start,
            run_for_recording,
            Color(span.text_color),
            fragment_device_rect,
            scale,
            orientation,
            glyph_bounding_rect,
        );
    } else {
        let range_rect = text_fragment::rect_for_selection_offsets(
            recorder.layout_arena,
            fragment,
            SelectionOffsets {
                start: span.start_code_unit,
                end: span.end_code_unit,
            },
            || text_fragment::first_available_font(recorder.layout_arena, fragment),
        );
        let span_rect = converter.rounded_device_rect(range_rect);
        recorder.recorder.record_clipped_to(span_rect, |recorder| {
            recorder.draw_glyph_run(
                baseline_start,
                run_for_recording,
                Color(span.text_color),
                fragment_device_rect,
                scale,
                orientation,
                glyph_bounding_rect,
            );
        });
        decoration_box.x = range_rect.x;
        decoration_box.width = range_rect.width;
    }

    for set in decoration_sets {
        crate::painting::record::paint::text_decoration::paint_decoration_lines(
            recorder,
            block,
            span.fragment_index,
            decoration_box,
            set,
        );
    }
}

// Paints the caret when it sits in a fragment owned by `owner`; the block itself
// (owner == None) also handles blank lines and empty editable elements.
pub(crate) fn paint_cursor(recorder: &mut PaintRecorder<'_>, block: NodeSlotId, owner: Option<NodeSlotId>) {
    let owner_shell = owner.map_or(std::ptr::null_mut(), |owner| recorder.layout_node_shell(owner));
    let facts = recorder
        .paint_host
        .cursor_facts(recorder.layout_node_shell(block), owner_shell);
    if !facts.paints {
        return;
    }
    let color = facts.color;
    if color.alpha() == 0 {
        return;
    }
    let converter = recorder.converter;
    recorder
        .recorder
        .fill_rect(converter.rounded_device_rect(CssPixelRect::from(facts.rect)), color);
}
