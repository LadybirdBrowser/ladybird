/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLFontElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLFontElement, HTMLElement);
    friend struct DOM::RustElementObjectHooks;

public:
    virtual bool is_presentational_hint(Utf16FlyString const&) const override;
    virtual void apply_presentational_hints(Vector<CSS::StyleProperty>&) const override;

    static Optional<CSS::Keyword> parse_legacy_font_size(Utf16View);

private:
    explicit HTMLFontElement(DOM::VtableProbe);
    virtual ~HTMLFontElement() override = default;
};

}
