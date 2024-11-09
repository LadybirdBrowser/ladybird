/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Element.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

class ListOfActiveFormattingElements {
public:
    ListOfActiveFormattingElements() = default;
    ~ListOfActiveFormattingElements();

    struct Entry {
        bool is_marker() const { return !element; }

        JS::GCPtr<DOM::Element> element;
    };

    bool is_empty() const { return m_entries.is_empty(); }
    bool contains(const DOM::Element&) const;

    void add(DOM::Element& element);
    void add_marker();
    void insert_at(size_t index, DOM::Element& element);

    void replace(DOM::Element& to_remove, DOM::Element& to_add);

    void remove(DOM::Element&);

    Vector<Entry> const& entries() const { return m_entries; }
    Vector<Entry>& entries() { return m_entries; }

    DOM::Element* last_element_with_tag_name_before_marker(FlyString const& tag_name);

    void clear_up_to_the_last_marker();

    Optional<size_t> find_index(DOM::Element const&) const;

    void visit_edges(JS::Cell::Visitor&);

private:
    Vector<Entry> m_entries;
};

}
