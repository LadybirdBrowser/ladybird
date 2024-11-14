/*
 * Copyright (c) 2024, Mohamed amine Bounya <mobounya@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Fetch/Infrastructure/FetchController.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#concept-fetch-record
class FetchRecord : public JS::Cell {
    GC_CELL(FetchRecord, JS::Cell);
    GC_DECLARE_ALLOCATOR(FetchRecord);

public:
    [[nodiscard]] static GC::Ref<FetchRecord> create(JS::VM&, GC::Ref<Infrastructure::Request>);
    [[nodiscard]] static GC::Ref<FetchRecord> create(JS::VM&, GC::Ref<Infrastructure::Request>, GC::Ptr<FetchController>);

    [[nodiscard]] GC::Ref<Infrastructure::Request> request() const { return m_request; }
    void set_request(GC::Ref<Infrastructure::Request> request) { m_request = request; }

    [[nodiscard]] GC::Ptr<FetchController> fetch_controller() const { return m_fetch_controller; }
    void set_fetch_controller(GC::Ptr<FetchController> fetch_controller) { m_fetch_controller = fetch_controller; }

private:
    explicit FetchRecord(GC::Ref<Infrastructure::Request>);
    FetchRecord(GC::Ref<Infrastructure::Request>, GC::Ptr<FetchController>);

    virtual void visit_edges(Visitor&) override;

    // https://fetch.spec.whatwg.org/#concept-request
    // A fetch record has an associated request (a request)
    GC::Ref<Infrastructure::Request> m_request;

    // https://fetch.spec.whatwg.org/#fetch-controller
    // A fetch record has an associated controller (a fetch controller or null)
    GC::Ptr<FetchController> m_fetch_controller { nullptr };
};

}
