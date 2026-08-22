/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::computed_value_views::LengthPercentageRef;
use crate::css::css_enums::border_image_repeat;
use crate::css::css_pixels::{CssPixelFraction, CssPixels};
use crate::css::css_pixels::{CssPixelRect, CssPixelSize};
use crate::css::style_value::StyleValueData;
use crate::layout::node_data::NodeSlotId;
use crate::painting::display_list::commands::ImageFrameResourceId;
use crate::painting::host::FfiLayerImageList;
use crate::painting::record::PaintRecorder;
use crate::painting::record::paint::background::to_gfx_scaling_mode;
use crate::painting::style_queries;
use libgfx_rust::{FloatRect, FloatSize, IntRect};

#[derive(Clone, Copy, PartialEq, Eq)]
enum Track {
    Start,
    Center,
    End,
}

const TRACKS: [Track; 3] = [Track::Start, Track::Center, Track::End];

#[derive(Clone, Copy)]
struct Axis([CssPixels; 4]);

impl Axis {
    fn track_start(self, track: Track) -> CssPixels {
        match track {
            Track::Start => self.0[0],
            Track::Center => self.0[1],
            Track::End => self.0[2],
        }
    }

    fn track_end(self, track: Track) -> CssPixels {
        match track {
            Track::Start => self.0[1],
            Track::Center => self.0[2],
            Track::End => self.0[3],
        }
    }

    fn track_size(self, track: Track) -> CssPixels {
        self.track_end(track) - self.track_start(track)
    }
}

fn scale_source_length_to_destination(
    source_length: CssPixels,
    source_reference: CssPixels,
    destination_reference: CssPixels,
) -> CssPixels {
    if source_reference <= CssPixels::from_raw(0) {
        return source_length;
    }
    let wide = source_length.raw_value() as i64 * destination_reference.raw_value() as i64;
    let scaled_length = CssPixels::from_raw(
        (wide / source_reference.raw_value() as i64).clamp(i32::MIN as i64, i32::MAX as i64) as i32,
    );
    CssPixels::from_raw(1).max(scaled_length)
}

fn rounded_repeated_border_image_tile_size(
    mut tile_size: FloatSize,
    dest_rect: IntRect,
    repeat_x: u8,
    repeat_y: u8,
) -> FloatSize {
    if repeat_x == border_image_repeat::STRETCH {
        tile_size.width = dest_rect.width as f32;
    } else if repeat_x == border_image_repeat::ROUND && tile_size.width > 0.0 {
        let tile_count = 1.max((dest_rect.width as f64 / tile_size.width as f64).round_ties_even() as i32);
        tile_size.width = dest_rect.width as f32 / tile_count as f32;
    }
    if repeat_y == border_image_repeat::STRETCH {
        tile_size.height = dest_rect.height as f32;
    } else if repeat_y == border_image_repeat::ROUND && tile_size.height > 0.0 {
        let tile_count = 1.max((dest_rect.height as f64 / tile_size.height as f64).round_ties_even() as i32);
        tile_size.height = dest_rect.height as f32 / tile_count as f32;
    }
    tile_size
}

#[allow(clippy::too_many_arguments)]
fn paint_border_image_slice(
    recorder: &mut PaintRecorder<'_>,
    frame_id: ImageFrameResourceId,
    source_rect: FloatRect,
    dest_rect: IntRect,
    device_tile_size: FloatSize,
    image_rendering: u8,
    repeat_x: u8,
    repeat_y: u8,
) {
    if source_rect.is_empty() || dest_rect.is_empty() {
        return;
    }

    let device_tile_size = rounded_repeated_border_image_tile_size(device_tile_size, dest_rect, repeat_x, repeat_y);
    if device_tile_size.width <= 0.0 || device_tile_size.height <= 0.0 {
        return;
    }

    let source_size = (
        source_rect.width.round_ties_even() as i32,
        source_rect.height.round_ties_even() as i32,
    );

    let mut start_x = dest_rect.x as f32;
    let mut start_y = dest_rect.y as f32;
    let mut tile_step = device_tile_size;
    let mut tile_count_x: Option<u32> = None;
    let mut tile_count_y: Option<u32> = None;

    if repeat_x == border_image_repeat::STRETCH {
        tile_count_x = Some(1);
    }
    if repeat_y == border_image_repeat::STRETCH {
        tile_count_y = Some(1);
    }

    if repeat_x == border_image_repeat::SPACE || repeat_y == border_image_repeat::SPACE {
        let whole_tiles_x = if repeat_x == border_image_repeat::SPACE {
            (dest_rect.width as f32 / device_tile_size.width).floor() as i32
        } else {
            1
        };
        let whole_tiles_y = if repeat_y == border_image_repeat::SPACE {
            (dest_rect.height as f32 / device_tile_size.height).floor() as i32
        } else {
            1
        };
        if whole_tiles_x <= 0 || whole_tiles_y <= 0 {
            return;
        }

        let gap_x = if repeat_x == border_image_repeat::SPACE {
            (dest_rect.width as f32 - whole_tiles_x as f32 * device_tile_size.width) / (whole_tiles_x + 1) as f32
        } else {
            0.0
        };
        let gap_y = if repeat_y == border_image_repeat::SPACE {
            (dest_rect.height as f32 - whole_tiles_y as f32 * device_tile_size.height) / (whole_tiles_y + 1) as f32
        } else {
            0.0
        };

        if repeat_x == border_image_repeat::SPACE {
            start_x = dest_rect.x as f32 + gap_x;
            tile_step.width = device_tile_size.width + gap_x;
            tile_count_x = Some(whole_tiles_x as u32);
        } else {
            tile_count_x = Some(1);
        }

        if repeat_y == border_image_repeat::SPACE {
            start_y = dest_rect.y as f32 + gap_y;
            tile_step.height = device_tile_size.height + gap_y;
            tile_count_y = Some(whole_tiles_y as u32);
        } else {
            tile_count_y = Some(1);
        }
    } else {
        if repeat_x == border_image_repeat::REPEAT {
            start_x += (dest_rect.width as f32 - device_tile_size.width) / 2.0;
            while start_x > dest_rect.x as f32 {
                start_x -= device_tile_size.width;
            }
        }
        if repeat_y == border_image_repeat::REPEAT {
            start_y += (dest_rect.height as f32 - device_tile_size.height) / 2.0;
            while start_y > dest_rect.y as f32 {
                start_y -= device_tile_size.height;
            }
        }
    }

    let tile_size_for_scaling = (
        1.max(device_tile_size.width.round_ties_even() as i32),
        1.max(device_tile_size.height.round_ties_even() as i32),
    );
    let scaling_mode = to_gfx_scaling_mode(image_rendering, source_size, tile_size_for_scaling);
    recorder.recorder.draw_tiled_decoded_image_frame(
        FloatRect::new(start_x, start_y, device_tile_size.width, device_tile_size.height),
        dest_rect,
        source_rect,
        tile_step,
        frame_id,
        scaling_mode,
        tile_count_x,
        tile_count_y,
    );
}

fn component_items(value: &StyleValueData) -> Vec<&StyleValueData> {
    if let StyleValueData::ValueList { values, .. } = value {
        return values.as_slice().iter().map(|item| item.data()).collect();
    }
    vec![value]
}

// https://drafts.csswg.org/css-backgrounds-3/#border-image-slice
fn resolve_slice_px(value: &StyleValueData, reference_length: CssPixels) -> CssPixels {
    match value {
        StyleValueData::Number { value } => CssPixels::nearest_value_for(*value),
        StyleValueData::Integer { value } => CssPixels::nearest_value_for(*value as f64),
        StyleValueData::Percentage { value } => {
            CssPixels::nearest_value_for(reference_length.to_double() * (*value * 0.01))
        }
        StyleValueData::Calculated { .. } => {
            if let Some(percentage) = crate::css::calc::resolve_calculated_percentage_without_context(value) {
                CssPixels::nearest_value_for(reference_length.to_double() * (percentage * 0.01))
            } else {
                CssPixels::nearest_value_for(
                    crate::css::calc::resolve_calculated_number_without_context(value)
                        .expect("computed border-image-slice calc resolves to a number"),
                )
            }
        }
        _ => unreachable!("computed border-image-slice holds an unknown value"),
    }
}

fn resolve_outset_px(value: &StyleValueData, border_width: CssPixels) -> CssPixels {
    match value {
        StyleValueData::Number { value } => CssPixels::nearest_value_for(border_width.to_double() * *value),
        StyleValueData::Integer { value } => CssPixels::nearest_value_for(border_width.to_double() * *value as f64),
        StyleValueData::Calculated { .. }
            if crate::css::calc::resolve_calculated_number_without_context(value).is_some() =>
        {
            let number =
                crate::css::calc::resolve_calculated_number_without_context(value).expect("checked just above");
            CssPixels::nearest_value_for(border_width.to_double() * number)
        }
        other => LengthPercentageRef::over(other).absolute_length_to_px(),
    }
}

/// https://drafts.csswg.org/css-backgrounds-3/#border-image-width
fn resolve_width_px(
    value: &StyleValueData,
    border_width: CssPixels,
    reference_length: CssPixels,
    auto_width: CssPixels,
) -> CssPixels {
    match value {
        StyleValueData::Keyword { .. } => auto_width,
        StyleValueData::Number { value } => CssPixels::nearest_value_for(border_width.to_double() * *value),
        StyleValueData::Integer { value } => CssPixels::nearest_value_for(border_width.to_double() * *value as f64),
        StyleValueData::Calculated { .. }
            if crate::css::calc::resolve_calculated_number_without_context(value).is_some() =>
        {
            let number =
                crate::css::calc::resolve_calculated_number_without_context(value).expect("checked just above");
            CssPixels::nearest_value_for(border_width.to_double() * number)
        }
        other => LengthPercentageRef::over(other).to_px(reference_length),
    }
}

fn shrink_opposite_sides_to_fit(first: &mut CssPixels, second: &mut CssPixels, available: CssPixels) {
    let total = *first + *second;
    if total <= available {
        return;
    }
    let factor = CssPixelFraction::ratio_of(available, total);
    *first = first.mul_by_fraction(factor);
    *second = second.mul_by_fraction(factor);
}

struct ResolvedBorderImageGeometry {
    source_columns: Axis,
    source_rows: Axis,
    destination_columns: Axis,
    destination_rows: Axis,
    source_slices: [CssPixels; 4],
    widths: [CssPixels; 4],
}

fn resolve_border_image_geometry(
    slice_values: [&StyleValueData; 4],
    width_items: Vec<&StyleValueData>,
    outset_items: Vec<&StyleValueData>,
    source_size: (i32, i32),
    border_box_rect: CssPixelRect,
    border_width: [CssPixels; 4],
) -> ResolvedBorderImageGeometry {
    let (source_width, source_height) = source_size;
    let source_width = CssPixels::from_integer(source_width as i64);
    let source_height = CssPixels::from_integer(source_height as i64);

    let [slice_top, slice_right, slice_bottom, slice_left] = slice_values;
    let mut source_slices = [
        resolve_slice_px(slice_top, source_height),
        resolve_slice_px(slice_right, source_width),
        resolve_slice_px(slice_bottom, source_height),
        resolve_slice_px(slice_left, source_width),
    ];
    {
        let [top, right, bottom, left] = &mut source_slices;
        shrink_opposite_sides_to_fit(left, right, source_width);
        shrink_opposite_sides_to_fit(top, bottom, source_height);
    }

    let resolved_outset = [
        resolve_outset_px(outset_items[0], border_width[0]),
        resolve_outset_px(outset_items[1 % outset_items.len()], border_width[1]),
        resolve_outset_px(outset_items[2 % outset_items.len()], border_width[2]),
        resolve_outset_px(outset_items[3 % outset_items.len()], border_width[3]),
    ];

    let mut border_image_rect = border_box_rect;
    border_image_rect.inflate(
        resolved_outset[0],
        resolved_outset[1],
        resolved_outset[2],
        resolved_outset[3],
    );

    // https://drafts.csswg.org/css-backgrounds-3/#border-image-process
    let mut widths = [
        resolve_width_px(
            width_items[0],
            border_width[0],
            border_image_rect.height,
            source_slices[0],
        ),
        resolve_width_px(
            width_items[1 % width_items.len()],
            border_width[1],
            border_image_rect.width,
            source_slices[1],
        ),
        resolve_width_px(
            width_items[2 % width_items.len()],
            border_width[2],
            border_image_rect.height,
            source_slices[2],
        ),
        resolve_width_px(
            width_items[3 % width_items.len()],
            border_width[3],
            border_image_rect.width,
            source_slices[3],
        ),
    ];
    {
        let [top, right, bottom, left] = &mut widths;
        shrink_opposite_sides_to_fit(left, right, border_image_rect.width);
        shrink_opposite_sides_to_fit(top, bottom, border_image_rect.height);
    }

    let zero = CssPixels::from_raw(0);
    ResolvedBorderImageGeometry {
        source_columns: Axis([zero, source_slices[3], source_width - source_slices[1], source_width]),
        source_rows: Axis([zero, source_slices[0], source_height - source_slices[2], source_height]),
        destination_columns: Axis([
            border_image_rect.left(),
            border_image_rect.left() + widths[3],
            border_image_rect.right() - widths[1],
            border_image_rect.right(),
        ]),
        destination_rows: Axis([
            border_image_rect.top(),
            border_image_rect.top() + widths[0],
            border_image_rect.bottom() - widths[2],
            border_image_rect.bottom(),
        ]),
        source_slices,
        widths,
    }
}

pub(crate) fn paint_border_image(
    recorder: &mut PaintRecorder<'_>,
    paintable: NodeSlotId,
    css_border_widths: [CssPixels; 4],
    border_box_rect: CssPixelRect,
) -> bool {
    let layout_arena = recorder.layout_arena;
    let Some(style) = layout_arena.node_style_if_live(paintable) else {
        return false;
    };
    let border = style.border();
    let Some(source) = style_queries::handle_value(&border.border_image_source) else {
        return false;
    };
    // FIXME: Support all abstract image sources here. NodeWithStyle loads and observes gradients and
    // image-set(), but this painting path currently only handles raster images.
    if !matches!(source, StyleValueData::Image { .. }) {
        return false;
    }
    let image_rendering = style.image_rendering();
    let frame = recorder.paint_host.layer_image_current_frame(
        recorder.layout_node_shell(paintable),
        FfiLayerImageList::BorderImageSource,
        0,
        libgfx_rust::IntRect::default(),
    );
    if !frame.has_frame {
        return false;
    }

    let Some(StyleValueData::BorderImageSlice {
        top: slice_top,
        right: slice_right,
        bottom: slice_bottom,
        left: slice_left,
        fill,
    }) = style_queries::handle_value(&border.border_image_slice)
    else {
        unreachable!("computed border-image-slice holds a slice value");
    };
    let width_value = style_queries::handle_value(&border.border_image_width)
        .expect("computed border-image-width lost its style value");
    let outset_value = style_queries::handle_value(&border.border_image_outset)
        .expect("computed border-image-outset lost its style value");
    let repeat_value = style_queries::handle_value(&border.border_image_repeat)
        .expect("computed border-image-repeat lost its style value");
    let repeat_items = component_items(repeat_value);
    let repeat_keyword = |item: &StyleValueData| {
        let StyleValueData::Keyword { keyword } = item else {
            unreachable!("computed border-image-repeat holds a keyword");
        };
        crate::css::css_enums::keyword_to_border_image_repeat(*keyword).unwrap_or(border_image_repeat::STRETCH)
    };
    let repeat_x = repeat_keyword(repeat_items[0]);
    let repeat_y = repeat_keyword(repeat_items[1 % repeat_items.len()]);

    let geometry = resolve_border_image_geometry(
        [
            slice_top.data(),
            slice_right.data(),
            slice_bottom.data(),
            slice_left.data(),
        ],
        component_items(width_value),
        component_items(outset_value),
        (frame.frame_width, frame.frame_height),
        border_box_rect,
        css_border_widths,
    );
    let fill = *fill;
    let source_columns = geometry.source_columns;
    let source_rows = geometry.source_rows;
    let destination_columns = geometry.destination_columns;
    let destination_rows = geometry.destination_rows;
    let source_slices = geometry.source_slices;
    let widths = geometry.widths;
    let frame_id = ImageFrameResourceId(frame.frame_id);
    let scale = recorder.converter.device_pixels_per_css_pixel() as f32;

    // A tile keeps the source region's aspect ratio at the scale forced by the border thickness.
    let natural_tile_size = |column: Track, row: Track, destination_rect: CssPixelRect| -> CssPixelSize {
        let source_width = source_columns.track_size(column);
        let source_height = source_rows.track_size(row);
        let mut tile_width = destination_rect.width;
        let mut tile_height = destination_rect.height;
        if column == Track::Center && row != Track::Center {
            tile_width = scale_source_length_to_destination(source_width, source_height, destination_rect.height);
        } else if row == Track::Center && column != Track::Center {
            tile_height = scale_source_length_to_destination(source_height, source_width, destination_rect.width);
        } else if column == Track::Center && row == Track::Center {
            // The centre fill is scaled horizontally like the top edge and vertically like the left edge.
            tile_width = scale_source_length_to_destination(source_width, source_slices[0], widths[0]);
            tile_height = scale_source_length_to_destination(source_height, source_slices[3], widths[3]);
        }
        CssPixelSize::new(tile_width, tile_height)
    };

    for row in TRACKS {
        for column in TRACKS {
            if column == Track::Center && row == Track::Center && !fill {
                // The centre is only painted when the 'fill' keyword is present.
                continue;
            }
            // Only the centre track of an axis honours border-image-repeat; the edges always stretch.
            let source_rect = FloatRect::new(
                source_columns.track_start(column).to_float(),
                source_rows.track_start(row).to_float(),
                source_columns.track_size(column).to_float(),
                source_rows.track_size(row).to_float(),
            );
            let destination_rect = CssPixelRect::new(
                destination_columns.track_start(column),
                destination_rows.track_start(row),
                destination_columns.track_size(column),
                destination_rows.track_size(row),
            );
            let destination_device_rect = recorder.converter.rounded_device_rect(destination_rect);
            let tile_size = natural_tile_size(column, row, destination_rect);
            let tile_device_size = FloatSize {
                width: tile_size.width.to_float() * scale,
                height: tile_size.height.to_float() * scale,
            };
            let horizontal_repeat = if column == Track::Center {
                repeat_x
            } else {
                border_image_repeat::STRETCH
            };
            let vertical_repeat = if row == Track::Center {
                repeat_y
            } else {
                border_image_repeat::STRETCH
            };
            paint_border_image_slice(
                recorder,
                frame_id,
                source_rect,
                destination_device_rect,
                tile_device_size,
                image_rendering,
                horizontal_repeat,
                vertical_repeat,
            );
        }
    }
    true
}
