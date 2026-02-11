/*
 * Copyright (c) 2022-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/PixelUnits.h>

namespace Web::DOM {

class ViewportClient {
public:
    virtual ~ViewportClient() = default;
    virtual void did_set_viewport_rect(CSSPixelRect const&) = 0;
};

}
