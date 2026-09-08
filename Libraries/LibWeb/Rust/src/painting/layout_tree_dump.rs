/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_enums::{flex_direction, positioning};
use crate::layout::LayoutNodeArena;
use crate::layout::formatting_context::{FormattingContextType, formatting_context_type_created_by_node_data};
use crate::layout::node_data::{NodeData, NodeFlag, NodeKind, NodeSlotId};
use crate::layout::node_facts;
use crate::layout::svg_formatting_context::FfiAffineTransform;
use crate::painting::dump::{
    Utf8Sink, dump_block_fragments, dump_inline_piece_fragments, format_float_like_ak, push_class_name,
    push_css_pixel_point, push_css_pixel_rect, push_css_pixels, push_indent,
};
use crate::painting::ffi::{arena_from_handle, scrollable_overflow_rect_measuring_if_missing};
use crate::painting::host::FfiScrollableOverflowHostCallbacks;
use crate::painting::host::visual_context::FfiVisualContextHostCallbacks;
use crate::painting::node_painting;
use crate::painting::paintable_data::FfiPixelBox;
use crate::painting::paintable_geometry;
use std::ffi::c_void;

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiNestedLayoutRoot {
    pub has_document: bool,
    pub layout_root_shell: *mut c_void,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiLayoutTreeDumpCallbacks {
    pub context: *mut c_void,
    pub describe_dom_node: unsafe extern "C" fn(
        context: *mut c_void,
        dom_node: *mut c_void,
        tag_name_sink: *mut c_void,
        identifier_sink: *mut c_void,
    ),
    pub navigable_container_content_document:
        unsafe extern "C" fn(context: *mut c_void, dom_node: *mut c_void, url_sink: *mut c_void) -> FfiNestedLayoutRoot,
    pub svg_as_image_layout_root: unsafe extern "C" fn(context: *mut c_void, dom_node: *mut c_void) -> *mut c_void,
    pub dump_nested_layout_tree: unsafe extern "C" fn(
        context: *mut c_void,
        layout_root_shell: *mut c_void,
        indent: usize,
        interactive: bool,
        output_sink: *mut c_void,
    ),
    pub append_text: unsafe extern "C" fn(context: *mut c_void, bytes: *const u8, byte_count: usize),
    pub visual_context: FfiVisualContextHostCallbacks,
    pub scrollable_overflow: FfiScrollableOverflowHostCallbacks,
}

impl FfiLayoutTreeDumpCallbacks {
    fn describe_dom_node(&self, dom_node: *mut c_void, tag_name_sink: &mut Vec<u8>, identifier_sink: &mut Vec<u8>) {
        // SAFETY: The C++ host fills both sinks synchronously through the exported push function.
        unsafe {
            (self.describe_dom_node)(
                self.context,
                dom_node,
                (&raw mut *tag_name_sink).cast(),
                (&raw mut *identifier_sink).cast(),
            );
        }
    }

    fn navigable_container_content_document(&self, dom_node: *mut c_void) -> Option<(Vec<u8>, *mut c_void)> {
        let mut url = Vec::new();
        // SAFETY: The C++ host fills the url sink synchronously through the exported push function.
        let nested =
            unsafe { (self.navigable_container_content_document)(self.context, dom_node, (&raw mut url).cast()) };
        nested.has_document.then_some((url, nested.layout_root_shell))
    }

    fn svg_as_image_layout_root(&self, dom_node: *mut c_void) -> *mut c_void {
        // SAFETY: The C++ host answers synchronously from a live DOM node.
        unsafe { (self.svg_as_image_layout_root)(self.context, dom_node) }
    }

    fn dump_nested_layout_tree(
        &self,
        layout_root_shell: *mut c_void,
        indent: usize,
        interactive: bool,
        output: &mut Vec<u8>,
    ) {
        // SAFETY: The C++ host re-enters the dump for the nested document's own arena and appends
        // into the output sink synchronously through the exported push function; no reference to
        // the output vector is alive across the call.
        unsafe {
            (self.dump_nested_layout_tree)(
                self.context,
                layout_root_shell,
                indent,
                interactive,
                (&raw mut *output).cast(),
            );
        }
    }

    fn append_text(&self, bytes: &[u8]) {
        // SAFETY: The C++ sink copies the completed dump synchronously.
        unsafe { (self.append_text)(self.context, bytes.as_ptr(), bytes.len()) };
    }
}

/// # Safety
///
/// `arena` must be a live handle from `layout_arena_create`, used on the document thread, with no
/// outstanding borrows of the arena. The host callbacks fill their sinks synchronously through
/// `layout_arena_paint_push_bytes`; `dump_nested_layout_tree` may re-enter this function for a
/// different document's arena.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_dump_layout_tree(
    arena: *mut c_void,
    root: NodeSlotId,
    viewport: NodeSlotId,
    initial_indent: usize,
    interactive: bool,
    callbacks: FfiLayoutTreeDumpCallbacks,
) {
    let context = LayoutTreeDumpContext {
        arena_handle: arena,
        viewport,
        interactive,
        palette: DumpPalette::new(interactive),
        callbacks: &callbacks,
    };
    let mut output = Vec::new();
    // SAFETY: Guaranteed by the caller.
    unsafe {
        dump_layout_node(&mut output, &context, root, initial_indent);
    }
    callbacks.append_text(&output);
}

struct LayoutTreeDumpContext<'a> {
    arena_handle: *mut c_void,
    viewport: NodeSlotId,
    interactive: bool,
    palette: DumpPalette,
    callbacks: &'a FfiLayoutTreeDumpCallbacks,
}

struct DumpPalette {
    nonbox: &'static str,
    box_: &'static str,
    svg_box: &'static str,
    positioned: &'static str,
    floating: &'static str,
    inline: &'static str,
    flex: &'static str,
    table: &'static str,
    formatting_context: &'static str,
    off: &'static str,
}

impl DumpPalette {
    fn new(interactive: bool) -> Self {
        let color = |code: &'static str| if interactive { code } else { "" };
        Self {
            nonbox: color("\x1b[33m"),
            box_: color("\x1b[34m"),
            svg_box: color("\x1b[31m"),
            positioned: color("\x1b[31;1m"),
            floating: color("\x1b[32;1m"),
            inline: color("\x1b[36;1m"),
            flex: color("\x1b[34;1m"),
            table: color("\x1b[91;1m"),
            formatting_context: color("\x1b[37;1m"),
            off: color("\x1b[0m"),
        }
    }
}

fn formatting_context_name(formatting_context_type: FormattingContextType) -> Option<&'static str> {
    match formatting_context_type {
        FormattingContextType::Block => Some("BFC"),
        FormattingContextType::Flex => Some("FFC"),
        FormattingContextType::Grid => Some("GFC"),
        FormattingContextType::Table => Some("TFC"),
        FormattingContextType::Svg => Some("SVG"),
        _ => None,
    }
}

fn push_box_model_axis(output: &mut Vec<u8>, edges: [crate::css::css_pixels::CssPixels; 7]) {
    let mut sink = Utf8Sink(output);
    for (separator, edge) in ["", "+", "+", " ", " ", "+", "+"].into_iter().zip(edges) {
        sink.0.extend_from_slice(separator.as_bytes());
        push_css_pixels(&mut sink, edge);
    }
}

fn push_box_model(output: &mut Vec<u8>, arena: &LayoutNodeArena, slot: NodeSlotId) {
    let margin: FfiPixelBox = paintable_geometry::committed_margin(arena, slot);
    let border = paintable_geometry::committed_border(arena, slot);
    let padding = paintable_geometry::committed_padding(arena, slot);
    let content_size = paintable_geometry::committed_content_size(&arena.paintable_rows(), slot);
    output.extend_from_slice(b" [");
    push_box_model_axis(
        output,
        [
            margin.left,
            border.left,
            padding.left,
            content_size.width,
            padding.right,
            border.right,
            margin.right,
        ],
    );
    output.extend_from_slice(b"] [");
    push_box_model_axis(
        output,
        [
            margin.top,
            border.top,
            padding.top,
            content_size.height,
            padding.bottom,
            border.bottom,
            margin.bottom,
        ],
    );
    output.push(b']');
}

fn push_position_and_box_model(
    output: &mut Vec<u8>,
    arena: &LayoutNodeArena,
    slot: NodeSlotId,
    has_committed_box: bool,
) {
    if !has_committed_box {
        output.extend_from_slice(b"(not painted)");
        return;
    }
    output.extend_from_slice(b"at ");
    push_css_pixel_point(
        &mut Utf8Sink(output),
        paintable_geometry::absolute_position(&arena.paintable_rows(), slot),
    );
    push_box_model(output, arena, slot);
}

fn push_affine_transform(output: &mut Vec<u8>, transform: FfiAffineTransform) {
    let components = [
        transform.a,
        transform.b,
        transform.c,
        transform.d,
        transform.e,
        transform.f,
    ];
    output.push(b'[');
    output.extend_from_slice(components.map(format_float_like_ak).join(" ").as_bytes());
    output.push(b']');
}

fn push_box_tokens(
    output: &mut Vec<u8>,
    arena: &LayoutNodeArena,
    data: &NodeData,
    slot: NodeSlotId,
    palette: &DumpPalette,
) {
    let style = arena.node_style_if_live(slot);
    let display = node_facts::node_display(style);
    let flex_direction_name = flex_direction::NAMES
        .get(style.map_or(flex_direction::ROW, |style| style.flex_direction()) as usize)
        .copied()
        .unwrap_or("");
    let tokens: [(bool, &str, &[&str]); 13] = [
        (
            node_facts::node_position(style) != positioning::STATIC,
            palette.positioned,
            &["positioned"],
        ),
        (
            node_facts::node_is_floating(data, style),
            palette.floating,
            &["floating"],
        ),
        (display.is_inline_block(), palette.inline, &["inline-block"]),
        (
            display.is_inline_outside() && display.is_table_inside(),
            palette.inline,
            &["inline-table"],
        ),
        (
            display.is_flex_inside(),
            palette.flex,
            &["flex-container(", flex_direction_name, ")"],
        ),
        (
            node_facts::has_flag(data, NodeFlag::IsFlexItem),
            palette.flex,
            &["flex-item"],
        ),
        (display.is_table_inside(), palette.table, &["table-box"]),
        (display.is_table_row_group(), palette.table, &["table-row-group"]),
        (display.is_table_column_group(), palette.table, &["table-column-group"]),
        (display.is_table_header_group(), palette.table, &["table-header-group"]),
        (display.is_table_footer_group(), palette.table, &["table-footer-group"]),
        (display.is_table_row(), palette.table, &["table-row"]),
        (display.is_table_cell(), palette.table, &["table-cell"]),
    ];
    for (is_set, color_on, parts) in tokens {
        if !is_set {
            continue;
        }
        output.push(b' ');
        output.extend_from_slice(color_on.as_bytes());
        for part in parts {
            output.extend_from_slice(part.as_bytes());
        }
        output.extend_from_slice(palette.off.as_bytes());
    }
}

fn push_box_suffix(
    output: &mut Vec<u8>,
    arena: &LayoutNodeArena,
    data: &NodeData,
    slot: NodeSlotId,
    has_committed_box: bool,
    palette: &DumpPalette,
) {
    if has_committed_box && let Some(transform) = paintable_geometry::committed_svg_viewport_transform(arena, slot) {
        output.extend_from_slice(b" viewport-transform=");
        push_affine_transform(output, transform);
    }
    let parent_style = arena
        .node_parent_if_live(slot)
        .and_then(|parent| arena.node_style_if_live(parent));
    if let Some(name) = formatting_context_type_created_by_node_data(data, arena.node_style_if_live(slot), parent_style)
        .and_then(formatting_context_name)
    {
        output.extend_from_slice(b" [");
        output.extend_from_slice(palette.formatting_context.as_bytes());
        output.extend_from_slice(name.as_bytes());
        output.extend_from_slice(palette.off.as_bytes());
        output.push(b']');
    }
    output.extend_from_slice(if node_facts::has_flag(data, NodeFlag::ChildrenAreInline) {
        b" children: inline"
    } else {
        b" children: not-inline"
    });
}

fn push_layout_node_line(
    output: &mut Vec<u8>,
    arena: &LayoutNodeArena,
    data: &NodeData,
    slot: NodeSlotId,
    context: &LayoutTreeDumpContext<'_>,
) {
    let palette = &context.palette;
    let kind = data.kind.get();
    let is_box = node_facts::kind_is_box(kind);
    let has_committed_box = arena.paintable_row_is_populated(slot);
    let class_color = match (is_box, node_facts::kind_is_svg_box(kind)) {
        (false, _) => palette.nonbox,
        (true, false) => palette.box_,
        (true, true) => palette.svg_box,
    };
    let tag_name_color = if is_box { class_color } else { "" };
    let identifier_color = if is_box { "" } else { class_color };

    output.extend_from_slice(class_color.as_bytes());
    push_class_name(output, kind);
    output.extend_from_slice(palette.off.as_bytes());
    output.extend_from_slice(b" <");
    output.extend_from_slice(tag_name_color.as_bytes());
    let mut identifier = Vec::new();
    if node_facts::has_flag(data, NodeFlag::Anonymous) {
        output.extend_from_slice(b"(anonymous)");
    } else {
        context
            .callbacks
            .describe_dom_node(arena.node_dom_node(slot), output, &mut identifier);
    }
    output.extend_from_slice(if is_box { palette.off } else { "" }.as_bytes());
    output.extend_from_slice(identifier_color.as_bytes());
    output.extend_from_slice(&identifier);
    output.extend_from_slice(if is_box { "" } else { palette.off }.as_bytes());
    output.extend_from_slice(b"> ");

    if !is_box {
        push_position_and_box_model(output, arena, slot, has_committed_box);
        return;
    }
    if has_committed_box {
        output.extend_from_slice(b"at ");
        push_css_pixel_point(
            &mut Utf8Sink(output),
            paintable_geometry::absolute_position(&arena.paintable_rows(), slot),
        );
    } else {
        output.extend_from_slice(b"(not painted)");
    }
    push_box_tokens(output, arena, data, slot, palette);
    if has_committed_box {
        push_box_model(output, arena, slot);
    }
    push_box_suffix(output, arena, data, slot, has_committed_box, palette);
}

/// # Safety
///
/// The context's arena handle must be live with no outstanding borrows; see
/// `layout_arena_dump_layout_tree`.
unsafe fn dump_layout_node(output: &mut Vec<u8>, context: &LayoutTreeDumpContext<'_>, slot: NodeSlotId, indent: usize) {
    push_indent(output, indent);

    let (dom_node, nested_navigable_document, dumps_block_fragments, dumps_inline_piece_fragments) = {
        // SAFETY: Guaranteed by the caller; the borrow ends with this block.
        let arena = unsafe { arena_from_handle(context.arena_handle) };
        let Some(data) = arena.node_data_if_live(slot) else {
            return;
        };
        push_layout_node_line(output, arena, data, slot, context);
        let kind = data.kind.get();
        let dom_node = arena.node_dom_node(slot);
        let nested_navigable_document = (kind == NodeKind::NavigableContainerViewport)
            .then(|| context.callbacks.navigable_container_content_document(dom_node))
            .flatten();
        let has_committed_box = arena.paintable_row_is_populated(slot);
        let dumps_block_fragments = has_committed_box
            && node_facts::kind_is_block_container(kind)
            && node_facts::has_flag(data, NodeFlag::ChildrenAreInline)
            && node_painting::has_lines(arena, slot);
        let dumps_inline_piece_fragments = has_committed_box && node_painting::is_fragmented_inline(arena, slot);
        (
            dom_node,
            nested_navigable_document,
            dumps_block_fragments,
            dumps_inline_piece_fragments,
        )
    };

    if let Some((url, nested_layout_root_shell)) = nested_navigable_document {
        output.extend_from_slice(b" (url: ");
        output.extend_from_slice(&url);
        output.extend_from_slice(b")\n");
        if !nested_layout_root_shell.is_null() {
            context.callbacks.dump_nested_layout_tree(
                nested_layout_root_shell,
                indent + 1,
                context.interactive,
                output,
            );
        }
    }

    // SAFETY: No arena borrow is alive here.
    let scrollable_overflow_rect = unsafe {
        scrollable_overflow_rect_measuring_if_missing(
            context.arena_handle,
            slot,
            context.viewport,
            &context.callbacks.visual_context,
            &context.callbacks.scrollable_overflow,
        )
    };
    if let Some(rect) = scrollable_overflow_rect {
        output.extend_from_slice(b" overflow: ");
        push_css_pixel_rect(&mut Utf8Sink(output), rect);
    }
    output.push(b'\n');

    if !dom_node.is_null() {
        let svg_as_image_layout_root = context.callbacks.svg_as_image_layout_root(dom_node);
        if !svg_as_image_layout_root.is_null() {
            push_indent(output, indent + 1);
            output.extend_from_slice(b"(SVG-as-image isolated context)\n");
            context.callbacks.dump_nested_layout_tree(
                svg_as_image_layout_root,
                indent + 1,
                context.interactive,
                output,
            );
        }
    }

    let mut child = {
        // SAFETY: No arena borrow is alive here; this one ends with the block.
        let arena = unsafe { arena_from_handle(context.arena_handle) };
        let rows = arena.paintable_rows();
        if dumps_block_fragments {
            dump_block_fragments(output, &rows, slot, indent, context.interactive);
        }
        if dumps_inline_piece_fragments {
            dump_inline_piece_fragments(output, &rows, slot, indent, context.interactive);
        }
        arena.node_first_child_if_live(slot)
    };
    while let Some(current) = child {
        // SAFETY: Guaranteed by the caller; no borrow is alive across the recursion.
        unsafe {
            dump_layout_node(output, context, current, indent + 1);
        }
        // SAFETY: The recursion left no borrow alive.
        child = unsafe { arena_from_handle(context.arena_handle) }.node_next_sibling_if_live(current);
    }
}
