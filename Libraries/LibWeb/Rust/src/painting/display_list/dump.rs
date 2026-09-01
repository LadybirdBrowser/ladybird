/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::builder::{HEADER_SIZE, read_header};
use super::commands::*;
use crate::css::color_resolution::format_to_8bit_compatible;
use crate::painting::dump::{
    push_float_like_ak, push_float_point, push_float_rect, push_float_size, push_int_point, push_int_rect,
    push_int_size,
};
use libgfx_rust::path::OwnedPath;
use libgfx_rust::{
    Color, CompositingAndBlendingOperator, CornerRadii, FloatPoint, FloatRect, FloatSize, IntPoint, IntRect, IntSize,
    LineStyle, ScalingMode,
};
use std::ffi::c_void;
use std::fmt::Write;

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiPaintingDumpCallbacks {
    pub context: *mut c_void,
    pub owner_label:
        unsafe extern "C" fn(context: *mut c_void, is_frame: bool, index: u32, label_sink: *mut c_void) -> bool,
    pub command_bytes:
        unsafe extern "C" fn(context: *mut c_void, display_list: *const c_void, byte_count: *mut usize) -> *const u8,
    pub nested_display_list: unsafe extern "C" fn(context: *mut c_void, display_list_id: u64) -> *const c_void,
    pub mask_display_list_count: unsafe extern "C" fn(context: *mut c_void, display_list: *const c_void) -> usize,
    pub mask_display_lists: unsafe extern "C" fn(
        context: *mut c_void,
        display_list: *const c_void,
        frames: *mut u32,
        display_list_ids: *mut u64,
    ),
    pub append_text: unsafe extern "C" fn(context: *mut c_void, bytes: *const u8, byte_count: usize),
}

impl FfiPaintingDumpCallbacks {
    fn owner_label(&self, is_frame: bool, index: u32) -> Option<String> {
        let mut label = Vec::new();
        // SAFETY: The C++ host fills the label sink synchronously through the exported push function.
        let has_label = unsafe { (self.owner_label)(self.context, is_frame, index, (&raw mut label).cast()) };
        has_label.then(|| String::from_utf8_lossy(&label).into_owned())
    }

    fn command_bytes(&self, display_list: *const c_void) -> &[u8] {
        let mut byte_count = 0;
        // SAFETY: The host owns the display list for the duration of the dump and returns a span
        // that stays live for this call.
        let bytes = unsafe { (self.command_bytes)(self.context, display_list, &raw mut byte_count) };
        if byte_count == 0 {
            return &[];
        }
        assert!(!bytes.is_null());
        // SAFETY: The host reported `byte_count` readable bytes at `bytes`.
        unsafe { std::slice::from_raw_parts(bytes, byte_count) }
    }

    fn nested_display_list(&self, display_list_id: DisplayListResourceId) -> *const c_void {
        // SAFETY: Nested ids come from records the host produced, so they resolve in its storage.
        let display_list = unsafe { (self.nested_display_list)(self.context, display_list_id.0) };
        assert!(!display_list.is_null());
        display_list
    }

    fn mask_display_lists(&self, display_list: *const c_void) -> Vec<(FrameNodeIndex, DisplayListResourceId)> {
        // SAFETY: The host reports how many mask entries this display list holds.
        let count = unsafe { (self.mask_display_list_count)(self.context, display_list) };
        let mut frames = vec![0; count];
        let mut display_list_ids = vec![0; count];
        // SAFETY: Both buffers hold the `count` entries the host just reported.
        unsafe {
            (self.mask_display_lists)(
                self.context,
                display_list,
                frames.as_mut_ptr(),
                display_list_ids.as_mut_ptr(),
            );
        };
        let mut masks: Vec<_> = frames
            .into_iter()
            .zip(display_list_ids)
            .map(|(frame, id)| (FrameNodeIndex(frame), DisplayListResourceId(id)))
            .collect();
        masks.sort_unstable_by_key(|(frame, _)| *frame);
        masks
    }

    fn append_text(&self, text: &str) {
        // SAFETY: The C++ sink copies the completed dump synchronously.
        unsafe { (self.append_text)(self.context, text.as_ptr(), text.len()) };
    }
}

/// # Safety
///
/// `visual_context_tree` must be a live retained tree handle; `command_runs` must address
/// `command_run_count` runs; `display_list` and every pointer returned by `callbacks` must remain
/// live for this call. The callback byte spans must contain display-list records produced by this
/// build of LibWeb. `owner_label` is called synchronously with a `Vec<u8>` sink the host fills
/// through `layout_arena_paint_push_bytes`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn painting_dump(
    visual_context_tree: *const c_void,
    command_runs: *const DisplayListCommandRun,
    command_run_count: usize,
    display_list: *const c_void,
    callbacks: FfiPaintingDumpCallbacks,
) {
    crate::abort_on_panic(|| {
        assert!(!display_list.is_null());
        let visual_context_tree = unsafe { crate::painting::ffi::tree_from_handle(visual_context_tree) };
        let command_runs = unsafe { crate::painting::ffi::ffi_slice(command_runs, command_run_count) };
        let mut output = visual_context_tree
            .dump_nodes_reachable_from_runs(command_runs, |is_frame, index| callbacks.owner_label(is_frame, index));
        output.push_str("\nDisplayList:\n");
        dump_commands(&mut output, &callbacks, display_list, 0);
        callbacks.append_text(&output);
    });
}

fn push_indent(output: &mut String, indent: usize) {
    output.extend(std::iter::repeat_n(' ', indent * 2));
}

fn dump_commands(
    output: &mut String,
    callbacks: &FfiPaintingDumpCallbacks,
    display_list: *const c_void,
    base_indent: usize,
) {
    let bytes = callbacks.command_bytes(display_list);
    let mut offset = 0;
    while offset < bytes.len() {
        let header = read_header(&bytes[offset..]);
        offset += HEADER_SIZE;
        let payload_size = header.payload_size as usize;
        assert!(payload_size <= bytes.len() - offset);
        let payload = &bytes[offset..offset + payload_size];
        offset += payload_size;

        push_indent(output, base_indent);
        write!(output, "{}@", header.command_type.name()).unwrap();
        write_context(output, header.context);
        dump_command(output, header.command_type, payload);
        if header.inline_clip_count > 0 {
            dump_inline_clips(output, &header, payload);
        }
        output.push('\n');

        let Some(nested) = nested_display_lists(header.command_type, payload) else {
            continue;
        };
        dump_commands(
            output,
            callbacks,
            callbacks.nested_display_list(nested.display_list),
            base_indent + 1,
        );
        if let Some(mask) = nested.mask {
            push_indent(output, base_indent + 1);
            output.push_str("Mask:\n");
            dump_commands(output, callbacks, callbacks.nested_display_list(mask), base_indent + 2);
        }
    }
    assert_eq!(offset, bytes.len());

    for (frame, id) in callbacks.mask_display_lists(display_list) {
        push_indent(output, base_indent);
        writeln!(output, "MaskDisplayList for frame f{}:", frame.0).unwrap();
        dump_commands(output, callbacks, callbacks.nested_display_list(id), base_indent + 1);
    }
}

struct NestedDisplayLists {
    display_list: DisplayListResourceId,
    mask: Option<DisplayListResourceId>,
}

fn nested_display_lists(command_type: DisplayListCommandType, payload: &[u8]) -> Option<NestedDisplayLists> {
    match command_type {
        DisplayListCommandType::PaintNestedDisplayList => Some(NestedDisplayLists {
            display_list: read_command::<PaintNestedDisplayList>(payload).display_list_id,
            mask: None,
        }),
        DisplayListCommandType::DrawIsolatedDisplayList => {
            let command = read_command::<DrawIsolatedDisplayList>(payload);
            Some(NestedDisplayLists {
                display_list: command.display_list_id,
                mask: (command.mask_display_list_id != NO_MASK_DISPLAY_LIST).then_some(command.mask_display_list_id),
            })
        }
        _ => None,
    }
}

trait DumpValue {
    fn push_dump(self, output: &mut String);
}

macro_rules! dump_value_via {
    ($($type:ty => $push:expr),+ $(,)?) => {
        $(impl DumpValue for $type {
            fn push_dump(self, output: &mut String) {
                $push(output, self);
            }
        })+
    };
}

dump_value_via! {
    IntPoint => push_int_point,
    FloatPoint => push_float_point,
    IntSize => push_int_size,
    FloatSize => push_float_size,
    IntRect => push_int_rect,
    FloatRect => push_float_rect,
    f32 => push_float_like_ak,
    Color => write_color,
    SpatialNodeIndex => write_spatial_node_index,
}

fn write_spatial_node_index(output: &mut String, index: SpatialNodeIndex) {
    write!(output, "s{}", index.0).unwrap();
}

fn write_field(output: &mut String, label: &str, value: impl DumpValue) {
    output.push(' ');
    output.push_str(label);
    output.push('=');
    value.push_dump(output);
}

fn read_command<C: Copy>(payload: &[u8]) -> C {
    assert!(payload.len() >= std::mem::size_of::<C>());
    // SAFETY: Display-list records are native-layout copies of these `Copy` command structs. The
    // byte stream is validated at the C++ boundary, and `read_unaligned` does not require the
    // payload pointer to have `C`'s alignment.
    unsafe { std::ptr::read_unaligned(payload.as_ptr().cast::<C>()) }
}

fn dump_command(output: &mut String, command_type: DisplayListCommandType, payload: &[u8]) {
    match command_type {
        DisplayListCommandType::DrawGlyphRun => {
            let command = read_command::<DrawGlyphRun>(payload);
            write_field(output, "rect", command.rect);
            write_field(output, "translation", command.translation);
            write_field(output, "color", command.color);
        }
        DisplayListCommandType::FillRect => {
            let command = read_command::<FillRect>(payload);
            write_field(output, "rect", command.rect);
            write_field(output, "color", command.color);
            write_blend_mode(output, command.compositing_and_blending_operator);
        }
        DisplayListCommandType::PaintCaret => {
            let command = read_command::<PaintCaret>(payload);
            write_field(output, "rect", command.rect);
            write_field(output, "color", command.color);
            write!(output, " should_blink={}", command.should_blink).unwrap();
        }
        DisplayListCommandType::DrawScaledDecodedImageFrame => {
            let command = read_command::<DrawScaledDecodedImageFrame>(payload);
            write_field(output, "dst_rect", command.dst_rect);
            if let Some(rect) = command.src_rect.get() {
                write_field(output, "src_rect", rect);
            }
            write_blend_mode(output, command.compositing_and_blending_operator);
            if let Some(color) = command.isolated_backdrop_color.get() {
                write_field(output, "isolated_backdrop_color", color);
            }
        }
        DisplayListCommandType::DrawRepeatedDecodedImageFrame => {
            let command = read_command::<DrawRepeatedDecodedImageFrame>(payload);
            write_field(output, "dst_rect", command.dst_rect);
            write_field(output, "clip_rect", command.clip_rect);
            write_blend_mode(output, command.compositing_and_blending_operator);
            if let Some(color) = command.isolated_backdrop_color.get() {
                write_field(output, "isolated_backdrop_color", color);
            }
        }
        DisplayListCommandType::DrawRepeatedDisplayList => {
            let command = read_command::<DrawRepeatedDisplayList>(payload);
            write_field(output, "dst_rect", command.dst_rect);
            write_field(output, "clip_rect", command.clip_rect);
            write!(output, " scaling_mode={}", scaling_mode_name(command.scaling_mode)).unwrap();
            write_blend_mode(output, command.compositing_and_blending_operator);
        }
        DisplayListCommandType::DrawTiledDecodedImageFrame => {
            let command = read_command::<DrawTiledDecodedImageFrame>(payload);
            write_field(output, "tile_rect", command.tile_rect);
            write_field(output, "clip_rect", command.clip_rect);
            write_field(output, "src_rect", command.src_rect);
            write_field(output, "tile_step", command.tile_step);
            if let Some(count) = command.tile_count_x.get() {
                write!(output, " tile_count_x={count}").unwrap();
            } else {
                output.push_str(" tile_count_x=repeat");
            }
            if let Some(count) = command.tile_count_y.get() {
                write!(output, " tile_count_y={count}").unwrap();
            } else {
                output.push_str(" tile_count_y=repeat");
            }
        }
        DisplayListCommandType::DrawCompositedContext => {
            let command = read_command::<DrawCompositedContext>(payload);
            write_field(output, "dst_rect", command.dst_rect);
        }
        DisplayListCommandType::DrawCanvas => {
            let command = read_command::<DrawCanvas>(payload);
            write_field(output, "dst_rect", command.dst_rect);
            write!(output, " content_generation={}", command.content_generation).unwrap();
        }
        DisplayListCommandType::DrawVideoFrame => {
            let command = read_command::<DrawVideoFrame>(payload);
            write_field(output, "dst_rect", command.dst_rect);
        }
        DisplayListCommandType::PaintLinearGradient => {
            let command = read_command::<PaintLinearGradient>(payload);
            write_field(output, "rect", command.gradient_rect);
            write_blend_mode(output, command.compositing_and_blending_operator);
        }
        DisplayListCommandType::PaintRadialGradient => {
            let command = read_command::<PaintRadialGradient>(payload);
            write_field(output, "rect", command.rect);
            write_field(output, "center", command.center);
            write_field(output, "size", command.size);
            write_blend_mode(output, command.compositing_and_blending_operator);
        }
        DisplayListCommandType::PaintConicGradient => {
            let command = read_command::<PaintConicGradient>(payload);
            write_field(output, "rect", command.rect);
            write_field(output, "position", command.position);
            write_field(output, "angle", command.start_angle);
            write_blend_mode(output, command.compositing_and_blending_operator);
        }
        DisplayListCommandType::PaintOuterBoxShadow => {
            let command = read_command::<PaintOuterBoxShadow>(payload);
            write_field(output, "content_rect", command.device_content_rect);
            write_field(output, "shadow_rect", command.shadow_rect);
            write!(output, " blur_radius={}", command.blur_radius).unwrap();
            write_field(output, "color", command.color);
        }
        DisplayListCommandType::PaintInnerBoxShadow => {
            let command = read_command::<PaintInnerBoxShadow>(payload);
            write_field(output, "content_rect", command.device_content_rect);
            write_field(output, "outer_shadow_rect", command.outer_shadow_rect);
            write_field(output, "inner_shadow_rect", command.inner_shadow_rect);
            write!(output, " blur_radius={}", command.blur_radius).unwrap();
            write_field(output, "color", command.color);
        }
        DisplayListCommandType::PaintTextShadow => {
            let command = read_command::<PaintTextShadow>(payload);
            write_field(output, "shadow_rect", command.shadow_bounding_rect);
            write_field(output, "text_rect", command.text_rect);
            write_field(output, "draw_location", command.draw_location);
            write!(output, " blur_radius={}", command.blur_radius).unwrap();
            write_field(output, "color", command.color);
        }
        DisplayListCommandType::FillRectWithRoundedCorners => {
            let command = read_command::<FillRectWithRoundedCorners>(payload);
            write_field(output, "rect", command.rect);
            write_field(output, "color", command.color);
        }
        DisplayListCommandType::FillPath => {
            let command = read_command::<FillPath>(payload);
            write_field(output, "path_bounding_rect", command.path_bounding_rect);
            write_blend_mode(output, command.compositing_and_blending_operator);
        }
        DisplayListCommandType::StrokePath => {
            let command = read_command::<StrokePath>(payload);
            write_field(output, "path_bounding_rect", command.path_bounding_rect);
            write_field(output, "thickness", command.thickness);
            if command.paint_kind == PathPaintKind::Color {
                write_field(output, "color", command.color);
            }
            if !command.dash_array.is_empty() {
                write!(
                    output,
                    " dash_array_size={} dash_offset={:.2}",
                    command.dash_array.size as usize / std::mem::size_of::<f32>(),
                    command.dash_offset
                )
                .unwrap();
            }
        }
        DisplayListCommandType::DrawEllipse => {
            let command = read_command::<DrawEllipse>(payload);
            write_field(output, "rect", command.rect);
            write_field(output, "color", command.color);
            write!(output, " thickness={}", command.thickness).unwrap();
        }
        DisplayListCommandType::DrawLine => {
            let command = read_command::<DrawLine>(payload);
            write_field(output, "from", command.from);
            write_field(output, "to", command.to);
            write_field(output, "color", command.color);
            write!(
                output,
                " thickness={} style={}",
                command.thickness,
                line_style_name(command.style)
            )
            .unwrap();
            if command.style != LineStyle::Solid {
                write_field(output, "alternate_color", command.alternate_color);
            }
        }
        DisplayListCommandType::ApplyBackdropFilter => {
            let command = read_command::<ApplyBackdropFilter>(payload);
            write_field(output, "backdrop_region", command.backdrop_region);
        }
        DisplayListCommandType::DrawRect => {
            let command = read_command::<DrawRect>(payload);
            write_field(output, "rect", command.rect);
            write_field(output, "color", command.color);
            write!(output, " rough={}", command.rough).unwrap();
        }
        DisplayListCommandType::PaintNestedDisplayList => {
            let command = read_command::<PaintNestedDisplayList>(payload);
            write_field(output, "rect", command.rect);
        }
        DisplayListCommandType::DrawIsolatedDisplayList => {
            let command = read_command::<DrawIsolatedDisplayList>(payload);
            write_field(output, "rect", command.rect);
            write_field(output, "list_size", command.list_size);
            write_blend_mode(output, command.compositing_and_blending_operator);
            if command.mask_display_list_id != NO_MASK_DISPLAY_LIST {
                write!(
                    output,
                    " has_mask={}",
                    if command.mask_kind == libgfx_rust::MaskKind::Luminance {
                        "luminance"
                    } else {
                        "alpha"
                    }
                )
                .unwrap();
            }
        }
        DisplayListCommandType::CompositorScrollNode => {
            let command = read_command::<CompositorScrollNode>(payload);
            write_field(output, "scroll_node_index", command.scroll_node_index);
            write_field(output, "parent_scroll_node_index", command.parent_scroll_node_index);
            write_field(output, "scrollport_rect", command.scrollport_rect);
            write_field(output, "min_scroll_offset", command.min_scroll_offset);
            write_field(output, "max_scroll_offset", command.max_scroll_offset);
            write!(output, " is_viewport={}", command.is_viewport).unwrap();
        }
        DisplayListCommandType::CompositorBlockingWheelEventRegion => {
            let command = read_command::<CompositorBlockingWheelEventRegion>(payload);
            write_field(output, "rect", command.rect);
        }
        DisplayListCommandType::CompositorWheelHitTestTarget => {
            let command = read_command::<CompositorWheelHitTestTarget>(payload);
            write_field(output, "target_scroll_node_index", command.target_scroll_node_index);
            write_field(output, "rect", command.rect);
        }
        DisplayListCommandType::CompositorWheelHitTestTargetWithCornerRadii => {
            let command = read_command::<CompositorWheelHitTestTargetWithCornerRadii>(payload);
            write_field(output, "target_scroll_node_index", command.target_scroll_node_index);
            write_field(output, "rect", command.rect);
            if command.corner_radii.has_any_radius() {
                let radii = command.corner_radii;
                write!(
                    output,
                    " corner_radii=[{}x{},{}x{},{}x{},{}x{}]",
                    radii.top_left.horizontal_radius,
                    radii.top_left.vertical_radius,
                    radii.top_right.horizontal_radius,
                    radii.top_right.vertical_radius,
                    radii.bottom_right.horizontal_radius,
                    radii.bottom_right.vertical_radius,
                    radii.bottom_left.horizontal_radius,
                    radii.bottom_left.vertical_radius
                )
                .unwrap();
            }
        }
        DisplayListCommandType::CompositorMainThreadWheelEventRegion => {
            let command = read_command::<CompositorMainThreadWheelEventRegion>(payload);
            write_field(output, "rect", command.rect);
        }
        DisplayListCommandType::CompositorViewportScrollbar => {
            let command = read_command::<CompositorViewportScrollbar>(payload);
            write_field(output, "scroll_node_index", command.scroll_node_index);
            write_field(output, "gutter_rect", command.gutter_rect);
            write_field(output, "thumb_rect", command.thumb_rect);
            write_field(output, "expanded_gutter_rect", command.expanded_gutter_rect);
            write_field(output, "expanded_thumb_rect", command.expanded_thumb_rect);
            write!(
                output,
                " scroll_size={} expanded_scroll_size={}",
                command.scroll_size, command.expanded_scroll_size
            )
            .unwrap();
            write_field(output, "min_scroll_offset", command.min_scroll_offset);
            write_field(output, "max_scroll_offset", command.max_scroll_offset);
            write_field(output, "thumb_color", command.thumb_color);
            write_field(output, "track_color", command.track_color);
            write!(output, " vertical={}", command.vertical).unwrap();
        }
        DisplayListCommandType::PaintScrollBar => {}
    }
}

fn dump_inline_clips(output: &mut String, header: &DisplayListCommandHeader, payload: &[u8]) {
    output.push_str(" inline_clips=[");
    let count = header.inline_clip_count as usize;
    let entries_size = count * INLINE_CLIP_ENTRY_SIZE;
    assert!(entries_size <= payload.len());
    let entries_offset = payload.len() - entries_size;
    for index in 0..count {
        if index > 0 {
            output.push_str(", ");
        }
        let clip = read_command::<DisplayListInlineClip>(&payload[entries_offset + index * INLINE_CLIP_ENTRY_SIZE..]);
        if clip.kind == InlineClipKind::Path {
            let start = clip.path_data.offset as usize;
            let size = clip.path_data.size as usize;
            assert!(start <= payload.len() && size <= payload.len() - start);
            let path = OwnedPath::from_serialized_bytes(&payload[start..start + size]);
            let svg_path = path.to_svg_string();
            let has_host_dependent_curves = svg_path.contains('Q') || svg_path.contains('C');
            output.push_str("clip_path=[bounds: ");
            push_float_rect(output, clip.clip_rect_or_path_device_bounds);
            if has_host_dependent_curves {
                let command_count = svg_path
                    .bytes()
                    .filter(|byte| matches!(byte, b'M' | b'L' | b'Q' | b'C' | b'Z'))
                    .count();
                write!(output, ", curved path: {command_count} commands]").unwrap();
            } else {
                write!(output, ", path: {svg_path}]").unwrap();
            }
        } else {
            output.push_str("clip=");
            push_float_rect(output, clip.clip_rect_or_path_device_bounds);
            write_inline_clip_radii(output, clip.corner_radii);
        }
        if clip.mode == ClipMode::Difference {
            output.push_str(" mode=difference");
        }
    }
    output.push(']');
}

fn write_context(output: &mut String, context: ContextRef) {
    write_spatial_node_index(output, context.spatial);
    if !context.frame.is_none() {
        write!(output, "/f{}", context.frame.0).unwrap();
    }
}

fn write_color(output: &mut String, color: Color) {
    match color.alpha() {
        0 => write!(output, "rgba({}, {}, {}, 0)", color.red(), color.green(), color.blue()).unwrap(),
        255 => write!(output, "rgb({}, {}, {})", color.red(), color.green(), color.blue()).unwrap(),
        alpha => {
            let (digits, length) = format_to_8bit_compatible(alpha);
            let fraction = std::str::from_utf8(&digits[..length]).expect("digits are ASCII");
            write!(
                output,
                "rgba({}, {}, {}, 0.{fraction})",
                color.red(),
                color.green(),
                color.blue()
            )
            .unwrap();
        }
    }
}

fn write_blend_mode(output: &mut String, operator: CompositingAndBlendingOperator) {
    if operator != CompositingAndBlendingOperator::Normal {
        write!(output, " blend_mode={}", operator as i32).unwrap();
    }
}

fn line_style_name(style: LineStyle) -> &'static str {
    match style {
        LineStyle::Solid => "Solid",
        LineStyle::Dotted => "Dotted",
        LineStyle::Dashed => "Dashed",
    }
}

fn scaling_mode_name(mode: ScalingMode) -> &'static str {
    match mode {
        ScalingMode::None => "None",
        ScalingMode::Bilinear => "Bilinear",
        ScalingMode::BilinearMipmap => "BilinearMipmap",
        ScalingMode::NearestNeighbor => "NearestNeighbor",
    }
}

fn write_inline_clip_radii(output: &mut String, radii: CornerRadii) {
    if radii.has_any_radius() {
        write!(
            output,
            " radii=({},{},{},{})",
            radii.top_left.horizontal_radius,
            radii.top_right.horizontal_radius,
            radii.bottom_right.horizontal_radius,
            radii.bottom_left.horizontal_radius
        )
        .unwrap();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::painting::display_list::builder::DisplayListBuilder;
    use libgfx_rust::IntRect;

    #[test]
    fn command_dump_matches_the_canonical_format() {
        let mut builder = DisplayListBuilder::new();
        builder.append(
            &FillRect {
                rect: IntRect::new(1, 2, 30, 40),
                color: Color::from_rgba(101, 2, 0, 204),
                compositing_and_blending_operator: CompositingAndBlendingOperator::Multiply,
            },
            &[],
            ContextRef {
                spatial: SpatialNodeIndex(3),
                frame: FrameNodeIndex(4),
            },
        );
        let bytes = builder.bytes();
        let header = read_header(bytes);
        let payload = &bytes[HEADER_SIZE..HEADER_SIZE + header.payload_size as usize];
        let mut output = format!("{}@", header.command_type.name());
        write_context(&mut output, header.context);
        dump_command(&mut output, header.command_type, payload);
        assert_eq!(
            output,
            "FillRect@s3/f4 rect=[1,2 30x40] color=rgba(101, 2, 0, 0.8) blend_mode=2"
        );
    }
}
