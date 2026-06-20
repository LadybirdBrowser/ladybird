/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Lucien Fiorini <lucienfiorini@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/NumericLimits.h>
#include <AK/OwnPtr.h>
#include <AK/Utf16StringBuilder.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/CanvasCommandList.h>
#include <LibGfx/CompositingAndBlendingOperator.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Painter.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/Rect.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibUnicode/Segmenter.h>
#include <LibWeb/Bindings/CanvasRenderingContext2D.h>
#include <LibWeb/Bindings/DOMRectReadOnly.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleValues/FilterValueListStyleValue.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/Canvas/RemoteCanvas2DTransport.h>
#include <LibWeb/HTML/CanvasRenderingContext2D.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/ImageData.h>
#include <LibWeb/HTML/ImageRequest.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Path2D.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/TextMetrics.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/SVG/SVGImageElement.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

// Keep bitmap-heavy recorded command lists small enough to send to the Compositor without exceeding IPC attachment limits.
static constexpr size_t max_pending_canvas_commands = 64;

GC_DEFINE_ALLOCATOR(CanvasRenderingContext2D);

JS::ThrowCompletionOr<GC::Ref<CanvasRenderingContext2D>> CanvasRenderingContext2D::create(JS::Realm& realm, HTMLCanvasElement& element, JS::Value options)
{
    auto context_attributes = TRY(Bindings::convert_to_idl_value_for_canvas_rendering_context2d_settings(realm.vm(), options));
    return realm.create<CanvasRenderingContext2D>(realm, element, context_attributes);
}

CanvasRenderingContext2D::CanvasRenderingContext2D(JS::Realm& realm, HTMLCanvasElement& element, Bindings::CanvasRenderingContext2DSettings context_attributes)
    : PlatformObject(realm)
    , CanvasPath(static_cast<Bindings::PlatformObject&>(*this), *this)
    , m_element(element)
    , m_size(element.bitmap_size_for_canvas())
    , m_context_attributes(move(context_attributes))
{
}

CanvasRenderingContext2D::~CanvasRenderingContext2D() = default;

void CanvasRenderingContext2D::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    set_prototype(&Bindings::ensure_web_prototype<Bindings::CanvasRenderingContext2DPrototype>(realm, "CanvasRenderingContext2D"_string));
}

void CanvasRenderingContext2D::finalize()
{
    discard_backing_storage();
    Base::finalize();
}

void CanvasRenderingContext2D::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    CanvasState::visit_edges(visitor);
    visitor.visit(m_element);
}

size_t CanvasRenderingContext2D::external_memory_size() const
{
    auto size = Base::external_memory_size();
    if (!has_backing_storage())
        return size;

    auto surface_size = m_size;
    if (surface_size.is_empty())
        return size;

    Checked<size_t> pixel_size = static_cast<size_t>(surface_size.width());
    pixel_size *= static_cast<size_t>(surface_size.height());
    pixel_size *= sizeof(u32);
    if (pixel_size.has_overflow())
        return NumericLimits<size_t>::max();
    return JS::saturating_add_external_memory_size(size, pixel_size.value());
}

GC::Ref<HTMLCanvasElement> CanvasRenderingContext2D::canvas_for_binding() const
{
    return *m_element;
}

Gfx::Path CanvasRenderingContext2D::rect_path(float x, float y, float width, float height)
{
    auto top_left = Gfx::FloatPoint(x, y);
    auto top_right = Gfx::FloatPoint(x + width, y);
    auto bottom_left = Gfx::FloatPoint(x, y + height);
    auto bottom_right = Gfx::FloatPoint(x + width, y + height);

    Gfx::Path path;
    path.move_to(top_left);
    path.line_to(top_right);
    path.line_to(bottom_right);
    path.line_to(bottom_left);
    path.line_to(top_left);
    return path;
}

void CanvasRenderingContext2D::fill_rect(float x, float y, float width, float height)
{
    fill_internal(rect_path(x, y, width, height), Gfx::WindingRule::EvenOdd);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-clearrect
void CanvasRenderingContext2D::clear_rect(float x, float y, float width, float height)
{
    // 1. If any of the arguments are infinite or NaN, then return.
    if (!isfinite(x) || !isfinite(y) || !isfinite(width) || !isfinite(height))
        return;

    if (auto* canvas_command_list = this->canvas_command_list()) {
        auto rect = Gfx::FloatRect(x, y, width, height);
        canvas_command_list->append(Gfx::CanvasCommands::ClearRect { .rect = rect, .color = clear_color() });
        did_draw(rect);
    }
}

void CanvasRenderingContext2D::stroke_rect(float x, float y, float width, float height)
{
    stroke_internal(rect_path(x, y, width, height));
}

// 4.12.5.1.14 Drawing images, https://html.spec.whatwg.org/multipage/canvas.html#drawing-images
WebIDL::ExceptionOr<void> CanvasRenderingContext2D::draw_image_internal(CanvasImageSource const& image, float source_x, float source_y, float source_width, float source_height, float destination_x, float destination_y, float destination_width, float destination_height)
{
    // 1. If any of the arguments are infinite or NaN, then return.
    if (!isfinite(source_x) || !isfinite(source_y) || !isfinite(source_width) || !isfinite(source_height) || !isfinite(destination_x) || !isfinite(destination_y) || !isfinite(destination_width) || !isfinite(destination_height))
        return {};

    // 2. Let usability be the result of checking the usability of image.
    auto usability = TRY(check_usability_of_image(image));

    // 3. If usability is bad, then return (without drawing anything).
    if (usability == CanvasImageSourceUsability::Bad)
        return {};

    auto source_bitmap_rect = Gfx::IntRect { {}, canvas_image_source_dimensions(image) };

    // 4. Establish the source and destination rectangles as follows:
    //    If not specified, the dw and dh arguments must default to the values of sw and sh, interpreted such that one CSS pixel in the image is treated as one unit in the output bitmap's coordinate space.
    //    If the sx, sy, sw, and sh arguments are omitted, then they must default to 0, 0, the image's intrinsic width in image pixels, and the image's intrinsic height in image pixels, respectively.
    //    If the image has no intrinsic dimensions, then the concrete object size must be used instead, as determined using the CSS "Concrete Object Size Resolution" algorithm, with the specified size having
    //    neither a definite width nor height, nor any additional constraints, the object's intrinsic properties being those of the image argument, and the default object size being the size of the output bitmap.
    //    The source rectangle is the rectangle whose corners are the four points (sx, sy), (sx+sw, sy), (sx+sw, sy+sh), (sx, sy+sh).
    //    The destination rectangle is the rectangle whose corners are the four points (dx, dy), (dx+dw, dy), (dx+dw, dy+dh), (dx, dy+dh).
    // NOTE: Implemented in drawImage() overloads
    if (source_width < 0) {
        source_x += source_width;
        source_width = abs(source_width);
    }
    if (source_height < 0) {
        source_y += source_height;
        source_height = abs(source_height);
    }
    if (destination_width < 0) {
        destination_x += destination_width;
        destination_width = abs(destination_width);
    }
    if (destination_height < 0) {
        destination_y += destination_height;
        destination_height = abs(destination_height);
    }

    //    The source rectangle is the rectangle whose corners are the four points (sx, sy), (sx+sw, sy), (sx+sw, sy+sh), (sx, sy+sh).
    auto source_rect = Gfx::FloatRect { source_x, source_y, source_width, source_height };
    //    The destination rectangle is the rectangle whose corners are the four points (dx, dy), (dx+dw, dy), (dx+dw, dy+dh), (dx, dy+dh).
    auto destination_rect = Gfx::FloatRect { destination_x, destination_y, destination_width, destination_height };
    //    When the source rectangle is outside the source image, the source rectangle must be clipped
    //    to the source image and the destination rectangle must be clipped in the same proportion.
    auto clipped_source = source_rect.intersected(source_bitmap_rect.to_type<float>());
    auto clipped_destination = destination_rect;
    if (clipped_source != source_rect) {
        clipped_destination.set_width(clipped_destination.width() * (clipped_source.width() / source_rect.width()));
        clipped_destination.set_height(clipped_destination.height() * (clipped_source.height() / source_rect.height()));
    }

    // 5. If one of the sw or sh arguments is zero, then return. Nothing is painted.
    if (source_width == 0 || source_height == 0)
        return {};

    // 6. Paint the region of the image argument specified by the source rectangle on the region of the rendering context's output bitmap specified by the destination rectangle, after applying the current transformation matrix to the destination rectangle.
    auto scaling_mode = Gfx::ScalingMode::NearestNeighbor;
    if (drawing_state().image_smoothing_enabled) {
        // FIXME: Honor drawing_state().image_smoothing_quality
        scaling_mode = Gfx::ScalingMode::BilinearMipmap;
    }

    if (auto* canvas_command_list = this->canvas_command_list()) {
        if (auto const* source_canvas = image.get_pointer<GC::Ref<HTMLCanvasElement>>()) {
            (*source_canvas)->ensure_backing_storage();
            (*source_canvas)->prepare_for_compositing();
            if (auto source_canvas_id = (*source_canvas)->canvas_id(); source_canvas_id.has_value()) {
                canvas_command_list->append(Gfx::CanvasCommands::DrawCanvas {
                    .source_canvas_id = source_canvas_id->value(),
                    .dst_rect = destination_rect,
                    .src_rect = source_rect.to_rounded<int>(),
                    .scaling_mode = scaling_mode,
                    .filter = drawing_state().filter,
                    .global_alpha = drawing_state().global_alpha,
                    .compositing_and_blending_operator = drawing_state().current_compositing_and_blending_operator,
                });
                did_draw(destination_rect);
                flush_recorded_commands(CommitCommands::No);

                // 7. If image is not origin-clean, then set the CanvasRenderingContext2D's origin-clean flag to false.
                if (image_is_not_origin_clean(image))
                    m_origin_clean = false;

                return {};
            }
        }

        auto frame = canvas_image_source_frame(image);
        if (!frame.has_value())
            return {};

        canvas_command_list->append(Gfx::CanvasCommands::DrawBitmap {
            .frame = *frame,
            .dst_rect = destination_rect,
            .src_rect = source_rect.to_rounded<int>(),
            .scaling_mode = scaling_mode,
            .filter = drawing_state().filter,
            .global_alpha = drawing_state().global_alpha,
            .compositing_and_blending_operator = drawing_state().current_compositing_and_blending_operator,
        });
        did_draw(destination_rect);
    }

    // 7. If image is not origin-clean, then set the CanvasRenderingContext2D's origin-clean flag to false.
    if (image_is_not_origin_clean(image))
        m_origin_clean = false;

    return {};
}

void CanvasRenderingContext2D::did_draw(Gfx::FloatRect const&)
{
    // FIXME: Make use of the rect to reduce the invalidated area when possible.
    m_element->set_canvas_content_dirty();
    m_element->set_needs_repaint(InvalidateDisplayList::No);
}

Gfx::CanvasCommandList* CanvasRenderingContext2D::canvas_command_list()
{
    if (is_context_lost())
        return nullptr;
    ensure_backing_storage();
    if (!has_backing_storage())
        return nullptr;
    if (m_commands.size() >= max_pending_canvas_commands)
        flush_recorded_commands(CommitCommands::No);
    return &m_commands;
}

bool CanvasRenderingContext2D::ensure_remote_canvas_context()
{
    if (m_transport)
        return true;

    auto& page = m_element->document().page();
    if (!page.has_compositor_host())
        return false;
    auto transport = page.compositor_host().create_canvas_2d_transport();
    if (!transport)
        return false;

    // FIXME: implement context attribute .color_space
    // FIXME: implement context attribute .color_type
    // FIXME: implement context attribute .desynchronized
    // FIXME: implement context attribute .will_read_frequently
    if (!transport->create_context(m_element->bitmap_size_for_canvas(), m_context_attributes.alpha))
        return false;
    m_transport = move(transport);
    return true;
}

void CanvasRenderingContext2D::flush_recorded_commands(CommitCommands commit)
{
    if (!m_transport)
        return;

    bool const should_commit = commit == CommitCommands::Yes;
    if (m_commands.is_empty()) {
        if (!should_commit || !m_has_uncommitted_remote_commands)
            return;
        m_transport->update_commands(m_commands, true);
        m_has_uncommitted_remote_commands = false;
        return;
    }

    auto commands = move(m_commands);
    m_transport->update_commands(commands, should_commit);
    m_has_uncommitted_remote_commands = !should_commit;
}

RefPtr<Gfx::Bitmap> CanvasRenderingContext2D::read_pixels(Gfx::IntRect const& rect)
{
    if (!has_backing_storage())
        return nullptr;
    flush_recorded_commands(CommitCommands::No);
    return m_transport->read_back_pixels(rect);
}

void CanvasRenderingContext2D::set_size(Gfx::IntSize const& size)
{
    if (m_size == size)
        return;
    m_size = size;
    discard_backing_storage();
}

void CanvasRenderingContext2D::prepare_for_compositing()
{
    flush_recorded_commands(CommitCommands::Yes);
}

Optional<Painting::CanvasId> CanvasRenderingContext2D::canvas_id() const
{
    if (!m_transport)
        return {};
    return m_transport->canvas_id();
}

// https://html.spec.whatwg.org/multipage/canvas.html#context-loss
void CanvasRenderingContext2D::notify_backing_storage_lost()
{
    if (!has_backing_storage())
        return;

    // When the user agent detects that the backing storage associated with a canvas context has been lost, then it
    // must queue a global task on the DOM manipulation task source given canvas's relevant global object to run
    // these steps:
    queue_global_task(HTML::Task::Source::DOMManipulation, relevant_global_object(*this), GC::create_function(heap(), [this] {
        // 1. Let canvas be context's canvas element.
        // 2. If context's context lost is true, then abort these steps.
        if (is_context_lost())
            return;

        // 3. Set context's context lost to true.
        set_context_lost(true);

        // AD-HOC: Drop recorded-but-unflushed draw commands; they targeted the lost storage.
        discard_backing_storage();

        // 4. Reset the rendering context to its default state given context.
        reset_to_default_state();

        // 5. Let shouldRestore be the result of firing an event named contextlost at canvas, with the cancelable
        //    attribute initialized to true.
        Bindings::EventInit context_lost_event_init;
        context_lost_event_init.cancelable = true;
        bool should_restore = m_element->dispatch_event(DOM::Event::create(realm(), HTML::EventNames::contextlost, context_lost_event_init));

        // 6. If shouldRestore is false, then abort these steps.
        if (!should_restore)
            return;

        // 7. Attempt to restore context by creating a backing storage using context's attributes and associating
        //    them with context. If this fails, then abort these steps.
        ensure_backing_storage();
        if (!has_backing_storage())
            return;

        // 8. Set context's context lost to false.
        set_context_lost(false);

        // 9. Fire an event named contextrestored at canvas.
        m_element->dispatch_event(DOM::Event::create(realm(), HTML::EventNames::contextrestored));
    }));
}

void CanvasRenderingContext2D::ensure_backing_storage()
{
    if (has_backing_storage() || m_size.is_empty())
        return;
    if (!ensure_remote_canvas_context())
        return;

    m_element->set_needs_repaint();
}

void CanvasRenderingContext2D::discard_backing_storage()
{
    m_commands = {};
    m_has_uncommitted_remote_commands = false;
    if (m_transport) {
        m_transport->destroy_context();
        m_transport = nullptr;
    }
}

Gfx::Path CanvasRenderingContext2D::text_path(Utf16String const& text, float x, float y, Optional<double> max_width)
{
    if (max_width.has_value() && max_width.value() <= 0)
        return {};

    auto& drawing_state = this->drawing_state();

    auto const& font_cascade_list = this->font_cascade_list();
    auto const& font = font_cascade_list->first();
    auto glyph_runs = Gfx::shape_text({ x, y }, text.utf16_view(), *font_cascade_list, resolved_letter_spacing());
    Gfx::Path path;
    float text_width = 0;
    for (auto const& glyph_run : glyph_runs) {
        path.glyph_run(glyph_run);
        text_width += glyph_run->width();
    }
    Gfx::AffineTransform transform = {};

    // https://html.spec.whatwg.org/multipage/canvas.html#text-preparation-algorithm:
    // 9. If maxWidth was provided and the hypothetical width of the inline box in the hypothetical line box
    // is greater than maxWidth CSS pixels, then change font to have a more condensed font (if one is
    // available or if a reasonably readable one can be synthesized by applying a horizontal scale
    // factor to the font) or a smaller font, and return to the previous step.
    if (max_width.has_value() && text_width > float(*max_width)) {
        auto horizontal_scale = float(*max_width) / text_width;
        transform = Gfx::AffineTransform {}.scale({ horizontal_scale, 1 });
        text_width *= horizontal_scale;
    }

    // Apply text align
    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-textalign
    // The direction property affects how "start" and "end" are interpreted:
    // - "ltr" or "inherit" (default): start=left, end=right
    // - "rtl": start=right, end=left

    // Determine if we're in RTL mode
    bool is_rtl = drawing_state.direction == Bindings::CanvasDirection::Rtl;

    // Center alignment is the same regardless of direction
    if (drawing_state.text_align == Bindings::CanvasTextAlign::Center) {
        transform = Gfx::AffineTransform {}.set_translation({ -text_width / 2, 0 }).multiply(transform);
    }
    // Handle "start" alignment
    else if (drawing_state.text_align == Bindings::CanvasTextAlign::Start) {
        // In RTL, "start" means right-aligned (translate by full width)
        if (is_rtl) {
            transform = Gfx::AffineTransform {}.set_translation({ -text_width, 0 }).multiply(transform);
        }
        // In LTR, "start" means left-aligned (no translation needed - default)
    }
    // Handle "end" alignment
    else if (drawing_state.text_align == Bindings::CanvasTextAlign::End) {
        // In RTL, "end" means left-aligned (no translation needed)
        if (!is_rtl) {
            // In LTR, "end" means right-aligned (translate by full width)
            transform = Gfx::AffineTransform {}.set_translation({ -text_width, 0 }).multiply(transform);
        }
    }
    // Explicit "left" and "right" alignments ignore direction
    else if (drawing_state.text_align == Bindings::CanvasTextAlign::Right) {
        transform = Gfx::AffineTransform {}.set_translation({ -text_width, 0 }).multiply(transform);
    }
    // Left is the default - no translation needed

    // Apply text baseline
    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-textbaseline
    auto const& font_pixel_metrics = font.pixel_metrics();
    auto baseline_y_offset = [&] {
        switch (drawing_state.text_baseline) {
        case Bindings::CanvasTextBaseline::Top:
            return font_pixel_metrics.ascent;
        case Bindings::CanvasTextBaseline::Hanging:
            return font_pixel_metrics.ascent * 0.8f;
        case Bindings::CanvasTextBaseline::Middle:
            return (font_pixel_metrics.ascent - font_pixel_metrics.descent) / 2.0f;
        case Bindings::CanvasTextBaseline::Alphabetic:
            return 0.0f;
        case Bindings::CanvasTextBaseline::Ideographic:
        case Bindings::CanvasTextBaseline::Bottom:
            return -font_pixel_metrics.descent;
        }
        VERIFY_NOT_REACHED();
    }();

    if (baseline_y_offset != 0.f)
        transform = Gfx::AffineTransform {}.set_translation({ 0, baseline_y_offset }).multiply(transform);

    return path.copy_transformed(transform);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-filltext
void CanvasRenderingContext2D::fill_text(Utf16String const& text, float x, float y, Optional<double> max_width)
{
    if (!isfinite(x) || !isfinite(y) || (max_width.has_value() && !isfinite(max_width.value())))
        return;

    fill_internal(text_path(text, x, y, max_width), Gfx::WindingRule::Nonzero);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-stroketext
void CanvasRenderingContext2D::stroke_text(Utf16String const& text, float x, float y, Optional<double> max_width)
{
    if (!isfinite(x) || !isfinite(y) || (max_width.has_value() && !isfinite(max_width.value())))
        return;

    stroke_internal(text_path(text, x, y, max_width));
}

void CanvasRenderingContext2D::begin_path()
{
    path().clear();
}

static Gfx::Path::CapStyle to_gfx_cap(Bindings::CanvasLineCap const& cap_style)
{
    switch (cap_style) {
    case Bindings::CanvasLineCap::Butt:
        return Gfx::Path::CapStyle::Butt;
    case Bindings::CanvasLineCap::Round:
        return Gfx::Path::CapStyle::Round;
    case Bindings::CanvasLineCap::Square:
        return Gfx::Path::CapStyle::Square;
    }
    VERIFY_NOT_REACHED();
}

static Gfx::Path::JoinStyle to_gfx_join(Bindings::CanvasLineJoin const& join_style)
{
    switch (join_style) {
    case Bindings::CanvasLineJoin::Round:
        return Gfx::Path::JoinStyle::Round;
    case Bindings::CanvasLineJoin::Bevel:
        return Gfx::Path::JoinStyle::Bevel;
    case Bindings::CanvasLineJoin::Miter:
        return Gfx::Path::JoinStyle::Miter;
    }

    VERIFY_NOT_REACHED();
}

static bool transparent_source_paint_can_be_ignored(Gfx::CompositingAndBlendingOperator compositing_and_blending_operator)
{
    // https://html.spec.whatwg.org/multipage/canvas.html#drawing-model
    // Composite B within the clipping region over the current output bitmap using the current compositing and blending operator.
    return compositing_and_blending_operator == Gfx::CompositingAndBlendingOperator::SourceOver;
}

// https://html.spec.whatwg.org/multipage/canvas.html#the-canvas-settings:concept-canvas-alpha
Gfx::Color CanvasRenderingContext2D::clear_color() const
{
    return m_context_attributes.alpha ? Gfx::Color::Transparent : Gfx::Color::Black;
}

void CanvasRenderingContext2D::stroke_internal(Gfx::Path path)
{
    auto* canvas_command_list = this->canvas_command_list();
    if (!canvas_command_list)
        return;

    auto& state = drawing_state();
    auto paint_style = state.stroke_style.to_gfx_paint_style();
    if (!paint_style->is_visible() && transparent_source_paint_can_be_ignored(state.current_compositing_and_blending_operator))
        return;

    auto line_cap = to_gfx_cap(state.line_cap);
    auto line_join = to_gfx_join(state.line_join);
    // FIXME: Need a Vector<float> for rendering dash_array, but state.dash_list is Vector<double>.
    // Maybe possible to avoid creating copies?
    auto dash_array = Vector<float> {};
    dash_array.ensure_capacity(state.dash_list.size());
    for (auto const& dash : state.dash_list) {
        dash_array.append(static_cast<float>(dash));
    }
    paint_shadow_for_stroke_internal(path, line_cap, line_join, dash_array);
    auto bounding_box = path.bounding_box();
    canvas_command_list->append(Gfx::CanvasCommands::StrokePath {
        .path = move(path),
        .style = Gfx::to_canvas_paint_style(*paint_style),
        .thickness = state.line_width,
        .cap_style = line_cap,
        .join_style = line_join,
        .miter_limit = state.miter_limit,
        .dash_array = move(dash_array),
        .dash_offset = state.line_dash_offset,
        .filter = state.filter,
        .global_alpha = state.global_alpha,
        .compositing_and_blending_operator = state.current_compositing_and_blending_operator,
    });

    did_draw(bounding_box);
}

void CanvasRenderingContext2D::stroke()
{
    stroke_internal(path().clone());
}

void CanvasRenderingContext2D::stroke(Path2D const& path)
{
    stroke_internal(path.path().clone());
}

static Gfx::WindingRule parse_fill_rule(StringView fill_rule)
{
    if (fill_rule == "evenodd"sv)
        return Gfx::WindingRule::EvenOdd;
    if (fill_rule == "nonzero"sv)
        return Gfx::WindingRule::Nonzero;
    dbgln("Unrecognized fillRule for CRC2D.fill() - this problem goes away once we pass an enum instead of a string");
    return Gfx::WindingRule::Nonzero;
}

void CanvasRenderingContext2D::fill_internal(Gfx::Path path, Gfx::WindingRule winding_rule)
{
    auto* canvas_command_list = this->canvas_command_list();
    if (!canvas_command_list)
        return;

    auto& state = this->drawing_state();
    auto paint_style = state.fill_style.to_gfx_paint_style();
    if (!paint_style->is_visible() && transparent_source_paint_can_be_ignored(state.current_compositing_and_blending_operator))
        return;

    paint_shadow_for_fill_internal(path, winding_rule);

    auto bounding_box = path.bounding_box();
    canvas_command_list->append(Gfx::CanvasCommands::FillPath {
        .path = move(path),
        .style = Gfx::to_canvas_paint_style(*paint_style),
        .winding_rule = winding_rule,
        .filter = state.filter,
        .global_alpha = state.global_alpha,
        .compositing_and_blending_operator = state.current_compositing_and_blending_operator,
    });

    did_draw(bounding_box);
}

void CanvasRenderingContext2D::fill(StringView fill_rule)
{
    fill_internal(path().clone(), parse_fill_rule(fill_rule));
}

void CanvasRenderingContext2D::fill(Path2D& path, StringView fill_rule)
{
    fill_internal(path.path().clone(), parse_fill_rule(fill_rule));
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createimagedata
WebIDL::ExceptionOr<GC::Ref<ImageData>> CanvasRenderingContext2D::create_image_data(int width, int height, Optional<Bindings::ImageDataSettings> const& settings) const
{
    // 1. If one or both of sw and sh are zero, then throw an "IndexSizeError" DOMException.
    if (width == 0 || height == 0)
        return WebIDL::IndexSizeError::create(realm(), "Width and height must not be zero"_utf16);

    int abs_width = abs(width);
    int abs_height = abs(height);

    // 2. Let newImageData be a new ImageData object.
    // 3. Initialize newImageData given the absolute magnitude of sw, the absolute magnitude of sh, settings set to settings, and defaultColorSpace set to this's color space.
    auto image_data = TRY(ImageData::create(realm(), abs_width, abs_height, settings));

    // 4. Initialize the image data of newImageData to transparent black.
    // ... this is handled by ImageData::create()

    // 5. Return newImageData.
    return image_data;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createimagedata-imagedata
WebIDL::ExceptionOr<GC::Ref<ImageData>> CanvasRenderingContext2D::create_image_data(ImageData const& image_data) const
{
    // 1. Let newImageData be a new ImageData object.
    // 2. Initialize newImageData given the value of imageData's width attribute, the value of imageData's height attribute, and defaultColorSpace set to the value of imageData's colorSpace attribute.
    // FIXME: Set defaultColorSpace to the value of image_data's colorSpace attribute
    // 3. Initialize the image data of newImageData to transparent black.
    // NOTE: No-op, already done during creation.
    // 4. Return newImageData.
    return TRY(ImageData::create(realm(), image_data.width(), image_data.height()));
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-getimagedata
WebIDL::ExceptionOr<GC::Ptr<ImageData>> CanvasRenderingContext2D::get_image_data(int x, int y, int width, int height, Optional<Bindings::ImageDataSettings> const& settings)
{
    // 1. If either the sw or sh arguments are zero, then throw an "IndexSizeError" DOMException.
    if (width == 0 || height == 0)
        return WebIDL::IndexSizeError::create(realm(), "Width and height must not be zero"_utf16);

    // 2. If the CanvasRenderingContext2D's origin-clean flag is set to false, then throw a "SecurityError" DOMException.
    if (!m_origin_clean)
        return WebIDL::SecurityError::create(realm(), "CanvasRenderingContext2D is not origin-clean"_utf16);

    // ImageData initialization requires positive width and height
    // https://html.spec.whatwg.org/multipage/canvas.html#initialize-an-imagedata-object
    int abs_width = abs(width);
    int abs_height = abs(height);

    // 3. Let imageData be a new ImageData object.
    // 4. Initialize imageData given sw, sh, settings set to settings, and defaultColorSpace set to this's color space.
    auto image_data = TRY(ImageData::create(realm(), abs_width, abs_height, settings));

    // 5. Let the source rectangle be the rectangle whose corners are the four points (sx, sy), (sx+sw, sy), (sx+sw, sy+sh), (sx, sy+sh).
    auto source_rect = Gfx::Rect { x, y, abs_width, abs_height };

    // NOTE: The spec doesn't seem to define this behavior, but MDN does and the WPT tests
    // assume it works this way.
    // https://developer.mozilla.org/en-US/docs/Web/API/CanvasRenderingContext2D/getImageData#sw
    if (width < 0 || height < 0) {
        source_rect = source_rect.translated(min(width, 0), min(height, 0));
    }
    auto source_rect_intersected = source_rect.intersected(Gfx::IntRect { {}, m_size });
    if (source_rect_intersected.is_empty())
        return image_data;

    // NOTE: If reading back from the Compositor fails (no backing storage or no connection),
    //       it's like copying only transparent black pixels (which is a no-op).
    auto pixels = read_pixels(source_rect_intersected);
    if (!pixels)
        return image_data;
    auto const snapshot = Gfx::DecodedImageFrame { *pixels };

    // 6. Set the pixel values of imageData to be the pixels of this's output bitmap in the area specified by the source rectangle in the bitmap's coordinate space units, converted from this's color space to imageData's colorSpace using 'relative-colorimetric' rendering intent.
    // NOTE: Internally we must use premultiplied alpha, but ImageData should hold unpremultiplied alpha. This conversion
    //       might result in a loss of precision, but is according to spec.
    //       See: https://html.spec.whatwg.org/multipage/canvas.html#premultiplied-alpha-and-the-2d-rendering-context
    VERIFY(snapshot.bitmap().alpha_type() == Gfx::AlphaType::Premultiplied);
    VERIFY(image_data->bitmap().alpha_type() == Gfx::AlphaType::Unpremultiplied);

    auto painter = Gfx::Painter::create(image_data->bitmap());
    painter->draw_bitmap(image_data->bitmap().rect().to_type<float>(), snapshot, snapshot.rect(), Gfx::ScalingMode::NearestNeighbor, {}, 1, Gfx::CompositingAndBlendingOperator::SourceOver);

    // 7. Set the pixels values of imageData for areas of the source rectangle that are outside of the output bitmap to transparent black.
    // NOTE: No-op, already done during creation.

    // 8. Return imageData.
    return image_data;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-putimagedata-short
WebIDL::ExceptionOr<void> CanvasRenderingContext2D::put_image_data(ImageData& image_data, float dx, float dy)
{
    // The putImageData(imageData, dx, dy) method steps are to put pixels from an ImageData onto a bitmap,
    // given imageData, this's output bitmap, dx, dy, 0, 0, imageData's width, and imageData's height.
    if (auto* canvas_command_list = this->canvas_command_list())
        TRY(put_pixels_from_an_image_data_onto_a_bitmap(image_data, *canvas_command_list, dx, dy, 0, 0, image_data.width(), image_data.height()));

    return {};
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-putimagedata
WebIDL::ExceptionOr<void> CanvasRenderingContext2D::put_image_data(ImageData& image_data, float x, float y, float dirty_x, float dirty_y, float dirty_width, float dirty_height)
{
    // The putImageData(imageData, dx, dy, dirtyX, dirtyY, dirtyWidth, dirtyHeight) method steps are to put pixels
    // from an ImageData onto a bitmap, given imageData, this's output bitmap, dx, dy, dirtyX, dirtyY, dirtyWidth, and
    // dirtyHeight.
    if (auto* canvas_command_list = this->canvas_command_list())
        TRY(put_pixels_from_an_image_data_onto_a_bitmap(image_data, *canvas_command_list, x, y, dirty_x, dirty_y, dirty_width, dirty_height));

    return {};
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context2d-putimagedata-common
WebIDL::ExceptionOr<void> CanvasRenderingContext2D::put_pixels_from_an_image_data_onto_a_bitmap(ImageData& image_data, Gfx::CanvasCommandList& canvas_command_list, float dx, float dy, float dirty_x, float dirty_y, float dirty_width, float dirty_height)
{
    // 1. Let buffer be imageData's data attribute value's [[ViewedArrayBuffer]] internal slot.
    auto* buffer = image_data.data()->viewed_array_buffer();

    // 2. If IsDetachedBuffer(buffer) is true, then throw an "InvalidStateError" DOMException
    if (buffer->is_detached())
        return WebIDL::InvalidStateError::create(image_data.realm(), "ImageData's underlying buffer is detached"_utf16);

    // 3. If dirtyWidth is negative, then let dirtyX be dirtyX+dirtyWidth, and let dirtyWidth be equal to the
    //    absolute magnitude of dirtyWidth.
    if (dirty_width < 0) {
        dirty_x += dirty_width;
        dirty_width = abs(dirty_width);
    }
    // If dirtyHeight is negative, then let dirtyY be dirtyY+dirtyHeight, and let dirtyHeight be equal to the absolute
    // magnitude of dirtyHeight.
    if (dirty_height < 0) {
        dirty_y += dirty_height;
        dirty_height = abs(dirty_height);
    }

    // 4. If dirtyX is negative, then let dirtyWidth be dirtyWidth+dirtyX, and let dirtyX be 0.
    if (dirty_x < 0) {
        dirty_width += dirty_x;
        dirty_x = 0;
    }

    // If dirtyY is negative, then let dirtyHeight be dirtyHeight+dirtyY, and let dirtyY be 0.
    if (dirty_y < 0) {
        dirty_height += dirty_y;
        dirty_y = 0;
    }

    // 5. If dirtyX+dirtyWidth is greater than the width attribute of the imageData argument, then let dirtyWidth be
    //    the value of that width attribute, minus the value of dirtyX.
    if (dirty_x + dirty_width > image_data.width()) {
        dirty_width = image_data.width() - dirty_x;
    }
    // If dirtyY+dirtyHeight is greater than the height attribute of the imageData argument, then let dirtyHeight be
    // the value of that height attribute, minus the value of dirtyY.
    if (dirty_y + dirty_height > image_data.height()) {
        dirty_height = image_data.height() - dirty_y;
    }

    // 6. If, after those changes, either dirtyWidth or dirtyHeight are negative or zero, then return without affecting
    //    any bitmaps.
    if (dirty_width <= 0 || dirty_height <= 0)
        return {};

    // 7. For all integer values of x and y where dirtyX ≤ x < dirtyX+dirtyWidth and dirtyY ≤ y < dirtyY+dirtyHeight,
    //    set the pixel with coordinate (dx+x, dy+y) in bitmap to the color of the pixel at coordinate (x, y) in the
    //    imageData data structure's bitmap, converted from imageData's colorSpace to the color space of bitmap using
    //    'relative-colorimetric' rendering intent.
    auto dst_rect = Gfx::FloatRect { dx + dirty_x, dy + dirty_y, dirty_width, dirty_height };
    // NOTE: The ImageData buffer is mutable from script and the recorded draw is only
    //       serialized at flush time, so snapshot the dirty pixels at call time. The
    //       snapshot is shareable so that flushing to the Compositor passes the pixels
    //       by file descriptor instead of copying them again.
    auto source_rect = Gfx::IntRect { dirty_x, dirty_y, dirty_width, dirty_height };
    auto const& source_bitmap = image_data.bitmap();
    auto bitmap_snapshot = MUST(Gfx::Bitmap::create_shareable(source_bitmap.format(), source_bitmap.alpha_type(), source_rect.size()));
    for (int y = 0; y < source_rect.height(); ++y)
        __builtin_memcpy(bitmap_snapshot->scanline(y), source_bitmap.scanline(source_rect.y() + y) + source_rect.x(), static_cast<size_t>(source_rect.width()) * sizeof(Gfx::RawPixel));
    canvas_command_list.append(Gfx::CanvasCommands::Save {});
    canvas_command_list.append(Gfx::CanvasCommands::SetTransform { .transform = {} });
    canvas_command_list.append(Gfx::CanvasCommands::DrawBitmap {
        .frame = Gfx::DecodedImageFrame { *bitmap_snapshot, Gfx::AlphaType::Unpremultiplied },
        .dst_rect = dst_rect,
        .src_rect = bitmap_snapshot->rect(),
        .filter = {},
    });
    canvas_command_list.append(Gfx::CanvasCommands::Restore {});

    did_draw(dst_rect);

    return {};
}

// https://html.spec.whatwg.org/multipage/canvas.html#reset-the-rendering-context-to-its-default-state
void CanvasRenderingContext2D::reset_to_default_state()
{
    auto* canvas_command_list = has_backing_storage() ? this->canvas_command_list() : nullptr;

    // 1. Clear canvas's bitmap to transparent black.
    if (canvas_command_list)
        canvas_command_list->append(Gfx::CanvasCommands::ClearRect { .rect = Gfx::FloatRect { {}, m_size.to_type<float>() }, .color = clear_color() });

    // 2. Empty the list of subpaths in context's current default path.
    path().clear();

    // 3. Clear the context's drawing state stack.
    clear_drawing_state_stack();

    // 4. Reset everything that drawing state consists of to their initial values.
    reset_drawing_state();

    if (canvas_command_list) {
        canvas_command_list->append(Gfx::CanvasCommands::Reset {});
        did_draw(Gfx::FloatRect { {}, m_size.to_type<float>() });
    }
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-measuretext
GC::Ref<TextMetrics> CanvasRenderingContext2D::measure_text(Utf16String const& text)
{
    // The measureText(text) method steps are to run the text preparation
    // algorithm, passing it text and the object implementing the CanvasText
    // interface, and then using the returned inline box return a new
    // TextMetrics object with members behaving as described in the following
    // list:
    auto prepared_text = prepare_text(text);
    auto metrics = TextMetrics::create(realm());
    // FIXME: Use the font that was used to create the glyphs in prepared_text.
    auto const& font = font_cascade_list()->first();
    auto const& font_pixel_metrics = font.pixel_metrics();
    auto const ascent = font_pixel_metrics.ascent;
    auto const descent = font_pixel_metrics.descent;
    auto const hanging_baseline = ascent * 0.8f;

    float baseline_offset = 0;
    switch (drawing_state().text_baseline) {
    case Bindings::CanvasTextBaseline::Top:
        baseline_offset = ascent;
        break;
    case Bindings::CanvasTextBaseline::Hanging:
        baseline_offset = hanging_baseline;
        break;
    case Bindings::CanvasTextBaseline::Middle:
        baseline_offset = (ascent - descent) / 2.0f;
        break;
    case Bindings::CanvasTextBaseline::Alphabetic:
        baseline_offset = 0;
        break;
    case Bindings::CanvasTextBaseline::Ideographic:
    case Bindings::CanvasTextBaseline::Bottom:
        baseline_offset = -descent;
        break;
    }

    // width attribute: The width of that inline box, in CSS pixels. (The text's advance width.)
    metrics->set_width(prepared_text.bounding_box.width());
    // actualBoundingBoxLeft attribute: The distance parallel to the baseline from the alignment point given by the textAlign attribute to the left side of the bounding rectangle of the given text, in CSS pixels; positive numbers indicating a distance going left from the given alignment point.
    metrics->set_actual_bounding_box_left(-prepared_text.bounding_box.left());
    // actualBoundingBoxRight attribute: The distance parallel to the baseline from the alignment point given by the textAlign attribute to the right side of the bounding rectangle of the given text, in CSS pixels; positive numbers indicating a distance going right from the given alignment point.
    metrics->set_actual_bounding_box_right(prepared_text.bounding_box.right());
    // fontBoundingBoxAscent attribute: The distance from the horizontal line indicated by the textBaseline attribute to the ascent metric of the first available font, in CSS pixels; positive numbers indicating a distance going up from the given baseline.
    metrics->set_font_bounding_box_ascent(ascent - baseline_offset);
    // fontBoundingBoxDescent attribute: The distance from the horizontal line indicated by the textBaseline attribute to the descent metric of the first available font, in CSS pixels; positive numbers indicating a distance going down from the given baseline.
    metrics->set_font_bounding_box_descent(descent + baseline_offset);
    // actualBoundingBoxAscent attribute: The distance from the horizontal line indicated by the textBaseline attribute to the top of the bounding rectangle of the given text, in CSS pixels; positive numbers indicating a distance going up from the given baseline.
    metrics->set_actual_bounding_box_ascent(ascent - baseline_offset);
    // actualBoundingBoxDescent attribute: The distance from the horizontal line indicated by the textBaseline attribute to the bottom of the bounding rectangle of the given text, in CSS pixels; positive numbers indicating a distance going down from the given baseline.
    metrics->set_actual_bounding_box_descent(descent + baseline_offset);
    // emHeightAscent attribute: The distance from the horizontal line indicated by the textBaseline attribute to the highest top of the em squares in the inline box, in CSS pixels; positive numbers indicating that the given baseline is below the top of that em square (so this value will usually be positive). Zero if the given baseline is the top of that em square; half the font size if the given baseline is the middle of that em square.
    metrics->set_em_height_ascent(ascent - baseline_offset);
    // emHeightDescent attribute: The distance from the horizontal line indicated by the textBaseline attribute to the lowest bottom of the em squares in the inline box, in CSS pixels; positive numbers indicating that the given baseline is above the bottom of that em square. (Zero if the given baseline is the bottom of that em square.)
    metrics->set_em_height_descent(descent + baseline_offset);
    // hangingBaseline attribute: The distance from the horizontal line indicated by the textBaseline attribute to the hanging baseline of the inline box, in CSS pixels; positive numbers indicating that the given baseline is below the hanging baseline. (Zero if the given baseline is the hanging baseline.)
    metrics->set_hanging_baseline(hanging_baseline - baseline_offset);
    // alphabeticBaseline attribute: The distance from the horizontal line indicated by the textBaseline attribute to the alphabetic baseline of the inline box, in CSS pixels; positive numbers indicating that the given baseline is below the alphabetic baseline. (Zero if the given baseline is the alphabetic baseline.)
    metrics->set_alphabetic_baseline(-baseline_offset);
    // ideographicBaseline attribute: The distance from the horizontal line indicated by the textBaseline attribute to the ideographic-under baseline of the inline box, in CSS pixels; positive numbers indicating that the given baseline is below the ideographic-under baseline. (Zero if the given baseline is the ideographic-under baseline.)
    metrics->set_ideographic_baseline(-descent - baseline_offset);

    return metrics;
}

RefPtr<Gfx::FontCascadeList const> CanvasRenderingContext2D::font_cascade_list()
{
    // When font style value is empty load default font
    if (!drawing_state().font_style_value) {
        set_font("10px sans-serif"sv);
    }

    // Get current loaded font
    return drawing_state().current_font_cascade_list;
}

// https://html.spec.whatwg.org/multipage/canvas.html#text-preparation-algorithm
CanvasRenderingContext2D::PreparedText CanvasRenderingContext2D::prepare_text(Utf16String const& text, float max_width)
{
    // 1. If maxWidth was provided but is less than or equal to zero or equal to NaN, then return an empty array.
    if (max_width <= 0 || max_width != max_width) {
        return {};
    }

    // 2. Replace all ASCII whitespace in text with U+0020 SPACE characters.
    Utf16StringBuilder builder { text.length_in_code_units() };
    for (auto c : text) {
        builder.append_code_point(Infra::is_ascii_whitespace(c) ? ' ' : c);
    }
    auto replaced_text = builder.to_string();

    // 3. Let font be the current font of target, as given by that object's font attribute.
    auto glyph_runs = Gfx::shape_text({ 0, 0 }, replaced_text.utf16_view(), *font_cascade_list(), resolved_letter_spacing());

    // FIXME: 4. Let language be the target's language.
    // FIXME: 5. If language is "inherit":
    //           ...
    // FIXME: 6. If language is the empty string, then set language to explicitly unknown.

    // FIXME: 7. Apply the appropriate step from the following list to determine the value of direction:
    //           ...

    // 8. Form a hypothetical infinitely-wide CSS line box containing a single inline box containing the text text,
    //    with the CSS content language set to language, and with its CSS properties set as follows:
    //   'direction'         -> direction
    //   'font'              -> font
    //   'font-kerning'      -> target's fontKerning
    //   'font-stretch'      -> target's fontStretch
    //   'font-variant-caps' -> target's fontVariantCaps
    //   'letter-spacing'    -> target's letterSpacing
    //   SVG text-rendering  -> target's textRendering
    //   'white-space'       -> 'pre'
    //   'word-spacing'      -> target's wordSpacing
    // ...and with all other properties set to their initial values.
    // FIXME: Actually use a LineBox here instead of, you know, using the default font and measuring its size (which is not the spec at all).
    // FIXME: Once we have CanvasTextDrawingStyles, add the CSS attributes.
    float height = 0;
    float width = 0;
    for (auto const& glyph_run : glyph_runs) {
        height = max(height, glyph_run->font().pixel_size());
        width += glyph_run->width();
    }

    // 9. If maxWidth was provided and the hypothetical width of the inline box in the hypothetical line box is greater than maxWidth CSS pixels, then change font to have a more condensed font (if one is available or if a reasonably readable one can be synthesized by applying a horizontal scale factor to the font) or a smaller font, and return to the previous step.
    // FIXME: Record the font size used for this piece of text, and actually retry with a smaller size if needed.

    // FIXME: 10. The anchor point is a point on the inline box, and the physical alignment is one of the values left, right,
    //            and center. These variables are determined by the textAlign and textBaseline values as follows:
    //            ...

    // 11. Let result be an array constructed by iterating over each glyph in the inline box from left to right (if
    //     any), adding to the array, for each glyph, the shape of the glyph as it is in the inline box, positioned on
    //     a coordinate space using CSS pixels with its origin is at the anchor point.
    PreparedText prepared_text { move(glyph_runs), Gfx::TextAlignment::CenterLeft, { 0, 0, width, height } };

    // 12. Return result, physical alignment, and the inline box.
    return prepared_text;
}

void CanvasRenderingContext2D::clip_internal(Gfx::Path& path, Gfx::WindingRule winding_rule)
{
    auto* canvas_command_list = this->canvas_command_list();
    if (!canvas_command_list)
        return;

    canvas_command_list->append(Gfx::CanvasCommands::ClipPath { .path = path.clone(), .winding_rule = winding_rule });
}

void CanvasRenderingContext2D::clip(StringView fill_rule)
{
    clip_internal(path(), parse_fill_rule(fill_rule));
}

void CanvasRenderingContext2D::clip(Path2D& path, StringView fill_rule)
{
    clip_internal(path.path(), parse_fill_rule(fill_rule));
}

static bool is_point_in_path_internal(Gfx::Path path, Gfx::AffineTransform const& transform, double x, double y, StringView fill_rule)
{
    auto point = Gfx::FloatPoint(x, y);
    if (auto inverse_transform = transform.inverse(); inverse_transform.has_value())
        point = inverse_transform->map(point);
    return path.contains(point, parse_fill_rule(fill_rule));
}

bool CanvasRenderingContext2D::is_point_in_path(double x, double y, StringView fill_rule)
{
    return is_point_in_path_internal(path(), drawing_state().transform, x, y, fill_rule);
}

bool CanvasRenderingContext2D::is_point_in_path(Path2D const& path, double x, double y, StringView fill_rule)
{
    return is_point_in_path_internal(path.path(), drawing_state().transform, x, y, fill_rule);
}

// https://html.spec.whatwg.org/multipage/canvas.html#check-the-usability-of-the-image-argument
WebIDL::ExceptionOr<CanvasImageSourceUsability> check_usability_of_image(CanvasImageSource const& image)
{
    // 1. Switch on image:
    auto usability = TRY(image.visit(
        // HTMLOrSVGImageElement
        [](GC::Ref<HTMLImageElement> image_element) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            // If image's current request's state is broken, then throw an "InvalidStateError" DOMException.
            if (image_element->current_request().state() == HTML::ImageRequest::State::Broken)
                return WebIDL::InvalidStateError::create(image_element->realm(), "Image element state is broken"_utf16);

            // If image is not fully decodable, then return bad.
            auto current_image_frame = image_element->current_image_frame();
            if (!current_image_frame.has_value())
                return { CanvasImageSourceUsability::Bad };

            // If image has an intrinsic width or intrinsic height (or both) equal to zero, then return bad.
            if (current_image_frame->width() == 0 || current_image_frame->height() == 0)
                return { CanvasImageSourceUsability::Bad };
            return Optional<CanvasImageSourceUsability> {};
        },
        // FIXME: Don't duplicate this for HTMLImageElement and SVGImageElement.
        [](GC::Ref<SVG::SVGImageElement> image_element) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            // FIXME: If image's current request's state is broken, then throw an "InvalidStateError" DOMException.

            // If image is not fully decodable, then return bad.
            auto current_image_frame = image_element->current_image_frame();
            if (!current_image_frame.has_value())
                return { CanvasImageSourceUsability::Bad };

            // If image has an intrinsic width or intrinsic height (or both) equal to zero, then return bad.
            if (current_image_frame->width() == 0 || current_image_frame->height() == 0)
                return { CanvasImageSourceUsability::Bad };
            return Optional<CanvasImageSourceUsability> {};
        },

        [](GC::Ref<HTML::HTMLVideoElement> video_element) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            // If image's readyState attribute is either HAVE_NOTHING or HAVE_METADATA, then return bad.
            if (video_element->ready_state() == HTML::HTMLMediaElement::ReadyState::HaveNothing || video_element->ready_state() == HTML::HTMLMediaElement::ReadyState::HaveMetadata) {
                return { CanvasImageSourceUsability::Bad };
            }
            return Optional<CanvasImageSourceUsability> {};
        },

        // OffscreenCanvas
        [](GC::Ref<OffscreenCanvas> offscreen_canvas) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            // If image has either a horizontal dimension or a vertical dimension equal to zero, then throw an "InvalidStateError" DOMException.
            if (offscreen_canvas->width() == 0 || offscreen_canvas->height() == 0)
                return WebIDL::InvalidStateError::create(offscreen_canvas->realm(), "OffscreenCanvas width or height is zero"_utf16);
            return Optional<CanvasImageSourceUsability> {};
        },
        // HTMLCanvasElement
        [](GC::Ref<HTMLCanvasElement> canvas_element) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            // If image has either a horizontal dimension or a vertical dimension equal to zero, then throw an "InvalidStateError" DOMException.
            if (canvas_element->width() == 0 || canvas_element->height() == 0)
                return WebIDL::InvalidStateError::create(canvas_element->realm(), "Canvas width or height is zero"_utf16);
            return Optional<CanvasImageSourceUsability> {};
        },

        // ImageBitmap
        // FIXME: VideoFrame
        [](GC::Ref<ImageBitmap> image_bitmap) -> WebIDL::ExceptionOr<Optional<CanvasImageSourceUsability>> {
            if (image_bitmap->is_detached())
                return WebIDL::InvalidStateError::create(image_bitmap->realm(), "Image bitmap is detached"_utf16);
            return Optional<CanvasImageSourceUsability> {};
        }));
    if (usability.has_value())
        return usability.release_value();

    // 2. Return good.
    return { CanvasImageSourceUsability::Good };
}

// https://html.spec.whatwg.org/multipage/canvas.html#the-image-argument-is-not-origin-clean
bool image_is_not_origin_clean(CanvasImageSource const& image)
{
    // An object image is not origin-clean if, switching on image's type:
    return image.visit(
        // HTMLOrSVGImageElement
        [](GC::Ref<HTMLImageElement> image) {
            // image's current request's image data is CORS-cross-origin.
            return image->current_request().image_data()->is_cors_cross_origin();
        },
        [](GC::Ref<SVG::SVGImageElement>) {
            // FIXME: image's current request's image data is CORS-cross-origin.
            return false;
        },
        [](GC::Ref<HTML::HTMLVideoElement>) {
            // FIXME: image's media data is CORS-cross-origin.
            return false;
        },
        // HTMLCanvasElement, ImageBitmap or OffscreenCanvas
        [](OneOf<GC::Ref<HTMLCanvasElement>, GC::Ref<ImageBitmap>, GC::Ref<OffscreenCanvas>> auto const&) {
            // FIXME: image's bitmap's origin-clean flag is false.
            return false;
        });
}

bool CanvasRenderingContext2D::image_smoothing_enabled() const
{
    return drawing_state().image_smoothing_enabled;
}

void CanvasRenderingContext2D::set_image_smoothing_enabled(bool enabled)
{
    drawing_state().image_smoothing_enabled = enabled;
}

Bindings::ImageSmoothingQuality CanvasRenderingContext2D::image_smoothing_quality() const
{
    return drawing_state().image_smoothing_quality;
}

void CanvasRenderingContext2D::set_image_smoothing_quality(Bindings::ImageSmoothingQuality quality)
{
    drawing_state().image_smoothing_quality = quality;
}

float CanvasRenderingContext2D::global_alpha() const
{
    return drawing_state().global_alpha;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-globalalpha
void CanvasRenderingContext2D::set_global_alpha(float alpha)
{
    // 1. If the given value is either infinite, NaN, or not in the range 0.0 to 1.0, then return.
    if (!isfinite(alpha) || alpha < 0.0f || alpha > 1.0f) {
        return;
    }
    // 2. Otherwise, set this's global alpha to the given value.
    drawing_state().global_alpha = alpha;
}

#define ENUMERATE_COMPOSITE_OPERATIONS(E)  \
    E("normal", Normal)                    \
    E("multiply", Multiply)                \
    E("screen", Screen)                    \
    E("overlay", Overlay)                  \
    E("darken", Darken)                    \
    E("lighten", Lighten)                  \
    E("color-dodge", ColorDodge)           \
    E("color-burn", ColorBurn)             \
    E("hard-light", HardLight)             \
    E("soft-light", SoftLight)             \
    E("difference", Difference)            \
    E("exclusion", Exclusion)              \
    E("hue", Hue)                          \
    E("saturation", Saturation)            \
    E("color", Color)                      \
    E("luminosity", Luminosity)            \
    E("clear", Clear)                      \
    E("copy", Copy)                        \
    E("source-over", SourceOver)           \
    E("destination-over", DestinationOver) \
    E("source-in", SourceIn)               \
    E("destination-in", DestinationIn)     \
    E("source-out", SourceOut)             \
    E("destination-out", DestinationOut)   \
    E("source-atop", SourceATop)           \
    E("destination-atop", DestinationATop) \
    E("xor", Xor)                          \
    E("lighter", Lighter)                  \
    E("plus-darker", PlusDarker)           \
    E("plus-lighter", PlusLighter)

String CanvasRenderingContext2D::global_composite_operation() const
{
    auto current_compositing_and_blending_operator = drawing_state().current_compositing_and_blending_operator;
    switch (current_compositing_and_blending_operator) {
#undef __ENUMERATE
#define __ENUMERATE(operation, compositing_and_blending_operator)                \
    case Gfx::CompositingAndBlendingOperator::compositing_and_blending_operator: \
        return operation##_string;
        ENUMERATE_COMPOSITE_OPERATIONS(__ENUMERATE)
#undef __ENUMERATE
    default:
        VERIFY_NOT_REACHED();
    }
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-globalcompositeoperation
void CanvasRenderingContext2D::set_global_composite_operation(String global_composite_operation)
{
    // 1. If the given value is not identical to any of the values that the <blend-mode> or the <composite-mode> properties are defined to take, then return.
    // 2. Otherwise, set this's current compositing and blending operator to the given value.
#undef __ENUMERATE
#define __ENUMERATE(operation, compositing_and_blending_operator)                                                                           \
    if (global_composite_operation == operation##sv) {                                                                                      \
        drawing_state().current_compositing_and_blending_operator = Gfx::CompositingAndBlendingOperator::compositing_and_blending_operator; \
        return;                                                                                                                             \
    }
    ENUMERATE_COMPOSITE_OPERATIONS(__ENUMERATE)
#undef __ENUMERATE
}

float CanvasRenderingContext2D::shadow_offset_x() const
{
    return drawing_state().shadow_offset_x;
}

void CanvasRenderingContext2D::set_shadow_offset_x(float offset_x)
{
    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowoffsetx
    if (!isfinite(offset_x))
        return;

    drawing_state().shadow_offset_x = offset_x;
}

float CanvasRenderingContext2D::shadow_offset_y() const
{
    return drawing_state().shadow_offset_y;
}

void CanvasRenderingContext2D::set_shadow_offset_y(float offset_y)
{
    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowoffsety
    if (!isfinite(offset_y))
        return;

    drawing_state().shadow_offset_y = offset_y;
}

float CanvasRenderingContext2D::shadow_blur() const
{
    return drawing_state().shadow_blur;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowblur
void CanvasRenderingContext2D::set_shadow_blur(float blur_radius)
{
    // On setting, the attribute must be set to the new value,
    // except if the value is negative, infinite or NaN, in which case the new value must be ignored.
    if (blur_radius < 0 || isinf(blur_radius) || isnan(blur_radius))
        return;

    drawing_state().shadow_blur = blur_radius;
}

String CanvasRenderingContext2D::shadow_color() const
{
    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowcolor
    return drawing_state().shadow_color.to_string(Gfx::Color::HTMLCompatibleSerialization::Yes);
}

void CanvasRenderingContext2D::set_shadow_color(String color)
{
    // 1. Let context be this's canvas attribute's value, if that is an element; otherwise null.

    // 2. Let parsedValue be the result of parsing the given value with context if non-null.
    auto parsed_value = parse_a_css_color_value(color);

    // 3. If parsedValue is failure, then return.
    if (!parsed_value.has_value())
        return;

    // 4. Set this's shadow color to parsedValue.
    drawing_state().shadow_color = parsed_value.value();
}
void CanvasRenderingContext2D::paint_shadow_for_fill_internal(Gfx::Path const& path, Gfx::WindingRule winding_rule)
{
    auto* canvas_command_list = this->canvas_command_list();
    if (!canvas_command_list)
        return;

    auto& state = this->drawing_state();
    if (state.shadow_blur == 0.0f && state.shadow_offset_x == 0.0f && state.shadow_offset_y == 0.0f)
        return;

    if (state.current_compositing_and_blending_operator == Gfx::CompositingAndBlendingOperator::Copy)
        return;

    auto alpha = state.global_alpha * (state.shadow_color.alpha() / 255.0f);
    auto fill_style_color = state.fill_style.as_color();
    if (fill_style_color.has_value() && fill_style_color->alpha() > 0)
        alpha = (fill_style_color->alpha() / 255.0f) * state.global_alpha;
    if (alpha == 0.0f)
        return;

    canvas_command_list->append(Gfx::CanvasCommands::Save {});

    Gfx::AffineTransform transform;
    transform.translate(state.shadow_offset_x, state.shadow_offset_y);
    transform.multiply(state.transform);
    canvas_command_list->append(Gfx::CanvasCommands::SetTransform { .transform = transform });
    canvas_command_list->append(Gfx::CanvasCommands::FillPath {
        .path = path.clone(),
        .style = state.shadow_color.with_opacity(alpha),
        .winding_rule = winding_rule,
        .blur_radius = state.shadow_blur,
        .filter = state.filter,
        .compositing_and_blending_operator = state.current_compositing_and_blending_operator,
    });

    canvas_command_list->append(Gfx::CanvasCommands::Restore {});

    did_draw(path.bounding_box());
}

void CanvasRenderingContext2D::paint_shadow_for_stroke_internal(Gfx::Path const& path, Gfx::Path::CapStyle line_cap, Gfx::Path::JoinStyle line_join, Vector<float> const& dash_array)
{
    auto* canvas_command_list = this->canvas_command_list();
    if (!canvas_command_list)
        return;

    auto& state = drawing_state();

    if (state.current_compositing_and_blending_operator == Gfx::CompositingAndBlendingOperator::Copy)
        return;

    if (state.shadow_blur == 0.0f && state.shadow_offset_x == 0.0f && state.shadow_offset_y == 0.0f)
        return;

    auto alpha = state.global_alpha * (state.shadow_color.alpha() / 255.0f);
    auto fill_style_color = state.fill_style.as_color();
    if (fill_style_color.has_value() && fill_style_color->alpha() > 0)
        alpha = (fill_style_color->alpha() / 255.0f) * state.global_alpha;
    if (alpha == 0.0f)
        return;

    canvas_command_list->append(Gfx::CanvasCommands::Save {});

    Gfx::AffineTransform transform;
    transform.translate(state.shadow_offset_x, state.shadow_offset_y);
    transform.multiply(state.transform);
    canvas_command_list->append(Gfx::CanvasCommands::SetTransform { .transform = transform });
    canvas_command_list->append(Gfx::CanvasCommands::StrokePath {
        .path = path.clone(),
        .style = state.shadow_color.with_opacity(alpha),
        .thickness = state.line_width,
        .cap_style = line_cap,
        .join_style = line_join,
        .miter_limit = state.miter_limit,
        .dash_array = dash_array,
        .dash_offset = state.line_dash_offset,
        .blur_radius = state.shadow_blur,
        .filter = state.filter,
        .compositing_and_blending_operator = state.current_compositing_and_blending_operator,
    });

    canvas_command_list->append(Gfx::CanvasCommands::Restore {});

    did_draw(path.bounding_box());
}

String CanvasRenderingContext2D::filter() const
{
    if (!drawing_state().filter_string.has_value()) {
        return String::from_utf8_without_validation("none"sv.bytes());
    }

    return drawing_state().filter_string.value();
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-filter
void CanvasRenderingContext2D::set_filter(String filter)
{
    drawing_state().filter.clear();

    // 1. If the given value is "none", then set this's current filter to "none" and return.
    if (filter == "none"sv) {
        drawing_state().filter_string.clear();
        return;
    }

    auto parser = CSS::Parser::Parser::create(CSS::Parser::ParsingParams { CSS::Parser::SpecialContext::CanvasContextGenericValue }, filter);

    // 2. Let parsedValue be the result of parsing the given values as a <filter-value-list>.
    //    If any property-independent style sheet syntax like 'inherit' or 'initial' is present,
    //    then this parsing must return failure.
    auto style_value = parser.parse_as_css_value(CSS::PropertyID::Filter);

    if (style_value && style_value->is_filter_value_list()) {
        auto filter_value_list = style_value->absolutized(computation_context_for_drawing_state())->as_filter_value_list().filter_value_list();

        // 4. Set this's current filter to the given value.
        for (auto& item : filter_value_list) {
            // FIXME: Add support for SVG filters when they get implement by the CSS parser.
            item.visit(
                [&](CSS::FilterOperation::Blur const& blur_filter) {
                    float radius = blur_filter.resolved_radius();
                    auto new_filter = Gfx::Filter::blur(radius, radius);

                    drawing_state().filter = drawing_state().filter.has_value()
                        ? Gfx::Filter::compose(new_filter, *drawing_state().filter)
                        : new_filter;
                },
                [&](CSS::FilterOperation::Color const& color) {
                    float amount = color.resolved_amount();
                    auto new_filter = Gfx::Filter::color(color.operation, amount);

                    drawing_state().filter = drawing_state().filter.has_value()
                        ? Gfx::Filter::compose(new_filter, *drawing_state().filter)
                        : new_filter;
                },
                [&](CSS::FilterOperation::HueRotate const& hue_rotate) {
                    float angle = hue_rotate.angle_degrees();
                    auto new_filter = Gfx::Filter::hue_rotate(angle);

                    drawing_state().filter = drawing_state().filter.has_value()
                        ? Gfx::Filter::compose(new_filter, *drawing_state().filter)
                        : new_filter;
                },
                [&](CSS::FilterOperation::DropShadow const& drop_shadow) {
                    float offset_x = static_cast<float>(CSS::Length::from_style_value(drop_shadow.offset_x, {}).absolute_length_to_px());
                    float offset_y = static_cast<float>(CSS::Length::from_style_value(drop_shadow.offset_y, {}).absolute_length_to_px());

                    float radius = 0.0f;
                    if (drop_shadow.radius) {
                        radius = static_cast<float>(CSS::Length::from_style_value(*drop_shadow.radius, {}).absolute_length_to_px());
                    };

                    DOM::AbstractElement abstract_element { *m_element };
                    m_element->document().update_style_if_needed_for_element(abstract_element);

                    Gfx::Color color = Gfx::Color::Black;
                    if (drop_shadow.color && m_element->computed_properties()) {
                        auto color_context = CSS::ColorResolutionContext::for_element(abstract_element);
                        color = drop_shadow.color->to_color(color_context).value_or(Gfx::Color::Black);
                    }

                    auto new_filter = Gfx::Filter::drop_shadow(offset_x, offset_y, radius, color);

                    drawing_state().filter = drawing_state().filter.has_value()
                        ? Gfx::Filter::compose(new_filter, *drawing_state().filter)
                        : new_filter;
                },
                [&](CSS::URL const& url) {
                    (void)url;
                    // FIXME: Resolve the SVG filter
                    dbgln("FIXME: SVG filters are not implemented for Canvas2D");
                });
        }

        drawing_state().filter_string = move(filter);
    }

    // 3. If parsedValue is failure, then return.
}

}
