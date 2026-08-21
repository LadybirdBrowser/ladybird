/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025-2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Painting/PaintableWithLines.h>

namespace Web::Painting {

NonnullRefPtr<PaintableWithLines> PaintableWithLines::create(Layout::BlockContainer const& block_container)
{
    return adopt_ref(*new PaintableWithLines(block_container));
}

PaintableWithLines::PaintableWithLines(Layout::BlockContainer const& layout_box)
    : Paintable(layout_box)
{
}

PaintableWithLines::~PaintableWithLines()
{
}

} // namespace Web::Painting
