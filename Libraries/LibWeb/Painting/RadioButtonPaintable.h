/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class RadioButtonPaintable final : public PaintableBox {
public:
    static NonnullRefPtr<RadioButtonPaintable> create(Layout::RadioButton const&);
    virtual StringView class_name() const override { return "RadioButtonPaintable"sv; }

    virtual void paint(DisplayListRecordingContext&, PaintPhase) const override;

private:
    RadioButtonPaintable(Layout::RadioButton const&);
};

}
