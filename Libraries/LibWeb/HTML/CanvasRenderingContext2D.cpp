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

GC::Ref<CanvasRenderingContext2D> CanvasRenderingContext2D::create(HTMLCanvasElement& element, CanvasRenderingContext2DSettings const& context_attributes)
{
    return static_cast<CanvasRenderingContext2D&>(Canvas2DContextBase::create(element, element, false, context_attributes));
}

}
