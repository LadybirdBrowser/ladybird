/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class CheckBoxPaintable final : public PaintableBox {
public:
    static NonnullRefPtr<CheckBoxPaintable> create(Layout::CheckBox const&);
    virtual StringView class_name() const override { return "CheckBoxPaintable"sv; }

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;

private:
    CheckBoxPaintable(Layout::CheckBox const&);
};

}
