/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2026, Gregory Bertilso <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class VideoPaintable final : public PaintableBox {
    GC_CELL(VideoPaintable, PaintableBox);
    GC_DECLARE_ALLOCATOR(VideoPaintable);

public:
    static GC::Ref<VideoPaintable> create(Layout::VideoBox const&);

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;

private:
    VideoPaintable(Layout::VideoBox const&);
};

}
