/*
 * Copyright (c) 2022-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Painting/Paintable.h>

namespace Web::Painting {

class PaintableWithLines : public Paintable {
public:
    static NonnullRefPtr<PaintableWithLines> create(Layout::BlockContainer const&);
    virtual ~PaintableWithLines() override;

protected:
    PaintableWithLines(Layout::BlockContainer const&);
};

}
