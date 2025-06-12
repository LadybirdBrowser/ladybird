/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Clip.h"

namespace Web::CSS {

Clip::Clip(Type type, EdgeRect edge_rect)
    : m_type(type)
    , m_edge_rect(edge_rect)
{
}

Clip::Clip(EdgeRect edge_rect)
    : m_type(Type::Rect)
    , m_edge_rect(edge_rect)
{
}

Clip Clip::make_auto()
{
    return Clip(Type::Auto, EdgeRect { Length::make_auto(), Length::make_auto(), Length::make_auto(), Length::make_auto() });
}

}
