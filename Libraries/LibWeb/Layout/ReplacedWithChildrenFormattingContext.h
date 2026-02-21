/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/FormattingContext.h>

namespace Web::Layout {

class ReplacedWithChildrenFormattingContext final : public FormattingContext {
public:
    explicit ReplacedWithChildrenFormattingContext(LayoutState&, LayoutMode, Box const&, FormattingContext* parent);

    virtual void run(AvailableSpace const&) override;
    virtual CSSPixels automatic_content_width() const override;
    virtual CSSPixels automatic_content_height() const override;
    virtual void parent_context_did_dimension_child_root_box() override;

private:
    CSSPixels m_automatic_content_width { 0 };
    CSSPixels m_automatic_content_height { 0 };
};

}
