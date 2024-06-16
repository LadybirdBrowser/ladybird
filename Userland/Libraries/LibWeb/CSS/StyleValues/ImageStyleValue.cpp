/*
 * Copyright (c) 2018-2023, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ImageStyleValue.h"
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/HTML/ImageRequest.h>
#include <LibWeb/HTML/PotentialCORSRequest.h>
#include <LibWeb/Painting/PaintContext.h>
#include <LibWeb/Painting/RecordingPainter.h>
#include <LibWeb/Platform/Timer.h>

namespace Web::CSS {

ImageStyleValue::ImageStyleValue(URL::URL const& url)
    : AbstractImageStyleValue(Type::Image)
    , m_url(url)
{
}

ImageStyleValue::~ImageStyleValue() = default;

void ImageStyleValue::load_any_resources(DOM::Document& document)
{
    if (m_image_request)
        return;
    m_document = &document;

    m_image_request = HTML::SharedImageRequest::get_or_create(document.realm(), document.page(), m_url);
    m_image_request->add_callbacks(
        [this, weak_this = make_weak_ptr()] {
            if (!weak_this)
                return;

            if (!m_document)
                return;

            // FIXME: Do less than a full repaint if possible?
            if (auto navigable = m_document->navigable())
                navigable->set_needs_display();

            auto image_data = m_image_request->image_data();
            if (image_data->is_animated() && image_data->frame_count() > 1) {
                m_timer = Platform::Timer::create();
                m_timer->set_interval(image_data->frame_duration(0));
                m_timer->on_timeout = [this] { animate(); };
                m_timer->start();
            }
        },
        nullptr);

    if (m_image_request->needs_fetching()) {
        auto request = HTML::create_potential_CORS_request(document.vm(), m_url, Fetch::Infrastructure::Request::Destination::Image, HTML::CORSSettingAttribute::NoCORS);
        request->set_client(&document.relevant_settings_object());
        m_image_request->fetch_image(document.realm(), request);
    }
}

void ImageStyleValue::animate()
{
    if (!m_image_request)
        return;
    auto image_data = m_image_request->image_data();
    if (!image_data)
        return;

    m_current_frame_index = (m_current_frame_index + 1) % image_data->frame_count();
    auto current_frame_duration = image_data->frame_duration(m_current_frame_index);

    if (current_frame_duration != m_timer->interval())
        m_timer->restart(current_frame_duration);

    if (m_current_frame_index == image_data->frame_count() - 1) {
        ++m_loops_completed;
        if (m_loops_completed > 0 && m_loops_completed == image_data->loop_count())
            m_timer->stop();
    }

    if (on_animate)
        on_animate();
}

bool ImageStyleValue::is_paintable() const
{
    return image_data();
}

Gfx::ImmutableBitmap const* ImageStyleValue::bitmap(size_t frame_index, Gfx::IntSize size) const
{
    if (auto image_data = this->image_data())
        return image_data->bitmap(frame_index, size);
    return nullptr;
}

String ImageStyleValue::to_string() const
{
    return serialize_a_url(MUST(m_url.to_string()));
}

bool ImageStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;
    return m_url == other.as_image().m_url;
}

Optional<CSSPixels> ImageStyleValue::natural_width() const
{
    if (auto image_data = this->image_data())
        return image_data->intrinsic_width();
    return {};
}

Optional<CSSPixels> ImageStyleValue::natural_height() const
{
    if (auto image_data = this->image_data())
        return image_data->intrinsic_height();
    return {};
}

Optional<CSSPixelFraction> ImageStyleValue::natural_aspect_ratio() const
{
    if (auto image_data = this->image_data())
        return image_data->intrinsic_aspect_ratio();
    return {};
}

void ImageStyleValue::paint(PaintContext& context, DevicePixelRect const& dest_rect, CSS::ImageRendering image_rendering, Vector<Gfx::Path> const& clip_paths) const
{
    if (auto const* b = bitmap(m_current_frame_index, dest_rect.size().to_type<int>()); b != nullptr) {
        auto scaling_mode = to_gfx_scaling_mode(image_rendering, b->rect(), dest_rect.to_type<int>());
        context.recording_painter().draw_scaled_immutable_bitmap(dest_rect.to_type<int>(), *b, b->rect(), scaling_mode, clip_paths);
    }
}

JS::GCPtr<HTML::DecodedImageData> ImageStyleValue::image_data() const
{
    if (!m_image_request)
        return nullptr;
    return m_image_request->image_data();
}

Optional<Gfx::Color> ImageStyleValue::color_if_single_pixel_bitmap() const
{
    if (auto const* b = bitmap(m_current_frame_index)) {
        if (b->width() == 1 && b->height() == 1)
            return b->bitmap().get_pixel(0, 0);
    }
    return {};
}

}
