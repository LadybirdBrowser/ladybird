/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
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

    explicit ParsingContext(JS::Realm&, Mode = Mode::Normal);
    explicit ParsingContext(JS::Realm&, URL::URL, Mode = Mode::Normal);
    explicit ParsingContext(DOM::Document const&, Mode = Mode::Normal);
    explicit ParsingContext(DOM::Document const&, URL::URL, Mode = Mode::Normal);
    explicit ParsingContext(DOM::ParentNode&, Mode = Mode::Normal);

    Mode mode() const { return m_mode; }
    bool is_parsing_svg_presentation_attribute() const { return m_mode == Mode::SVGPresentationAttribute; }

    bool in_quirks_mode() const;
    DOM::Document const* document() const { return m_document; }
    HTML::Window const* window() const;
    URL::URL complete_url(StringView) const;

    PropertyID current_property_id() const { return m_current_property_id; }
    void set_current_property_id(PropertyID property_id) { m_current_property_id = property_id; }

    JS::Realm& realm() const { return m_realm; }

private:
    JS::NonnullGCPtr<JS::Realm> m_realm;
    JS::GCPtr<DOM::Document const> m_document;
    PropertyID m_current_property_id { PropertyID::Invalid };
    URL::URL m_url;
    Mode m_mode { Mode::Normal };
};

}
