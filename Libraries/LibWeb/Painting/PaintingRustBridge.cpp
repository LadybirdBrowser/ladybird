/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <AK/StringBuilder.h>
#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListCommand.h>
#include <LibCompositing/DisplayList/DisplayListResourceStorage.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/Environment.h>
#include <LibGfx/CornerRadii.h>
#include <LibGfx/Filter.h>
#include <LibGfx/GradientInterpolation.h>
#include <LibGfx/Matrix4x4.h>
#include <LibGfx/Path.h>
#include <LibGfx/TextLayout.h>
#include <LibWeb/Animations/DocumentTimeline.h>
#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/AbstractImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorStyleValue.h>
#include <LibWeb/CSS/VisualViewport.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/ImageRequest.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/ImageProvider.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Page/EventHandler.h>
#include <LibWeb/Page/MiddleButtonScrollHandler.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/Blending.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/ChromeMetrics.h>
#include <LibWeb/Painting/ChromeWidget.h>
#include <LibWeb/Painting/DocumentPaintState.h>
#include <LibWeb/Painting/ImagePaint.h>
#include <LibWeb/Painting/PaintFacts.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/Painting/ResizeHandle.h>
#include <LibWeb/Painting/ScrollSnap.h>
#include <LibWeb/Painting/Scrollbar.h>
#include <LibWeb/Painting/Scrolling.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWeb/SVG/SVGClipPathElement.h>
#include <LibWeb/SVG/SVGDecodedImageData.h>
#include <LibWeb/SVG/SVGFilterElement.h>
#include <LibWeb/SVG/SVGGradientElement.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/SVG/SVGImageElement.h>
#include <LibWeb/SVG/SVGMaskElement.h>
#include <LibWebCommon/CSS/SystemColor.h>

namespace Web::Painting {

static_assert(to_underlying(CSS::FontSmoothing::Auto) == to_underlying(Compositing::FontSmoothing::Auto));
static_assert(to_underlying(CSS::FontSmoothing::None) == to_underlying(Compositing::FontSmoothing::None));
static_assert(to_underlying(CSS::FontSmoothing::Antialiased) == to_underlying(Compositing::FontSmoothing::Antialiased));
static_assert(to_underlying(CSS::FontSmoothing::SubpixelAntialiased) == to_underlying(Compositing::FontSmoothing::SubpixelAntialiased));

static_assert(sizeof(Layout::RustFFI::ScrollDirection) == sizeof(ScrollDirection));
static_assert(to_underlying(Layout::RustFFI::ScrollDirection::Horizontal) == to_underlying(ScrollDirection::Horizontal));
static_assert(to_underlying(Layout::RustFFI::ScrollDirection::Vertical) == to_underlying(ScrollDirection::Vertical));

namespace {

template<typename T>
struct RustOptionalLayout {
    T value;
    bool has_value;
};

}

static_assert(sizeof(CSSPixelRect) == 16);

static_assert(sizeof(RustOptionalLayout<CSSPixels>) == sizeof(Optional<CSSPixels>));
static_assert(alignof(RustOptionalLayout<CSSPixels>) == alignof(Optional<CSSPixels>));
static_assert(sizeof(RustOptionalLayout<CSSPixelRect>) == sizeof(Optional<CSSPixelRect>));
static_assert(alignof(RustOptionalLayout<CSSPixelRect>) == alignof(Optional<CSSPixelRect>));
static_assert(sizeof(RustOptionalLayout<Gfx::IntRect>) == sizeof(Optional<Gfx::IntRect>));
static_assert(alignof(RustOptionalLayout<Gfx::IntRect>) == alignof(Optional<Gfx::IntRect>));
static_assert(sizeof(RustOptionalLayout<float>) == sizeof(Optional<float>));
static_assert(alignof(RustOptionalLayout<float>) == alignof(Optional<float>));
static_assert(sizeof(RustOptionalLayout<Gfx::FloatPoint>) == sizeof(Optional<Gfx::FloatPoint>));
static_assert(alignof(RustOptionalLayout<Gfx::FloatPoint>) == alignof(Optional<Gfx::FloatPoint>));
static_assert(sizeof(RustOptionalLayout<Gfx::FloatSize>) == sizeof(Optional<Gfx::FloatSize>));
static_assert(alignof(RustOptionalLayout<Gfx::FloatSize>) == alignof(Optional<Gfx::FloatSize>));
static_assert(sizeof(RustOptionalLayout<i64>) == sizeof(Optional<i64>));
static_assert(alignof(RustOptionalLayout<i64>) == alignof(Optional<i64>));
static_assert(sizeof(RustOptionalLayout<size_t>) == sizeof(Optional<size_t>));
static_assert(alignof(RustOptionalLayout<size_t>) == alignof(Optional<size_t>));

static_assert(sizeof(Optional<CSSPixels>) == 8);
static_assert(alignof(Optional<CSSPixels>) == 4);

static_assert(sizeof(Compositing::ClipMode) == sizeof(u8));
static_assert(to_underlying(Compositing::ClipMode::Intersect) == 0);
static_assert(to_underlying(Compositing::ClipMode::Difference) == 1);

#define VERIFY_SHARED_FFI_TYPE(type) static_assert(IsTriviallyCopyable<type>)
VERIFY_SHARED_FFI_TYPE(CSSPixels);
VERIFY_SHARED_FFI_TYPE(CSSPixelPoint);
VERIFY_SHARED_FFI_TYPE(CSSPixelSize);
VERIFY_SHARED_FFI_TYPE(CSSPixelRect);
VERIFY_SHARED_FFI_TYPE(Gfx::IntPoint);
VERIFY_SHARED_FFI_TYPE(Gfx::FloatPoint);
VERIFY_SHARED_FFI_TYPE(Gfx::IntSize);
VERIFY_SHARED_FFI_TYPE(Gfx::FloatSize);
VERIFY_SHARED_FFI_TYPE(Gfx::FloatVector3);
VERIFY_SHARED_FFI_TYPE(Gfx::IntRect);
VERIFY_SHARED_FFI_TYPE(Gfx::FloatRect);
VERIFY_SHARED_FFI_TYPE(Gfx::Color);
VERIFY_SHARED_FFI_TYPE(Gfx::AffineTransform);
VERIFY_SHARED_FFI_TYPE(Gfx::FloatMatrix4x4);
VERIFY_SHARED_FFI_TYPE(Gfx::CornerRadius);
VERIFY_SHARED_FFI_TYPE(Gfx::CornerRadii);
VERIFY_SHARED_FFI_TYPE(Gfx::GradientInterpolationMethod);
VERIFY_SHARED_FFI_TYPE(Gfx::WindingRule);
VERIFY_SHARED_FFI_TYPE(Gfx::MaskKind);
VERIFY_SHARED_FFI_TYPE(Gfx::CompositingAndBlendingOperator);
VERIFY_SHARED_FFI_TYPE(Gfx::ScalingMode);
VERIFY_SHARED_FFI_TYPE(Gfx::InterpolationColorSpace);
VERIFY_SHARED_FFI_TYPE(Compositing::ClipMode);
VERIFY_SHARED_FFI_TYPE(ChromeMetrics);
static_assert(sizeof(ChromeMetrics) == 7 * sizeof(CSSPixels));
static_assert(offsetof(ChromeMetrics, scroll_thumb_min_length) == 0);
static_assert(offsetof(ChromeMetrics, scroll_thumb_padding_thin) == sizeof(CSSPixels));
static_assert(offsetof(ChromeMetrics, scroll_thumb_thickness_thin) == 2 * sizeof(CSSPixels));
static_assert(offsetof(ChromeMetrics, scroll_thumb_thickness) == 3 * sizeof(CSSPixels));
static_assert(offsetof(ChromeMetrics, scroll_gutter_thickness) == 4 * sizeof(CSSPixels));
static_assert(offsetof(ChromeMetrics, resize_gripper_size) == 5 * sizeof(CSSPixels));
static_assert(offsetof(ChromeMetrics, resize_gripper_padding) == 6 * sizeof(CSSPixels));
VERIFY_SHARED_FFI_TYPE(Optional<CSSPixels>);
VERIFY_SHARED_FFI_TYPE(Optional<CSSPixelRect>);
VERIFY_SHARED_FFI_TYPE(Optional<Gfx::IntRect>);
VERIFY_SHARED_FFI_TYPE(Optional<Gfx::FloatPoint>);
VERIFY_SHARED_FFI_TYPE(Optional<Gfx::FloatSize>);
VERIFY_SHARED_FFI_TYPE(Optional<Gfx::FloatRect>);
VERIFY_SHARED_FFI_TYPE(Optional<Gfx::Color>);
VERIFY_SHARED_FFI_TYPE(Optional<Gfx::AffineTransform>);
VERIFY_SHARED_FFI_TYPE(Optional<i64>);
VERIFY_SHARED_FFI_TYPE(Optional<size_t>);
VERIFY_SHARED_FFI_TYPE(Optional<u32>);
VERIFY_SHARED_FFI_TYPE(Optional<float>);
#undef VERIFY_SHARED_FFI_TYPE

namespace {

static bool rust_painting_timing_enabled()
{
    static bool enabled = [] {
        auto value = Core::Environment::get("LADYBIRD_RUST_PAINTING_TIMING"sv);
        return value.has_value() && !value->is_empty() && *value != "0"sv;
    }();
    return enabled;
}

static Layout::NodeWithStyle::ImageObserver const* layer_image_observer(Layout::NodeWithStyle const& layout_node, Layout::RustFFI::FfiLayerImageList list, u32 computed_index)
{
    switch (list) {
    case Layout::RustFFI::FfiLayerImageList::Background:
        return layout_node.background_image_observer(computed_index);
    case Layout::RustFFI::FfiLayerImageList::Mask:
        return layout_node.mask_image_observer(computed_index);
    case Layout::RustFFI::FfiLayerImageList::BorderImageSource:
        return layout_node.border_image_source_observer();
    }
    VERIFY_NOT_REACHED();
}

// The render side draws into a viewport it never asks about: every pass that reads it is handed this.
static Compositing::RustFFI::FfiVisualContextTreeInputs visual_context_tree_inputs(DOM::Document& document)
{
    Compositing::RustFFI::FfiVisualContextTreeInputs inputs {};
    inputs.device_pixels_per_css_pixel = document.page().client().device_pixels_per_css_pixel();
    auto const& visual_viewport = *document.visual_viewport();
    auto offset = visual_viewport.offset().to_type<double>();
    inputs.visual_viewport_offset_x = offset.x();
    inputs.visual_viewport_offset_y = offset.y();
    inputs.visual_viewport_scale = visual_viewport.scale();
    return inputs;
}

}

Optional<Gfx::Filter> filter_from_functions(ReadonlySpan<Compositing::RustFFI::FfiFilterFunction> functions)
{
    ByteBuffer serialized_filter;
    bool has_filter = Layout::RustFFI::layout_arena_filter_functions_serialize(
        functions.data(),
        functions.size(),
        [](void* context, u8 const* bytes, size_t length) {
            static_cast<ByteBuffer*>(context)->append(bytes, length);
        },
        &serialized_filter);
    if (!has_filter)
        return {};
    return Gfx::Filter { move(serialized_filter) };
}

static Layout::RustFFI::DocumentHost* document_host(DOM::Document const& document)
{
    return const_cast<DOM::Document&>(document).layout_node_arena().host();
}

Layout::RustFFI::FfiVisualContextUpdateOutcome rust_update_accumulated_visual_contexts(Layout::BegunRead const& read, DOM::Document& document)
{
    auto update_timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
    auto outcome = Layout::RustFFI::render_state_update_accumulated_visual_contexts(document.layout_node_arena().host(), &read, viewport_row_slot(read, document), visual_context_tree_inputs(document));
    if (rust_painting_timing_enabled())
        dbgln("AVC_UPDATE rust={} µs {}", update_timer.elapsed_time().to_microseconds(), outcome.performed_full_build ? "full"sv : "incremental"sv);
    return outcome;
}

Vector<u32> rust_owned_visual_context_node_indices(Layout::Node const& layout_node, Layout::RustFFI::FfiVisualContextBoxNodeList list)
{
    Vector<u32> indices;
    if (!has_committed_box(layout_node))
        return indices;
    auto* host = layout_node.document_host();
    auto slot = committed_row_slot(layout_node);
    indices.resize(Layout::RustFFI::render_state_paintable_visual_context_node_count(host, slot, list));
    if (!indices.is_empty())
        Layout::RustFFI::render_state_paintable_visual_context_copy_node_indices(host, slot, list, indices.data(), indices.size());
    return indices;
}

bool rust_background_color_can_be_compositor_animated(Layout::Node const& layout_node)
{
    if (!has_committed_box(layout_node))
        return false;
    return Layout::RustFFI::render_state_background_color_can_be_compositor_animated(
        layout_node.document_host(), committed_row_slot(layout_node));
}

void const* retain_rust_main_visual_context_tree(Layout::BegunRead const& read, DOM::Document const& document)
{
    auto const* tree = Layout::RustFFI::render_state_main_visual_context_tree_retain(document_host(document), &read);
    VERIFY(tree);
    return tree;
}

Layout::RustFFI::FfiPhysicalOverflowDirections rust_physical_overflow_directions(Layout::Node const& box)
{
    return Layout::RustFFI::render_state_physical_overflow_directions(box.document_host(), committed_row_slot(box));
}

void register_geometry_host(Layout::NodeArena& arena)
{
    Layout::RustFFI::FfiGeometryHostCallbacks callbacks {
        .context = &arena,
        .set_scroll_offset = [](void* context, Layout::BegunRead const* read, Compositing::RustFFI::NodeSlotId slot, CSSPixelPoint offset) {
            auto* layout_node = static_cast<Layout::NodeArena*>(context)->node_if_live(*read, slot);
            VERIFY(layout_node);
            set_scroll_offset(*layout_node, offset); },
    };
    Layout::RustFFI::document_host_set_geometry_host(arena.host(), callbacks);
}

Layout::RustFFI::FfiRenderingPreparationOutcome rust_prepare_for_rendering(Layout::BegunRead const& read, DOM::Document& document, bool visual_context_update_pending)
{
    return Layout::RustFFI::render_state_prepare_for_rendering(document.layout_node_arena().host(), &read, visual_context_update_pending, &document,
        [](void* document) { return visual_context_tree_inputs(*static_cast<DOM::Document*>(document)); });
}

static CSS::PreferredColorScheme image_color_scheme(Layout::NodeWithStyle const& layout_node)
{
    auto supports_color_scheme = [](ReadonlySpan<Utf16FlyString> schemes) {
        return schemes.contains_slow("light"_utf16) || schemes.contains_slow("dark"_utf16);
    };
    if (supports_color_scheme(layout_node.color_schemes()))
        return layout_node.color_scheme();
    auto& document = layout_node.document();
    if (auto schemes = document.supported_color_schemes(); schemes.has_value() && supports_color_scheme(*schemes))
        return layout_node.color_scheme();
    // INTEROP: Like Firefox, images use the preferred scheme when neither the element nor
    //          its document opts into a supported scheme. Controls still default to light.
    return document.svg_image_color_scheme().value_or(document.page().preferred_color_scheme());
}

CSS::ColorResolutionContext gradient_stop_color_resolution_context(Layout::NodeWithStyle const& layout_node)
{
    void const* current_color_style_value_data = nullptr;
    if (auto* dom_node = layout_node.dom_node()) {
        if (auto* element = as_if<DOM::Element>(*dom_node)) {
            if (auto const* values = element->style_group<CSS::ComputedValues::InheritedTextValues>())
                current_color_style_value_data = values->color_style_value.pointer;
        }
    }
    return {
        .color_scheme = layout_node.color_scheme(),
        .current_color = layout_node.color(),
        .current_color_style_value_data = current_color_style_value_data,
        .calculation_resolution_context = {},
    };
}

void rust_update_visual_viewport_transform(Layout::BegunRead const& read, DOM::Document& document)
{
    Layout::RustFFI::render_state_update_visual_viewport_transform(document.layout_node_arena().host(), &read, visual_context_tree_inputs(document));
}

// Describes the row in the slot as its layout node describes itself, for a dump or a trace.
static void push_debug_description(DOM::Document const& document, Compositing::RustFFI::NodeSlotId slot, void* description_sink)
{
    Layout::ForcedReadScope read { document };
    auto const* layout_node = const_cast<DOM::Document&>(document).layout_node_arena().node_if_live(read, slot);
    VERIFY(layout_node);
    auto description = layout_node->debug_description();
    auto bytes = description.bytes();
    Layout::RustFFI::layout_arena_paint_push_bytes(description_sink, bytes.data(), bytes.size());
}

Utf16String serialize_painting_dump(Layout::BegunRead const& read, DOM::Document const& document, Compositing::AccumulatedVisualContextTree const& visual_context_tree, Compositing::DisplayList const& display_list, Compositing::DisplayListResourceStorage const& resource_storage)
{
    struct DumpContext {
        GC::Ref<DOM::Document const> document;
        Compositing::DisplayListResourceStorage const& resource_storage;
        Utf16String dump;
    } context { document, resource_storage, {} };

    Layout::RustFFI::FfiPaintingDumpCallbacks callbacks {
        .context = &context,
        .debug_description = [](void* context_pointer, Compositing::RustFFI::NodeSlotId slot, void* description_sink) { push_debug_description(*static_cast<DumpContext*>(context_pointer)->document, slot, description_sink); },
        .command_bytes = [](void*, void const* display_list_pointer, size_t* byte_count) -> u8 const* {
            auto bytes = static_cast<Compositing::DisplayList const*>(display_list_pointer)->command_bytes();
            *byte_count = bytes.size();
            return bytes.data();
        },
        .command_runs = [](void*, void const* display_list_pointer, size_t* run_count) -> Compositing::DisplayListCommandRun const* {
            auto runs = static_cast<Compositing::DisplayList const*>(display_list_pointer)->command_runs();
            *run_count = runs.size();
            return runs.data();
        },
        .nested_display_list = [](void* context_pointer, u64 display_list_id) -> void const* {
            auto& context = *static_cast<DumpContext*>(context_pointer);
            return &context.resource_storage.display_list(Compositing::DisplayListResourceId { display_list_id });
        },
        .append_text = [](void* context_pointer, u8 const* bytes, size_t byte_count) { static_cast<DumpContext*>(context_pointer)->dump = Utf16String::from_utf8_without_validation(StringView { bytes, byte_count }); },
    };
    auto command_runs = display_list.command_runs();
    Layout::RustFFI::painting_dump(document_host(document), &read, viewport_row_slot(read, document), visual_context_tree.rust_handle(), command_runs.data(), command_runs.size(), &display_list, callbacks);
    return move(context.dump);
}

static void append_bytes_to_string_builder(void* context, u8 const* bytes, size_t byte_count)
{
    static_cast<StringBuilder*>(context)->append(StringView { bytes, byte_count });
}

void dump_stacking_context_tree(Layout::BegunRead const& read, StringBuilder& builder, DOM::Document const& document)
{
    struct DumpContext {
        GC::Ref<DOM::Document const> document;
        StringBuilder& builder;
    } context { document, builder };
    Layout::RustFFI::FfiStackingContextDumpCallbacks callbacks {
        .context = &context,
        .debug_description = [](void* context_pointer, Compositing::RustFFI::NodeSlotId slot, void* description_sink) { push_debug_description(*static_cast<DumpContext*>(context_pointer)->document, slot, description_sink); },
        .append_text = [](void* context_pointer, u8 const* bytes, size_t byte_count) { static_cast<DumpContext*>(context_pointer)->builder.append(StringView { bytes, byte_count }); },
    };
    Layout::RustFFI::render_state_dump_stacking_context_tree(
        document_host(document), &read, viewport_row_slot(read, document), callbacks);
}

static void push_bytes_to_dump_sink(void* sink, ReadonlyBytes bytes)
{
    Layout::RustFFI::layout_arena_paint_push_bytes(sink, bytes.data(), bytes.size());
}

static void dump_layout_tree(Layout::Node const& root, size_t initial_indent, bool interactive, void* output_context, void (*append_text)(void*, u8 const*, size_t))
{
    Layout::RustFFI::FfiLayoutTreeDumpCallbacks callbacks {
        .context = output_context,
        .document = &const_cast<DOM::Document&>(root.document()),
        .describe_dom_node = [](void* document, Layout::RustFFI::FfiNodeIdentity node, void* tag_name_sink, void* identifier_sink) {
            // A row whose node has been removed no longer names one, and the dump says so.
            auto dom_node = node_identity_of(node).resolve(*static_cast<DOM::Document*>(document));
            if (!dom_node) {
                push_bytes_to_dump_sink(tag_name_sink, "(detached)"sv.bytes());
                return;
            }
            auto const* element = as_if<DOM::Element>(*dom_node);
            StringBuilder tag_name_builder;
            tag_name_builder.append(element ? element->local_name() : dom_node->node_name());
            push_bytes_to_dump_sink(tag_name_sink, tag_name_builder.string_view().bytes());
            if (!element)
                return;
            StringBuilder identifier_builder;
            if (element->id().has_value() && !element->id()->is_empty()) {
                identifier_builder.append('#');
                identifier_builder.append(*element->id());
            }
            for (auto const& class_name : element->class_names()) {
                identifier_builder.append('.');
                identifier_builder.append(class_name);
            }
            push_bytes_to_dump_sink(identifier_sink, identifier_builder.string_view().bytes()); },
        .navigable_container_content_document = [](void* document, Layout::RustFFI::FfiNodeIdentity node, void* url_sink) -> Layout::RustFFI::FfiNestedLayoutRoot {
            auto const* container = as_if<HTML::NavigableContainer>(node_identity_of(node).resolve(*static_cast<DOM::Document*>(document)).ptr());
            auto const* content_document = container ? container->content_document_without_origin_check() : nullptr;
            if (!content_document)
                return { .has_document = false, .layout_root = nullptr };
            auto serialized_url = content_document->url().serialize();
            push_bytes_to_dump_sink(url_sink, serialized_url.bytes());
            // The nested document's tree is the dump's own read of that document's render state.
            Layout::ForcedReadScope read { *content_document };
            return { .has_document = true, .layout_root = const_cast<Layout::Viewport*>(content_document->layout_node(read)) }; },
        .svg_as_image_layout_root = [](void* document, Layout::RustFFI::FfiNodeIdentity node) -> void* {
            auto const* image_element = as_if<HTML::HTMLImageElement>(node_identity_of(node).resolve(*static_cast<DOM::Document*>(document)).ptr());
            if (!image_element)
                return nullptr;
            auto const* svg_image_data = as_if<SVG::SVGDecodedImageData>(image_element->current_request().image_data().ptr());
            if (!svg_image_data)
                return nullptr;
            // The image's tree is the dump's own read of its document's render state.
            Layout::ForcedReadScope read { svg_image_data->svg_document() };
            return const_cast<Layout::Viewport*>(svg_image_data->svg_document().unsafe_layout_node(read)); },
        .dump_nested_layout_tree = [](void*, void* layout_root, size_t indent, bool interactive, void* output_sink) { dump_layout_tree(*static_cast<Layout::Node const*>(layout_root), indent, interactive, output_sink, Layout::RustFFI::layout_arena_paint_push_bytes); },
        .append_text = append_text,
    };
    Layout::RustFFI::render_state_dump_layout_tree(root.document_host(), Layout::Node::slot_id(&root), initial_indent, interactive, callbacks);
}

void dump_layout_tree(StringBuilder& builder, Layout::Node const& root, bool interactive)
{
    dump_layout_tree(root, 0, interactive, &builder, append_bytes_to_string_builder);
}

namespace {

// What a recording's publication adds its resources to, and what an SVG-as-image render is found and cut from: the
// document's host, through which a layout node of the recording is reached, and the tree the recording was cut from.
struct RecordingPublishContext {
    Compositing::DisplayListResourceStorage& resource_storage;
    Layout::RustFFI::DocumentHost* host;
    Layout::BegunRead const& read;
    Compositing::AccumulatedVisualContextTree const& visual_context_tree;
};

static Layout::RustFFI::FfiRecordingPublishCallbacks recording_publish_callbacks(RecordingPublishContext& context)
{
    return {
        .context = &context,
        .add_font = [](void* context_pointer, void const* font) {
            auto& context = *static_cast<RecordingPublishContext*>(context_pointer);
            context.resource_storage.add_font(*static_cast<Gfx::Font const*>(font)); },
        .add_image_frame = [](void* context_pointer, void const* frame) {
            auto& context = *static_cast<RecordingPublishContext*>(context_pointer);
            context.resource_storage.add_image_frame(*static_cast<Gfx::DecodedImageFrame const*>(frame)); },
        .resolve_vector_image_display_list = [](void* context_pointer, Layout::RustFFI::FfiVectorImageRenderRequest const* request) -> u64 {
            auto& context = *static_cast<RecordingPublishContext*>(context_pointer);
            auto empty_display_list = [&] {
                return context.resource_storage.add_display_list(Compositing::DisplayList::create(context.visual_context_tree), context.visual_context_tree).value();
            };
            auto const* layout_node = static_cast<Layout::NodeWithStyle const*>(Layout::RustFFI::render_state_node_shell_if_live(context.host, &context.read, request->owner));
            if (!layout_node)
                return empty_display_list();
            GC::Ptr<HTML::DecodedImageData> decoded_image_data;
            // A recording that lands after its boxes' elements were removed may draw a box whose element is gone.
            if (request->is_replaced_content) {
                if (layout_node->kind() == Layout::RustFFI::NodeKind::ImageBox) {
                    if (auto const* image_provider = static_cast<Layout::Box const&>(*layout_node).image_provider_if_any())
                        decoded_image_data = image_provider->decoded_image_data();
                } else if (auto const* image = as_if<SVG::SVGImageElement>(layout_node->dom_node())) {
                    decoded_image_data = image->decoded_image_data();
                }
            } else if (auto const* observer = layer_image_observer(*layout_node, request->list, request->computed_index)) {
                decoded_image_data = observer->decoded_image_data();
            }
            auto const* svg_image_data = as_if<SVG::SVGDecodedImageData>(decoded_image_data.ptr());
            if (!svg_image_data)
                return empty_display_list();
            auto display_list = svg_image_data->record_display_list_at_scale({ request->css_width, request->css_height }, request->raster_scale, image_color_scheme(*layout_node), context.resource_storage);
            if (!display_list.has_value())
                return empty_display_list();
            return context.resource_storage.add_display_list(move(*display_list)).value();
        },
        .add_video_sink = [](void* context_pointer, u64 resource_id, u64 sink_handle) {
            auto& context = *static_cast<RecordingPublishContext*>(context_pointer);
            context.resource_storage.add_video_sink(Compositing::VideoSinkResourceId { resource_id }, Media::VideoSinkHandle { sink_handle }); },
    };
}

// The platform default font at an overlay label's CSS size and at that size in device pixels, kept alive for the
// recording call.
struct OverlayLabelFonts {
    RefPtr<Gfx::Font> css_font;
    RefPtr<Gfx::Font> device_font;

    Layout::RustFFI::FfiOverlayLabelFonts ffi() const { return { .css_font = css_font.ptr(), .device_font = device_font.ptr() }; }
};

static OverlayLabelFonts overlay_label_fonts(float css_size, double device_pixels_per_css_pixel)
{
    OverlayLabelFonts fonts {
        .css_font = Platform::FontPlugin::the().default_font(css_size),
        .device_font = Platform::FontPlugin::the().default_font(css_size * static_cast<float>(device_pixels_per_css_pixel)),
    };
    VERIFY(fonts.css_font && fonts.device_font);
    return fonts;
}

}

void take_recording_trace_if_pending(Layout::BegunRead const& read, DOM::Document& document)
{
    struct TraceContext {
        GC::Ref<DOM::Document const> document;
        StringBuilder trace;
    } context { document, {} };
    bool has_pending_trace = Layout::RustFFI::render_state_take_recording_trace(
        document_host(document), &read, &context,
        [](void* context_pointer, Compositing::RustFFI::NodeSlotId slot, void* description_sink) { push_debug_description(*static_cast<TraceContext*>(context_pointer)->document, slot, description_sink); },
        [](void* context_pointer, u8 const* bytes, size_t byte_count) { static_cast<TraceContext*>(context_pointer)->trace.append(StringView { bytes, byte_count }); });
    if (has_pending_trace)
        document.paint_state().append_recording_trace(MUST(context.trace.to_string()));
}

// The time of the document's timeline its rendering update sampled the animations at: the frames sampled after the
// update's never show them before it.
static double rendering_update_timestamp(DOM::Document& document)
{
    if (auto time = document.timeline()->current_time(); time.has_value() && time->type == Animations::TimeValue::Type::Milliseconds)
        return time->value;
    return -AK::Infinity<double>;
}

// Hands the render owner `presentation` and its seal, which the host gives up.
static Layout::RustFFI::FfiPresentation give_up(Compositor::FlightPresentation presentation)
{
    return { .presenter = &presentation.presenter.leak_ref(), .sealed = presentation.sealed.leak_ptr() };
}

void commit_unrecorded_frame(Layout::BegunRead const& read, DOM::Document& document, Compositor::FlightPresentation presentation)
{
    bool const sends_visual_context_tree = presentation.sealed->sends_visual_context_tree;
    Layout::RustFFI::render_state_commit_unrecorded_frame(document_host(document), &read, sends_visual_context_tree, rendering_update_timestamp(document), give_up(move(presentation)));
}

Optional<DisplayListRecording> start_rust_display_list_recording(Layout::BegunRead const& read, DOM::Document& document, Compositing::AccumulatedVisualContextTree visual_context_tree, Optional<Gfx::Color> surface_clear_color, PaintCommandCacheMode cache_mode, HTML::PaintConfig const& config, InspectorOverlayInputs const& overlay_inputs, Optional<Compositor::FlightPresentation> committed)
{
    auto* host = document_host(document);
    auto device_pixels_per_css_pixel = document.page().client().device_pixels_per_css_pixel();
    auto device_viewport_rect = document.page().css_to_device_rect(document.viewport_rect());
    auto wheel_event_region_state = document.paint_state().collect_root_blocking_wheel_event_regions(document);
    Layout::RustFFI::FfiRecordingInputs inputs {};
    if (overlay_inputs.highlighted_layout_node) {
        inputs.has_inspector_highlight = true;
        inputs.inspector_highlight_paintable = committed_row_slot(*overlay_inputs.highlighted_layout_node);
    }
    inputs.tooltip_color = overlay_inputs.tooltip_color;
    inputs.tooltip_text_color = overlay_inputs.tooltip_text_color;
    inputs.tooltip_border_color = overlay_inputs.tooltip_border_color;
    Vector<Layout::RustFFI::FfiGridOverlayInput> ffi_grid_overlays;
    OverlayLabelFonts grid_label_fonts;
    if (!overlay_inputs.grid_highlights.is_empty()) {
        grid_label_fonts = overlay_label_fonts(10.0f, device_pixels_per_css_pixel);
        inputs.grid_label_fonts = grid_label_fonts.ffi();
    }
    for (auto const& highlight : overlay_inputs.grid_highlights) {
        ffi_grid_overlays.append({
            .paintable = committed_row_slot(*highlight.layout_node),
            .color = highlight.options.color,
            .label_foreground_color = highlight.options.color.with_alpha(235).suggested_foreground_color(),
            .label_css_pixel_size = grid_label_fonts.css_font->pixel_size(),
            .show_area_names = highlight.options.show_area_names,
            .show_line_numbers = highlight.options.show_line_numbers,
            .show_track_sizes = highlight.options.show_track_sizes,
            .show_infinite_lines = highlight.options.show_infinite_lines,
        });
    }
    inputs.grid_overlays = ffi_grid_overlays.data();
    inputs.grid_overlay_count = ffi_grid_overlays.size();
    Vector<Layout::RustFFI::FfiFlexOverlayInput> ffi_flex_overlays;
    for (auto const& highlight : overlay_inputs.flex_highlights) {
        ffi_flex_overlays.append({
            .paintable = committed_row_slot(*highlight.layout_node),
            .color = highlight.options.color,
        });
    }
    inputs.flex_overlays = ffi_flex_overlays.data();
    inputs.flex_overlay_count = ffi_flex_overlays.size();
    inputs.caret_debug_rect = overlay_inputs.caret_debug_rect;
    ByteString inspector_highlight_label_text;
    OverlayLabelFonts inspector_label_fonts;
    if (overlay_inputs.highlighted_layout_node) {
        auto const& layout_node = *overlay_inputs.highlighted_layout_node;
        auto border_rect = absolute_border_box_rect(layout_node);
        inspector_highlight_label_text = ByteString::formatted("{} {}x{} @ {},{}", layout_node.debug_description(), border_rect.width(), border_rect.height(), border_rect.x(), border_rect.y());
        inspector_label_fonts = overlay_label_fonts(12.0f, device_pixels_per_css_pixel);
        inputs.inspector_highlight_label = {
            .fonts = inspector_label_fonts.ffi(),
            .text = inspector_highlight_label_text.bytes().data(),
            .text_byte_count = inspector_highlight_label_text.length(),
        };
    }
    inputs.device_viewport_rect = device_viewport_rect.to_type<int>();
    if (auto navigable = document.navigable())
        inputs.css_viewport_rect = navigable->viewport_rect();
    inputs.should_show_line_box_borders = config.should_show_line_box_borders;
    inputs.force_dark_enabled = config.force_dark_enabled;
    inputs.force_dark_foreground_threshold = config.force_dark_foreground_threshold;
    inputs.force_dark_background_threshold = config.force_dark_background_threshold;
    inputs.should_paint_overlay = config.paint_overlay;
    inputs.document_id = document.unique_id().value();
    inputs.has_blocking_wheel_event_region_covering_viewport = wheel_event_region_state.has_blocking_wheel_event_region_covering_viewport;
    inputs.wheel_event_listener_state_generation = document.page().wheel_event_listener_state_generation();
    inputs.chrome_metrics = document.page().chrome_metrics();
    inputs.paint_viewport_scrollbars = should_paint_viewport_scrollbars();
    if (auto navigable = document.navigable()) {
        if (auto handler = navigable->event_handler().middle_button_scroll_handler(); handler.has_value()) {
            inputs.middle_button_scroll_active = true;
            inputs.middle_button_scroll_origin = handler->origin();
        }
    }
    inputs.publishes_recording = cache_mode == PaintCommandCacheMode::ReadWrite;
    {
        auto navigable = document.navigable();
        inputs.window_is_focused = navigable && navigable->is_focused();
        inputs.outline_auto_color = CSS::SystemColor::accent_color(CSS::PreferredColorScheme::Auto);
        auto palette = document.page().palette();
        inputs.palette_is_dark = palette.is_dark();
        inputs.selection_background_from_palette = CSS::SystemColor::transform_selection_background_color(palette.selection());
        inputs.selection_background_light = CSS::SystemColor::transform_selection_background_color(CSS::SystemColor::highlight(CSS::PreferredColorScheme::Light));
        inputs.selection_background_dark = CSS::SystemColor::transform_selection_background_color(CSS::SystemColor::highlight(CSS::PreferredColorScheme::Dark));
        inputs.inactive_selection_background_from_palette = CSS::SystemColor::transform_selection_background_color(palette.inactive_selection());
        inputs.inactive_selection_background_light = CSS::SystemColor::transform_selection_background_color(CSS::SystemColor::inactive_highlight(CSS::PreferredColorScheme::Light));
        inputs.inactive_selection_background_dark = CSS::SystemColor::transform_selection_background_color(CSS::SystemColor::inactive_highlight(CSS::PreferredColorScheme::Dark));
        inputs.document_has_supported_color_schemes = document.supported_color_schemes().has_value();
        // The AT focus ring paints as an outline on one paintable, whether or not that element has a CSS outline of
        // its own. So the recorder learns which paintable that is here, up front — its paint-phase mask would
        // otherwise skip that paintable's outline phase as empty.
        auto const* accessibility_focus_target = document.accessibility_focus_target();
        inputs.accessibility_focus_target = Layout::Node::slot_id(accessibility_focus_target ? accessibility_focus_target->layout_node(read) : nullptr);
    }
    inputs.caret = resolve_document_caret_paint(read, document);
    inputs.focused_text_control = resolve_focused_text_control_selection(read, document);
    Vector<u8> focused_area_path_bytes;
    inputs.focused_area_outline = resolve_focused_area_outline(read, document, focused_area_path_bytes);
    {
        auto color_scheme = document.canvas_color_scheme(read);
        bool opaque_canvas = false;
        // NB: The container's document is laid out ahead of the document of the navigable it hosts, and its box is read
        //     as it last laid it out, as the embedding document's own read.
        if (auto container_element = document.navigable()->container()) {
            Layout::ForcedReadScope container_read { container_element->document() };
            if (auto const* container_node = container_element->unsafe_layout_node(container_read)) {
                auto container_scheme = container_node->color_scheme();
                if (container_scheme == CSS::PreferredColorScheme::Auto)
                    container_scheme = CSS::PreferredColorScheme::Light;
                opaque_canvas = container_scheme != color_scheme;
            }
        }
        inputs.canvas_fill_rect = config.canvas_fill_rect;
        inputs.canvas_color = CSS::SystemColor::canvas(color_scheme);
        inputs.opaque_canvas = opaque_canvas;
        Gfx::IntRect bitmap_rect { {}, device_viewport_rect.size().to_type<int>() };
        inputs.bitmap_rect = bitmap_rect;
        inputs.background_color = document.background_color(read);
    }
    reconcile_navigable_container_paint_facts(read, document);
    Optional<Compositing::DisplayList::AsyncScrollingMetadata> async_scrolling_metadata;
    if (auto navigable = document.navigable()) {
        async_scrolling_metadata = Compositing::DisplayList::AsyncScrollingMetadata {
            .viewport_rect = device_viewport_rect.to_type<int>(),
            .wheel_event_listener_state_generation = navigable->page().wheel_event_listener_state_generation(),
            .has_blocking_wheel_event_listeners = wheel_event_region_state.has_blocking_wheel_event_listeners,
            .has_blocking_wheel_event_region_covering_viewport = wheel_event_region_state.has_blocking_wheel_event_region_covering_viewport,
            .device_pixels_per_css_pixel = device_pixels_per_css_pixel,
        };
    }
    DisplayListRecording recording {
        .visual_context_tree = move(visual_context_tree),
        .surface_clear_color = surface_clear_color,
        .cache_mode = cache_mode,
        .in_flight = false,
        .async_scrolling_metadata = async_scrolling_metadata,
        .paint_command_cache_source = document.paint_state().display_list_used_as_paint_command_cache_source(),
    };
    // The recording copies what it reads of the overlay arrays and buffers, which live until here.
    auto viewport = viewport_row_slot(read, document);
    // A committed frame takes the presentation it presents with, which the landing gives back.
    if (committed.has_value()) {
        committed->sealed->recording = recording;
        recording.in_flight = true;
        Layout::RustFFI::render_state_commit_recorded_frame(host, &read, viewport, inputs, rendering_update_timestamp(document), give_up(committed.release_value()));
        return recording;
    }
    if (Layout::RustFFI::render_state_record_display_list(host, &read, viewport, inputs) == Layout::RustFFI::FfiRecordingStart::NothingToRecord)
        return {};
    return recording;
}

void render_vector_images(Layout::BegunRead const& read, DOM::Document& document, DisplayListRecording const& recording)
{
    auto resources = make<Compositor::VectorImageResources>();
    RecordingPublishContext publish_context { resources->storage, document_host(document), read, recording.visual_context_tree };
    Layout::RustFFI::render_state_render_vector_images(publish_context.host, recording_publish_callbacks(publish_context), resources.leak_ptr());
}

RefPtr<Compositing::DisplayList> finish_rust_display_list_recording(Layout::BegunRead const& read, DOM::Document& document, DisplayListRecording const& recording, Compositing::DisplayListResourceStorage& resource_storage)
{
    RecordingPublishContext publish_context { resource_storage, document_host(document), read, recording.visual_context_tree };
    auto rust_timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
    Layout::RustFFI::FfiPresentedRecording presented {};
    Layout::RustFFI::render_state_publish_recording(publish_context.host, &read, recording_publish_callbacks(publish_context), &presented);
    take_recording_trace_if_pending(read, document);
    auto display_list = display_list_of_published_recording(recording, presented);
    if (rust_painting_timing_enabled())
        dbgln("PAINT_RECORD rust={} µs commands={} bytes", rust_timer.elapsed_time().to_microseconds(), display_list->command_bytes().size());
    return display_list;
}

NonnullRefPtr<Compositing::DisplayList> display_list_of_published_recording(DisplayListRecording const& recording, Layout::RustFFI::FfiPresentedRecording const& presented)
{
    auto stamp_async_scrolling_metadata = [&](Compositing::DisplayList& display_list) {
        auto metadata = recording.async_scrolling_metadata;
        if (!metadata.has_value())
            return;
        metadata->has_blocking_wheel_event_listeners |= presented.has_blocking_wheel_event_listeners;
        display_list.set_async_scrolling_metadata(*metadata);
    };

    if (presented.is_identical_to_published_recording) {
        if (auto source = recording.paint_command_cache_source) {
            stamp_async_scrolling_metadata(*source);
            return source.release_nonnull();
        }
    }

    auto display_list = Compositing::DisplayList::share_rust_command_storage(recording.visual_context_tree, presented.display_list);
    if (recording.surface_clear_color.has_value())
        display_list->set_surface_clear_color(*recording.surface_clear_color);
    stamp_async_scrolling_metadata(*display_list);
    return display_list;
}

static Compositing::DisplayListResource record_image_paint(Layout::RustFFI::FfiImagePaintRecordInputs const& inputs)
{
    Optional<Compositing::DisplayListResource> recorded_display_list;
    Layout::RustFFI::ladybird_web_record_image_paint_display_list(&inputs, &recorded_display_list,
        [](void* context, void const* retained_commands, void const* retained_tree) {
            auto visual_context_tree = Compositing::AccumulatedVisualContextTree::adopt_rust_handle(retained_tree);
            auto display_list = Compositing::DisplayList::adopt_rust_command_storage(visual_context_tree, retained_commands);
            *static_cast<Optional<Compositing::DisplayListResource>*>(context) = Compositing::DisplayListResource { move(display_list), move(visual_context_tree) };
        });
    return recorded_display_list.release_value();
}

Compositing::DisplayListResource record_image_paint_display_list(ImagePaint const& paint, ImagePaintRequest const& request, double device_pixels_per_css_pixel)
{
    Layout::RustFFI::FfiImagePaintRecordInputs inputs {};
    inputs.dest_rect = request.dest_rect;
    inputs.device_pixels_per_css_pixel = device_pixels_per_css_pixel;
    Optional<CSS::ComputedValuesFFI::FfiLengthResolutionContext> gradient_stop_length_resolution_context_storage;
    CSS::StyleValueFFI::FfiColorResolutionInput gradient_stop_color_resolution_input {};
    paint.value.visit(
        [&](ImagePaint::DecodedFrame const& decoded_frame) {
            inputs.kind = Layout::RustFFI::FfiImagePaintRecordKind::DecodedFrame;
            inputs.frame_id = request.resource_storage.add_image_frame(decoded_frame.frame).value();
            inputs.scaling_mode = CSS::to_gfx_scaling_mode(request.image_rendering, decoded_frame.natural_size, request.dest_rect.to_rounded<int>().size());
        },
        [&](ImagePaint::NestedDisplayList const& nested) {
            inputs.kind = Layout::RustFFI::FfiImagePaintRecordKind::NestedDisplayList;
            inputs.nested_display_list_id = request.resource_storage.add_display_list(nested.resource.display_list, nested.resource.visual_context_tree).value();
            inputs.nested_display_list_size = nested.list_size;
        },
        [&](ImagePaint::Gradient const& gradient) {
            inputs.kind = Layout::RustFFI::FfiImagePaintRecordKind::Gradient;
            inputs.gradient_style_value = gradient.style_value->rust_style_value_data();
            inputs.gradient_tile_size = request.dest_rect.size().to_type<CSSPixels>();
            gradient_stop_color_resolution_input = CSS::make_rust_color_resolution_input(request.gradient_stop_color_resolution_context, gradient_stop_length_resolution_context_storage);
            inputs.gradient_stop_color_resolution_input = &gradient_stop_color_resolution_input;
        });
    return record_image_paint(inputs);
}

Compositing::DisplayListResource record_image_frame_display_list(Gfx::DecodedImageFrame const& frame, Gfx::FloatRect const& dest_rect, Gfx::ScalingMode scaling_mode, Compositing::DisplayListResourceStorage& resource_storage)
{
    Layout::RustFFI::FfiImagePaintRecordInputs inputs {};
    inputs.kind = Layout::RustFFI::FfiImagePaintRecordKind::DecodedFrame;
    inputs.dest_rect = dest_rect;
    inputs.device_pixels_per_css_pixel = 1;
    inputs.frame_id = resource_storage.add_image_frame(frame).value();
    inputs.scaling_mode = scaling_mode;
    return record_image_paint(inputs);
}

}
