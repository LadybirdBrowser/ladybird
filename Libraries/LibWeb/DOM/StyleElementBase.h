/*
 * Copyright (c) 2023, Preston Taylor <PrestonLeeTaylor@proton.me>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleSheet.h>
#include <LibWeb/DOM/DocumentLoadEventDelayer.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

class StyleElementBase {
public:
    virtual ~StyleElementBase() = default;

    void update_a_style_block();
    void update_a_style_block_for_dynamic_change();
    void style_element_attribute_changed(FlyString const&, Optional<String> const& value);

    CSS::CSSStyleSheet* sheet();
    CSS::CSSStyleSheet const* sheet() const;

    [[nodiscard]] GC::Ptr<CSS::StyleSheetList> style_sheet_list() { return m_style_sheet_list; }
    [[nodiscard]] GC::Ptr<CSS::StyleSheetList const> style_sheet_list() const { return m_style_sheet_list; }

    enum class AnyFailed : u8 {
        No,
        Yes,
    };
    void finished_loading_critical_subresources(AnyFailed);

    void visit_style_element_edges(JS::Cell::Visitor&);

    virtual Element& as_element() = 0;
    virtual Element const& as_element() const = 0;

protected:
    bool style_element_contributes_a_script_blocking_style_sheet() const;

private:
    // https://www.w3.org/TR/cssom/#associated-css-style-sheet
    GC::Ptr<CSS::CSSStyleSheet> m_associated_css_style_sheet;

    GC::Ptr<CSS::StyleSheetList> m_style_sheet_list;

    Optional<DocumentLoadEventDelayer> m_document_load_event_delayer;
};

}
