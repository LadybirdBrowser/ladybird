/*
 * Copyright (c) 2023, Preston Taylor <PrestonLeeTaylor@proton.me>
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/DOM/DocumentLoadEventDelayer.h>
#include <LibWeb/Forward.h>

namespace Web::DOM {

class StyleElementBase {
public:
    virtual ~StyleElementBase() = default;

    enum class UpdateSource : u8 {
        Dynamic,
        ParserPop,
    };
    void update_a_style_block(UpdateSource = UpdateSource::Dynamic);
    void update_a_style_block_for_dynamic_change();
    void associated_style_sheet_media_attribute_changed();
    void set_parser_document(Badge<HTML::HTMLParser>, GC::Ref<Document>);
    void did_pop_off_parser_stack_of_open_elements();
    void style_element_attribute_changed(Utf16FlyString const&, Optional<Utf16String> const& value);
    void style_element_moved();

    CSS::StyleSheetState* sheet();
    CSS::StyleSheetState const* sheet() const;
    CSS::CSSStyleSheet* cssom_sheet() const;

    bool disabled();
    void set_disabled(bool disabled);

    enum class AnyFailed : u8 {
        No,
        Yes,
    };
    void finished_loading_critical_subresources(AnyFailed);

    void visit_style_element_edges(JS::Cell::Visitor&);

    void retarget_style_load_event_delayer(Document& new_document)
    {
        if (m_document_load_event_delayer.has_value())
            m_document_load_event_delayer.emplace(new_document);
    }

    virtual Element& as_element() = 0;
    virtual Element const& as_element() const = 0;

protected:
    bool style_element_contributes_a_script_blocking_style_sheet() const;

private:
    void evaluate_associated_style_sheet_media_queries();
    void remove_from_script_blocking_style_sheet_set_if_needed();
    void clear_associated_css_style_sheet_parser_blocking_state();

    GC::Weak<Document> m_parser_document;

    // https://www.w3.org/TR/cssom/#associated-css-style-sheet
    RefPtr<CSS::StyleSheetState> m_associated_css_style_sheet;

    CSS::StyleScope* m_style_sheet_scope { nullptr };

    Optional<DocumentLoadEventDelayer> m_document_load_event_delayer;

    u64 m_style_sheet_update_generation { 0 };

    bool m_associated_css_style_sheet_was_created_by_parser : 1 { false };
    bool m_associated_css_style_sheet_was_enabled_when_created_by_parser : 1 { false };
    bool m_associated_css_style_sheet_media_matches_environment : 1 { false };
    bool m_associated_css_style_sheet_load_is_pending_for_script_blocking : 1 { false };
    bool m_associated_css_style_sheet_is_blocking_scripts : 1 { false };
    bool m_is_on_parser_stack_of_open_elements : 1 { false };
};

}
