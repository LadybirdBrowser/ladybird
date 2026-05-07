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
public:
    static NonnullRefPtr<VideoPaintable> create(Layout::VideoBox const&);
    virtual StringView class_name() const override { return "VideoPaintable"sv; }

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;

private:
    VideoPaintable(Layout::VideoBox const&);
};

}
