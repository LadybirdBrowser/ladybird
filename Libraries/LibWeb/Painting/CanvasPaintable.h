/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/Box.h>
#include <LibWeb/Painting/Paintable.h>

namespace Web::Painting {

class CanvasPaintable final : public Paintable {
public:
    static NonnullRefPtr<CanvasPaintable> create(Layout::Box const&);
    virtual StringView class_name() const override { return "CanvasPaintable"sv; }

private:
    CanvasPaintable(Layout::Box const&);
};

}
