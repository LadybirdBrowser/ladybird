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
    JS_CELL(FetchRecord, JS::Cell);
    JS_DECLARE_ALLOCATOR(FetchRecord);

public:
    [[nodiscard]] static JS::NonnullGCPtr<FetchRecord> create(JS::VM&, JS::NonnullGCPtr<Infrastructure::Request>);
    [[nodiscard]] static JS::NonnullGCPtr<FetchRecord> create(JS::VM&, JS::NonnullGCPtr<Infrastructure::Request>, JS::GCPtr<FetchController>);

    [[nodiscard]] JS::NonnullGCPtr<Infrastructure::Request> request() const { return m_request; }
    void set_request(JS::NonnullGCPtr<Infrastructure::Request> request) { m_request = request; }

    [[nodiscard]] JS::GCPtr<FetchController> fetch_controller() const { return m_fetch_controller; }
    void set_fetch_controller(JS::GCPtr<FetchController> fetch_controller) { m_fetch_controller = fetch_controller; }

private:
    explicit FetchRecord(JS::NonnullGCPtr<Infrastructure::Request>);
    FetchRecord(JS::NonnullGCPtr<Infrastructure::Request>, JS::GCPtr<FetchController>);

    virtual void visit_edges(Visitor&) override;

    // https://fetch.spec.whatwg.org/#concept-request
    // A fetch record has an associated request (a request)
    JS::NonnullGCPtr<Infrastructure::Request> m_request;

    // https://fetch.spec.whatwg.org/#fetch-controller
    // A fetch record has an associated controller (a fetch controller or null)
    JS::GCPtr<FetchController> m_fetch_controller { nullptr };
};

}
