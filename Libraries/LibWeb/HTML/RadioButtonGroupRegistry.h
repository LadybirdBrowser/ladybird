/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Utf16FlyString.h>
#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

class RadioButtonGroupRegistry final : public GC::Cell {
    GC_CELL(RadioButtonGroupRegistry, GC::Cell);
    GC_DECLARE_ALLOCATOR(RadioButtonGroupRegistry);

public:
    void add_button(Utf16FlyString const& group_name, HTMLInputElement&);
    void remove_button(Utf16FlyString const& group_name, HTMLInputElement&);
    void checked_state_changed(Utf16FlyString const& group_name, HTMLInputElement&);
    void required_state_changed(Utf16FlyString const& group_name, HTMLInputElement&);

    GC::Ptr<HTMLInputElement> checked_button(Utf16FlyString const& group_name) const;
    bool group_is_suffering_from_being_missing(Utf16FlyString const& group_name) const;

private:
    struct RadioButtonGroup {
        HashTable<GC::Ref<HTMLInputElement>> members;
        GC::Ptr<HTMLInputElement> checked_button;
        size_t required_count { 0 };
        bool suffering_from_being_missing { false };
    };

    virtual void visit_edges(Cell::Visitor&) override;

    void set_checked_button(RadioButtonGroup&, HTMLInputElement&);
    void update_group_suffering_from_being_missing(RadioButtonGroup&);

    HashMap<Utf16FlyString, RadioButtonGroup> m_groups;
};

}
