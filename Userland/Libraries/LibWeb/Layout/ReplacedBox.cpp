/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/InlineFormattingContext.h>
#include <LibWeb/Layout/ReplacedBox.h>

namespace Web::Layout {

ReplacedBox::ReplacedBox(DOM::Document& document, DOM::Element& element, CSS::StyleProperties style)
    : Box(document, &element, move(style))
{
}

ReplacedBox::~ReplacedBox() = default;

}
