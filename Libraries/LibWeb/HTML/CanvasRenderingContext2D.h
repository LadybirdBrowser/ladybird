/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/Canvas/Canvas2DContextBase.h>
#include <LibWeb/HTML/Canvas/CanvasTextDrawingStyles.h>

namespace Web::HTML {

class CanvasRenderingContext2D
    : public Canvas2DContextBase
    , public CanvasTextDrawingStyles<HTMLCanvasElement> {

    WEB_WRAPPABLE(CanvasRenderingContext2D, Canvas2DContextBase);
    GC_DECLARE_ALLOCATOR(CanvasRenderingContext2D);

public:
    static GC::Ref<CanvasRenderingContext2D> create(HTMLCanvasElement&, HTML::CanvasRenderingContext2DSettings);

    virtual ~CanvasRenderingContext2D() override;

    GC::Ref<HTMLCanvasElement> canvas_for_binding() const;

protected:
    virtual CanvasHost& canvas_host() const override { return *m_element; }

private:
    CanvasRenderingContext2D(JS::Realm&, HTMLCanvasElement&, HTML::CanvasRenderingContext2DSettings);

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<HTMLCanvasElement> m_element;
};

}
