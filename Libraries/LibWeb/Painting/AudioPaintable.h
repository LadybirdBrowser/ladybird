/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Painting/MediaPaintable.h>

namespace Web::Painting {

class AudioPaintable final : public MediaPaintable {
    GC_CELL(AudioPaintable, MediaPaintable);
    GC_DECLARE_ALLOCATOR(AudioPaintable);

public:
    static GC::Ref<AudioPaintable> create(Layout::AudioBox const&);

    virtual void paint(PaintContext&, PaintPhase) const override;

    Layout::AudioBox& layout_box();
    Layout::AudioBox const& layout_box() const;

private:
    explicit AudioPaintable(Layout::AudioBox const&);
};

}
