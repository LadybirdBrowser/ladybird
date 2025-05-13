/*
 * Copyright (c) 2025, Bohdan Sverdlov <freezar92@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/AnonymousImageBox.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Painting {

class AnonymousImagePaintable final
    : public PaintableBox
    , public DOM::Document::ViewportClient {
    GC_CELL(AnonymousImagePaintable, PaintableBox);
    GC_DECLARE_ALLOCATOR(AnonymousImagePaintable);

public:
    static GC::Ref<AnonymousImagePaintable> create(Layout::AnonymousImageBox const& layout_box);

    virtual void paint(PaintContext&, PaintPhase) const override;

private:
    // ^JS::Cell
    virtual void visit_edges(Visitor&) override;
    virtual void finalize() override;

    // ^Document::ViewportClient
    virtual void did_set_viewport_rect(CSSPixelRect const&) final;

    AnonymousImagePaintable(Layout::Box const& layout_box);
};

}
