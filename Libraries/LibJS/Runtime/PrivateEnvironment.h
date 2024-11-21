/*
 * Copyright (c) 2021, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>

namespace JS {

struct PrivateName {
    PrivateName() = default;
    PrivateName(u64 unique_id, FlyString description)
        : unique_id(unique_id)
        , description(move(description))
    {
    }

    u64 unique_id { 0 };
    FlyString description;

    bool operator==(PrivateName const& rhs) const;
};

class PrivateEnvironment : public Cell {
    GC_CELL(PrivateEnvironment, Cell);
    GC_DECLARE_ALLOCATOR(PrivateEnvironment);

public:
    PrivateName resolve_private_identifier(FlyString const& identifier) const;

    void add_private_name(FlyString description);

    PrivateEnvironment* outer_environment() { return m_outer_environment; }
    PrivateEnvironment const* outer_environment() const { return m_outer_environment; }

private:
    explicit PrivateEnvironment(PrivateEnvironment* parent);

    virtual void visit_edges(Visitor&) override;

    auto find_private_name(FlyString const& description) const
    {
        return m_private_names.find_if([&](PrivateName const& private_name) {
            return private_name.description == description;
        });
    }

    static u64 s_next_id;

    GC::Ptr<PrivateEnvironment> m_outer_environment; // [[OuterEnv]]
    Vector<PrivateName> m_private_names;             // [[Names]]
    u64 m_unique_id;
};

}
