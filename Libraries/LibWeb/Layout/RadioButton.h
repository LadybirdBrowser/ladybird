/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Layout/ReplacedBox.h>

namespace Web::Layout {

class RadioButton final : public ReplacedBox {
    LAYOUT_NODE(RadioButton, ReplacedBox);

public:
    RadioButton(DOM::Document&, HTML::HTMLInputElement&, CSS::LayoutStyle);
    virtual ~RadioButton() override;
};

}
