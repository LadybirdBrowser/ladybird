/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWebCommon/PixelUnits.h>

namespace Web::CSS {

NonnullRefPtr<StyleValue const> unscaled_by_zoom(PropertyID, NonnullRefPtr<StyleValue const>, double zoom);
double unscaled(double, double effective_zoom);
CSSPixels unscaled(CSSPixels, double effective_zoom);
double effective_zoom(DOM::Element const&);
double effective_zoom(Layout::NodeWithStyle const&);

}
