/*
 * Copyright (c) 2024, Mohamed amine Bounya <mobounya@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/Fetch/Infrastructure/FetchRecord.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>

namespace Web::Fetch::Infrastructure {

GC_DEFINE_ALLOCATOR(FetchRecord);

GC::Ref<FetchRecord> FetchRecord::create(NonnullRefPtr<Infrastructure::Request> request, GC::Ptr<Fetch::Infrastructure::FetchController> fetch_controller)
{
    return GC::Heap::the().allocate<FetchRecord>(request, fetch_controller);
}

FetchRecord::FetchRecord(NonnullRefPtr<Infrastructure::Request> request, GC::Ptr<Fetch::Infrastructure::FetchController> fetch_controller)
    : m_request(move(request))
    , m_fetch_controller(fetch_controller)
{
}

void FetchRecord::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_fetch_controller);
}

void FetchRecord::finalize()
{
    Base::finalize();
    m_list_node.remove();
}

}
