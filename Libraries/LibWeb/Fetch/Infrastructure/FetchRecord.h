/*
 * Copyright (c) 2024, Mohamed amine Bounya <mobounya@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/FetchController.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#concept-fetch-record
class WEB_API FetchRecord final : public JS::Cell {
    GC_CELL(FetchRecord, JS::Cell);
    GC_DECLARE_ALLOCATOR(FetchRecord);

public:
    [[nodiscard]] static GC::Ref<FetchRecord> create(NonnullRefPtr<Infrastructure::Request>, GC::Ptr<FetchController>);

    [[nodiscard]] Infrastructure::Request& request() const { return *m_request; }

    [[nodiscard]] GC::Ptr<FetchController> fetch_controller() const { return m_fetch_controller; }
    void set_fetch_controller(GC::Ptr<FetchController> fetch_controller) { m_fetch_controller = fetch_controller; }

private:
    FetchRecord(NonnullRefPtr<Infrastructure::Request>, GC::Ptr<FetchController>);

    virtual void visit_edges(Visitor&) override;
    virtual void finalize() override;

    // https://fetch.spec.whatwg.org/#concept-request
    // A fetch record has an associated request (a request)
    NonnullRefPtr<Infrastructure::Request> m_request;

    // https://fetch.spec.whatwg.org/#fetch-controller
    // A fetch record has an associated controller (a fetch controller or null)
    GC::Ptr<FetchController> m_fetch_controller { nullptr };

    IntrusiveListNode<FetchRecord> m_list_node;

public:
    using List = IntrusiveList<&FetchRecord::m_list_node>;
};

}
