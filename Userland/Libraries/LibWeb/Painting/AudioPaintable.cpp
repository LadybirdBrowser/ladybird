/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/NumberFormat.h>
#include <LibGfx/AntiAliasingPainter.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/AudioTrackList.h>
#include <LibWeb/HTML/HTMLAudioElement.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/Layout/AudioBox.h>
#include <LibWeb/Painting/AudioPaintable.h>
#include <LibWeb/Painting/BorderRadiusCornerClipper.h>

namespace Web::Painting {

JS_DEFINE_ALLOCATOR(AudioPaintable);

JS::NonnullGCPtr<AudioPaintable> AudioPaintable::create(Layout::AudioBox const& layout_box)
{
    return layout_box.heap().allocate_without_realm<AudioPaintable>(layout_box);
}

AudioPaintable::AudioPaintable(Layout::AudioBox const& layout_box)
    : MediaPaintable(layout_box)
{
}

Layout::AudioBox& AudioPaintable::layout_box()
{
    return static_cast<Layout::AudioBox&>(layout_node());
}

Layout::AudioBox const& AudioPaintable::layout_box() const
{
    return static_cast<Layout::AudioBox const&>(layout_node());
}

void AudioPaintable::paint(PaintContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    Base::paint(context, phase);

    if (phase != PaintPhase::Foreground)
        return;

    DisplayListRecorderStateSaver saver { context.display_list_recorder() };

    auto audio_rect = context.rounded_device_rect(absolute_rect());
    context.display_list_recorder().add_clip_rect(audio_rect.to_type<int>());

    ScopedCornerRadiusClip corner_clip { context, audio_rect, normalized_border_radii_data(ShrinkRadiiForBorders::Yes) };

    auto const& audio_element = layout_box().dom_node();
    auto mouse_position = MediaPaintable::mouse_position(context, audio_element);

    auto paint_user_agent_controls = audio_element.has_attribute(HTML::AttributeNames::controls) || audio_element.is_scripting_disabled();

    if (paint_user_agent_controls)
        paint_media_controls(context, audio_element, audio_rect, mouse_position);
}

}
