/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Element.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Parser/HTMLToken.h>

namespace Web::HTML {

class ListOfActiveFormattingElements {
public:
    ListOfActiveFormattingElements() = default;
    ~ListOfActiveFormattingElements();

    struct Entry {
        bool is_marker() const { return !element; }

        GC::Ptr<DOM::Element> element;
        AK::OwnPtr<HTMLToken> token;
    };

    bool is_empty() const { return m_entries.is_empty(); }
    bool contains(DOM::Element const&) const;

    void add(DOM::Element& element, HTMLToken& token);
    void add_marker();
    void insert_at(size_t index, DOM::Element& element, HTMLToken& token);

    void replace(DOM::Element& to_remove, DOM::Element& to_add, HTMLToken& token);

    void remove(DOM::Element&);

    Vector<Entry> const& entries() const { return m_entries; }
    Vector<Entry>& entries() { return m_entries; }

    DOM::Element* last_element_with_tag_name_before_marker(FlyString const& tag_name);

    void clear_up_to_the_last_marker();

    Optional<size_t> find_index(DOM::Element const&) const;

    void visit_edges(JS::Cell::Visitor&);

    static AK::OwnPtr<HTMLToken> create_own_token(HTMLToken& token);

private:
    Vector<Entry> m_entries;
    void ensure_noahs_ark_clause(DOM::Element& element);
};

}
