/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Lucien Fiorini <lucienfiorini@gmail.com>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/CanvasRenderingContext2D.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(CanvasRenderingContext2D);

GC::Ref<CanvasRenderingContext2D> CanvasRenderingContext2D::create(HTMLCanvasElement& element, HTML::CanvasRenderingContext2DSettings context_attributes)
{
    auto& realm = HTML::relevant_realm(element);
    auto context = realm.create<CanvasRenderingContext2D>(realm, element, context_attributes);
    return context;
}

CanvasRenderingContext2D::CanvasRenderingContext2D(JS::Realm& realm, HTMLCanvasElement& element, HTML::CanvasRenderingContext2DSettings context_attributes)
    : Canvas2DContextBase(realm, element.bitmap_size_for_canvas(), move(context_attributes))
    , m_element(element)
{
}

CanvasRenderingContext2D::~CanvasRenderingContext2D() = default;

void CanvasRenderingContext2D::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_element);
}

GC::Ref<HTMLCanvasElement> CanvasRenderingContext2D::canvas_for_binding() const
{
    return *m_element;
}

}
