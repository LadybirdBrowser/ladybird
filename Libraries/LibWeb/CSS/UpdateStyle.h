/*
 * Copyright (c) 2018-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

class AbstractElement;
class Document;

}

namespace Web::CSS {

enum class StyleUpdateMode : u8 {
    Normal,
    StopAtDisplayNone,
};

WEB_API void update_style(DOM::Document&);
WEB_API void update_style_if_needed_for_element(DOM::Document&, DOM::AbstractElement const&);
WEB_API ComputedProperties const* update_style_for_element(DOM::Document&, DOM::AbstractElement const&, StyleUpdateMode = StyleUpdateMode::Normal);
WEB_API bool element_needs_style_update(DOM::Document const&, DOM::AbstractElement const&);

}
