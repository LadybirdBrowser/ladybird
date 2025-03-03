/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/ContentSecurityPolicy/Policy.h>

namespace Web::ContentSecurityPolicy {

class PolicyList final : public JS::Cell {
    GC_CELL(PolicyList, JS::Cell);
    GC_DECLARE_ALLOCATOR(PolicyList);

public:
    [[nodiscard]] static GC::Ref<PolicyList> create(JS::Realm&, GC::RootVector<GC::Ref<Policy>> const&);
    [[nodiscard]] static GC::Ref<PolicyList> create(JS::Realm&, Vector<SerializedPolicy> const&);
    [[nodiscard]] static GC::Ptr<PolicyList> from_object(JS::Object&);

    virtual ~PolicyList() = default;

    [[nodiscard]] Vector<GC::Ref<Policy>> const& policies() const { return m_policies; }

    [[nodiscard]] bool contains_header_delivered_policy() const;

    [[nodiscard]] HTML::SandboxingFlagSet csp_derived_sandboxing_flags() const;

    [[nodiscard]] GC::Ref<PolicyList> clone(JS::Realm&) const;
    [[nodiscard]] Vector<SerializedPolicy> serialize() const;

protected:
    virtual void visit_edges(Cell::Visitor&) override;

private:
    PolicyList() = default;

    Vector<GC::Ref<Policy>> m_policies;
};

}
