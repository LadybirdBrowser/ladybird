/*
 * Copyright (c) 2024, Mohamed amine Bounya <mobounya@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Infrastructure/FetchRecord.h>

namespace Web::Fetch::Infrastructure {

JS_DEFINE_ALLOCATOR(FetchRecord);

JS::NonnullGCPtr<FetchRecord> FetchRecord::create(JS::VM& vm, JS::NonnullGCPtr<Infrastructure::Request> request)
{
    return vm.heap().allocate_without_realm<FetchRecord>(request);
}

JS::NonnullGCPtr<FetchRecord> FetchRecord::create(JS::VM& vm, JS::NonnullGCPtr<Infrastructure::Request> request, JS::GCPtr<Fetch::Infrastructure::FetchController> fetch_controller)
{
    return vm.heap().allocate_without_realm<FetchRecord>(request, fetch_controller);
}

FetchRecord::FetchRecord(JS::NonnullGCPtr<Infrastructure::Request> request)
    : m_request(request)
{
}

FetchRecord::FetchRecord(JS::NonnullGCPtr<Infrastructure::Request> request, JS::GCPtr<Fetch::Infrastructure::FetchController> fetch_controller)
    : m_request(request)
    , m_fetch_controller(fetch_controller)
{
}

void FetchRecord::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_request);
    visitor.visit(m_fetch_controller);
}

}
