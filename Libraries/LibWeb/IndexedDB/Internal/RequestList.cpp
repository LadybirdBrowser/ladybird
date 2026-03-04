/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/IndexedDB/Internal/Algorithms.h>
#include <LibWeb/IndexedDB/Internal/RequestList.h>

namespace Web::IndexedDB {

void RequestList::enqueue(GC::Ref<IDBRequest> request, GC::Ref<GC::Function<void()>> steps)
{
    m_entries.append({ request, steps });
    maybe_process_next_request();
}

void RequestList::maybe_process_next_request()
{
    if (m_blocked)
        return;

    while (!m_entries.is_empty() && !m_entries.first().request)
        m_entries.remove(0);

    for (auto& [request, steps] : m_entries) {
        if (!request)
            continue;
        if (request->processed())
            continue;
        if (request->aborted())
            continue;

        // If the steps are null here, the request is still executing, so we should wait until it is finished.
        if (!steps)
            return;

        queue_a_database_task(*steps);
        steps = nullptr;
        return;
    }
}

void RequestList::on_request_processed()
{
    maybe_process_next_request();
}

void RequestList::remove(GC::Ref<IDBRequest> request)
{
    m_entries.remove_first_matching([&request](auto const& entry) {
        return entry.request.ptr() == request.ptr();
    });
}

bool RequestList::is_empty() const
{
    return m_entries.is_empty();
}

void RequestList::set_on_all_processed(GC::Ref<GC::Function<void()>> callback)
{
    m_on_all_processed = callback;
    check_all_processed();
}

void RequestList::clear_on_all_processed()
{
    m_on_all_processed = nullptr;
}

void RequestList::check_all_processed()
{
    if (!m_on_all_processed)
        return;

    m_entries.remove_all_matching([](auto const& entry) { return !entry.request; });

    for (auto const& entry : m_entries) {
        if (!entry.request->processed())
            return;
    }

    auto callback = move(m_on_all_processed);
    callback->function()();
}

void RequestList::unblock_execution()
{
    m_blocked = false;
    maybe_process_next_request();
}

GC::Ref<IDBRequest> RequestList::RequestIterator::operator*() const
{
    return *m_entries[m_index].request;
}

RequestList::RequestIterator& RequestList::RequestIterator::operator++()
{
    ++m_index;
    skip_null();
    return *this;
}

void RequestList::RequestIterator::skip_null()
{
    while (m_index < m_entries.size() && !m_entries[m_index].request)
        ++m_index;
}

}
