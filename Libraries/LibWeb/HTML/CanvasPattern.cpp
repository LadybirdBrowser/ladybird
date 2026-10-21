/*
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/DecodedImageFrame.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/HTML/Canvas/Canvas2DRecorder.h>
#include <LibWeb/HTML/Canvas/RemoteCanvas2DTransport.h>
#include <LibWeb/HTML/CanvasPattern.h>
#include <LibWeb/HTML/CanvasRenderingContext2D.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/Page/Page.h>

namespace Web::HTML {

extern "C" {

// Defined in Bindings/Rust/src/html/exports.rs.
void* web_canvas_pattern_create(JSVM*, Gfx::DecodedImageFrame const* frame, Compositing::Canvas2DPatternRepetition);
}

// A pattern outlives any one video frame, so it reads the frame back instead of holding it.
static Optional<Gfx::DecodedImageFrame> read_back_current_video_frame(HTMLVideoElement& video)
{
    auto captured_frame = video.capture_current_video_frame();
    if (!captured_frame.has_value())
        return {};
    auto& page = video.document().page();
    if (!page.has_compositor_host())
        return {};
    auto transport = page.compositor_host().create_canvas_2d_transport();
    if (!transport)
        return {};
    auto recorder = Canvas2DRecorder::create(transport.release_nonnull());

    Gfx::IntRect frame_rect { 0, 0, static_cast<int>(video.video_width()), static_cast<int>(video.video_height()) };
    if (frame_rect.is_empty() || !recorder->create_context(frame_rect.size(), true))
        return {};

    recorder->draw_video_frame(*captured_frame, frame_rect.to_type<float>(), frame_rect, Gfx::ScalingMode::NearestNeighbor,
        { .compositing_and_blending_operator = Gfx::CompositingAndBlendingOperator::Copy });
    recorder->flush();
    auto bitmap = recorder->read_back_pixels(frame_rect, Gfx::AlphaType::Premultiplied);
    recorder->destroy_context();
    if (!bitmap)
        return {};
    return Gfx::DecodedImageFrame { NonnullRefPtr<Gfx::Bitmap const> { bitmap.release_nonnull() } };
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-createpattern
WebIDL::ExceptionOr<GC::Ptr<CanvasPattern>> CanvasPattern::create(CanvasImageSource const& image, Utf16FlyString const& repetition)
{
    auto parse_repetition = [&](auto value) -> Optional<Compositing::Canvas2DPatternRepetition> {
        if (value == u"repeat"sv)
            return Compositing::Canvas2DPatternRepetition::Repeat;
        if (value == u"repeat-x"sv)
            return Compositing::Canvas2DPatternRepetition::RepeatX;
        if (value == u"repeat-y"sv)
            return Compositing::Canvas2DPatternRepetition::RepeatY;
        if (value == u"no-repeat"sv)
            return Compositing::Canvas2DPatternRepetition::NoRepeat;
        return {};
    };

    // 1. Let usability be the result of checking the usability of image.
    auto usability = TRY(check_usability_of_image(image));

    // 2. If usability is bad, then return null.
    if (usability == CanvasImageSourceUsability::Bad)
        return GC::Ptr<CanvasPattern> {};

    // 3. Assert: usability is good.
    VERIFY(usability == CanvasImageSourceUsability::Good);

    // 4. If repetition is the empty string, then set it to "repeat".
    auto repetition_or_default = repetition.is_empty() ? "repeat"_utf16_fly_string : repetition;

    // 5. If repetition is not identical to one of "repeat", "repeat-x", "repeat-y", or "no-repeat",
    // then throw a "SyntaxError" DOMException.
    auto repetition_value = parse_repetition(repetition_or_default);
    if (!repetition_value.has_value())
        return WebIDL::SyntaxError::create("Repetition value is not valid"_utf16);

    // 6. Let pattern be a new CanvasPattern object with the image image and the repetition behavior given by repetition.
    Optional<Gfx::DecodedImageFrame> frame;
    if (auto const* video = image.get_pointer<GC::Ref<HTMLVideoElement>>())
        frame = read_back_current_video_frame(*video);
    else
        frame = canvas_image_source_frame(image);
    auto* pattern = static_cast<CanvasPattern*>(web_canvas_pattern_create(JS::HostABI::vm_to_abi(JS::VM::the()), frame.has_value() ? &frame.value() : nullptr, *repetition_value));

    // FIXME: 7. If image is not origin-clean, then mark pattern as not origin-clean.

    // 8. Return pattern.
    return GC::Ptr { pattern };
}

}
