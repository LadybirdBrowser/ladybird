/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/DOM/Document.h>

namespace Web::CSS::Parser {

class ParsingContext {
public:
    enum class Mode {
        Normal,
        SVGPresentationAttribute, // See https://svgwg.org/svg2-draft/types.html#presentation-attribute-css-value
    };

    explicit ParsingContext(Mode = Mode::Normal);
    explicit ParsingContext(JS::Realm&, Mode = Mode::Normal);
    explicit ParsingContext(JS::Realm&, URL::URL, Mode = Mode::Normal);
    explicit ParsingContext(DOM::Document const&, Mode = Mode::Normal);
    explicit ParsingContext(DOM::Document const&, URL::URL, Mode = Mode::Normal);

    Mode mode() const { return m_mode; }
    bool is_parsing_svg_presentation_attribute() const { return m_mode == Mode::SVGPresentationAttribute; }

    bool in_quirks_mode() const;
    DOM::Document const* document() const { return m_document; }
    HTML::Window const* window() const;
    URL::URL complete_url(StringView) const;

    JS::Realm& realm() const
    {
        VERIFY(m_realm);
        return *m_realm;
    }

private:
    GC::Ptr<JS::Realm> m_realm;
    GC::Ptr<DOM::Document const> m_document;
    URL::URL m_url;
    Mode m_mode { Mode::Normal };
};

}
