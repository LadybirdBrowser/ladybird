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
use crate::painting::host::FfiNodeIdentity;
use crate::painting::node_painting;
use crate::painting::paint_read::{GeometryRead, PaintRead};
use crate::painting::paintable_data::FfiPixelBox;
use crate::painting::paintable_geometry;
use crate::stage::MainThread;
use std::ffi::c_void;

crate::render_state::held_node_entries!();

/// Mints the main thread token for this module's FFI entry points; only this module can make one.
pub(crate) struct MainThreadFfiEntry {
    _private: (),
}

const MAIN_THREAD_FFI_ENTRY: MainThreadFfiEntry = MainThreadFfiEntry { _private: () };

/// The layout root of a document nested in a row's node, which the dump hands back to the host
/// to dump, without reading it.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiNestedLayoutRoot {
    pub has_document: bool,
    pub layout_root: *mut c_void,
}

/// What a layout tree dump asks the document. The fields are private: the callbacks are reached
/// only through the methods below, which take the main thread token.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiLayoutTreeDumpCallbacks {
    context: *mut c_void,
    /// The document whose rows are dumped, which resolves the nodes they stand for.
    document: *mut c_void,
    describe_dom_node: unsafe extern "C" fn(
        document: *mut c_void,
        node: FfiNodeIdentity,
        tag_name_sink: *mut c_void,
        identifier_sink: *mut c_void,
    ),
    navigable_container_content_document: unsafe extern "C" fn(
        document: *mut c_void,
        node: FfiNodeIdentity,
        url_sink: *mut c_void,
    ) -> FfiNestedLayoutRoot,
    svg_as_image_layout_root: unsafe extern "C" fn(document: *mut c_void, node: FfiNodeIdentity) -> *mut c_void,
    dump_nested_layout_tree: unsafe extern "C" fn(
        context: *mut c_void,
        layout_root: *mut c_void,
        indent: usize,
        interactive: bool,
        output_sink: *mut c_void,
    ),
    append_text: unsafe extern "C" fn(context: *mut c_void, bytes: *const u8, byte_count: usize),
}

impl FfiLayoutTreeDumpCallbacks {
    fn describe_dom_node(
        &self,
        _: &MainThread,
        node: FfiNodeIdentity,
        tag_name_sink: &mut Vec<u8>,
        identifier_sink: &mut Vec<u8>,
    ) {
        // SAFETY: The C++ host fills both sinks synchronously through the exported push function.
        unsafe {
            (self.describe_dom_node)(
                self.document,
                node,
                (&raw mut *tag_name_sink).cast(),
                (&raw mut *identifier_sink).cast(),
            );
        }
    }

    fn navigable_container_content_document(
        &self,
        _: &MainThread,
        node: FfiNodeIdentity,
    ) -> Option<(Vec<u8>, *mut c_void)> {
        let mut url = Vec::new();
        // SAFETY: The C++ host fills the url sink synchronously through the exported push function.
        let nested = unsafe { (self.navigable_container_content_document)(self.document, node, (&raw mut url).cast()) };
        nested.has_document.then_some((url, nested.layout_root))
    }

    fn svg_as_image_layout_root(&self, _: &MainThread, node: FfiNodeIdentity) -> *mut c_void {
        // SAFETY: The C++ host answers synchronously.
        unsafe { (self.svg_as_image_layout_root)(self.document, node) }
    }

    fn dump_nested_layout_tree(
        &self,
        _: &MainThread,
        layout_root: *mut c_void,
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
                layout_root,
                indent,
                interactive,
                (&raw mut *output).cast(),
            );
        }
    }

    fn append_text(&self, _: &MainThread, bytes: &[u8]) {
        // SAFETY: The C++ sink copies the completed dump synchronously.
        unsafe { (self.append_text)(self.context, bytes.as_ptr(), bytes.len()) };
    }
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread. The host callbacks fill their sinks synchronously
/// through `layout_arena_paint_push_bytes`; `dump_nested_layout_tree` may re-enter this function for a different
/// document.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_dump_layout_tree(
    host: &crate::render_state::DocumentHost,
    root: NodeSlotId,
    initial_indent: usize,
    interactive: bool,
    callbacks: FfiLayoutTreeDumpCallbacks,
) {
    // SAFETY: Guaranteed by the entry point's contract.
    let main_thread = unsafe { crate::stage::from_ffi_entry(&MAIN_THREAD_FFI_ENTRY, host) };
    // SAFETY: As above.
    let plan = unsafe {
        crate::painting::ffi::read_arena(
            host,
            node_read(),
            (root, initial_indent, interactive),
            |arena, (root, initial_indent, interactive)| {
                arena.measure_scrollable_overflow();
                let mut plan = DumpPlan::default();
                plan_layout_node(
                    &mut plan,
                    arena,
                    root,
                    initial_indent,
                    &DumpPalette::new(interactive),
                    interactive,
                );
                plan
            },
        )
    };
    let mut output = Vec::new();
    plan.write(&mut output, &callbacks, &main_thread, interactive);
    callbacks.append_text(&main_thread, &output);
}

/// What a layout tree dump writes: the text the rows answer, and at points in it what only the host can say.
#[derive(Default)]
struct DumpPlan {
    text: Vec<u8>,
    /// Each point in `text` where the host says something, in order.
    host_parts: Vec<(usize, HostPart)>,
}

/// What the host says at a point of a layout tree dump.
enum HostPart {
    /// The tag name the host describes the node with, then the plan's text up to `identifier_at`, then the node's
    /// identifier.
    DomNode {
        node: FfiNodeIdentity,
        identifier_at: usize,
    },
    /// The URL and the layout tree of the document a navigable container shows, if it shows one.
    NavigableContent { node: FfiNodeIdentity, indent: usize },
    /// The isolated layout tree of the SVG-as-image the node shows, if it shows one.
    SvgAsImage { node: FfiNodeIdentity, indent: usize },
}

impl DumpPlan {
    /// Writes the dump to `output`, asking the host what only it can say where the plan says so.
    fn write(
        self,
        output: &mut Vec<u8>,
        callbacks: &FfiLayoutTreeDumpCallbacks,
        main_thread: &MainThread,
        interactive: bool,
    ) {
        let mut written = 0;
        for (at, part) in self.host_parts {
            output.extend_from_slice(&self.text[written..at]);
            written = at;
            match part {
                HostPart::DomNode { node, identifier_at } => {
                    let mut identifier = Vec::new();
                    callbacks.describe_dom_node(main_thread, node, output, &mut identifier);
                    output.extend_from_slice(&self.text[at..identifier_at]);
                    output.extend_from_slice(&identifier);
                    written = identifier_at;
                }
                HostPart::NavigableContent { node, indent } => {
                    let Some((url, nested_layout_root_shell)) =
                        callbacks.navigable_container_content_document(main_thread, node)
                    else {
                        continue;
                    };
                    output.extend_from_slice(b" (url: ");
                    output.extend_from_slice(&url);
                    output.extend_from_slice(b")\n");
                    if !nested_layout_root_shell.is_null() {
                        callbacks.dump_nested_layout_tree(
                            main_thread,
                            nested_layout_root_shell,
                            indent + 1,
                            interactive,
                            output,
                        );
                    }
                }
                HostPart::SvgAsImage { node, indent } => {
                    let svg_as_image_layout_root = callbacks.svg_as_image_layout_root(main_thread, node);
                    if !svg_as_image_layout_root.is_null() {
                        push_indent(output, indent + 1);
                        output.extend_from_slice(b"(SVG-as-image isolated context)\n");
                        callbacks.dump_nested_layout_tree(
                            main_thread,
                            svg_as_image_layout_root,
                            indent + 1,
                            interactive,
                            output,
                        );
                    }
                }
            }
        }
        output.extend_from_slice(&self.text[written..]);
    }
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
    if let Some(name) = formatting_context_type_created_by_node_data(
        data,
        arena.node_style_if_live(slot),
        crate::layout::node_facts::node_is_flex_or_grid_container(parent_style),
    )
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
    plan: &mut DumpPlan,
    arena: &LayoutNodeArena,
    data: &NodeData,
    slot: NodeSlotId,
    palette: &DumpPalette,
) {
    let output = &mut plan.text;
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
    let described = if node_facts::has_flag(data, NodeFlag::Anonymous) {
        output.extend_from_slice(b"(anonymous)");
        None
    } else {
        Some((
            output.len(),
            crate::painting::hit_test::resolve::row_node_identity(arena, slot, false),
        ))
    };
    output.extend_from_slice(if is_box { palette.off } else { "" }.as_bytes());
    output.extend_from_slice(identifier_color.as_bytes());
    if let Some((at, node)) = described {
        let identifier_at = output.len();
        plan.host_parts.push((at, HostPart::DomNode { node, identifier_at }));
    }
    let output = &mut plan.text;
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

/// Plans the dump of the row in `slot` and its subtree, indented by `indent`.
fn plan_layout_node(
    plan: &mut DumpPlan,
    arena: &LayoutNodeArena,
    slot: NodeSlotId,
    indent: usize,
    palette: &DumpPalette,
    interactive: bool,
) {
    let Some(data) = arena.node_data_if_live(slot) else {
        return;
    };
    push_indent(&mut plan.text, indent);
    push_layout_node_line(plan, arena, data, slot, palette);
    let kind = data.kind.get();
    // The host reads the row's DOM node, which the row names by identity.
    let layout_node = arena
        .node_is_dom_backed(slot)
        .then(|| crate::painting::hit_test::resolve::row_node_identity(arena, slot, false));
    if let Some(node) = layout_node.filter(|_| kind == NodeKind::NavigableContainerViewport) {
        plan.host_parts
            .push((plan.text.len(), HostPart::NavigableContent { node, indent }));
    }
    let has_committed_box = arena.paintable_row_is_populated(slot);
    let rows = arena.paintable_rows();
    if paintable_geometry::has_scrollable_overflow(&rows, slot)
        && let Some(rect) = paintable_geometry::scrollable_overflow_rect(&rows, slot)
    {
        plan.text.extend_from_slice(b" overflow: ");
        push_css_pixel_rect(&mut Utf8Sink(&mut plan.text), rect);
    }
    plan.text.push(b'\n');
    if let Some(node) = layout_node {
        plan.host_parts
            .push((plan.text.len(), HostPart::SvgAsImage { node, indent }));
    }
    if has_committed_box
        && node_facts::kind_is_block_container(kind)
        && node_facts::has_flag(data, NodeFlag::ChildrenAreInline)
        && node_painting::has_lines(arena, slot)
    {
        dump_block_fragments(&mut plan.text, &rows, slot, indent, interactive);
    }
    if has_committed_box && node_painting::is_fragmented_inline(arena, slot) {
        dump_inline_piece_fragments(&mut plan.text, &rows, slot, indent, interactive);
    }
    for child in arena.children(slot) {
        plan_layout_node(plan, arena, child, indent + 1, palette, interactive);
    }
}
