/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/IndexedDB/Internal/RequestList.h>

namespace Web::IndexedDB {

bool RequestList::all_requests_processed() const
{
    for (auto const& entry : *this) {
        if (!entry->processed())
            return false;
    }

    return true;
}

bool RequestList::all_previous_requests_processed(GC::Ref<IDBRequest> const& request) const
{
    for (auto const& entry : *this) {
        if (entry == request)
            return true;
        if (!entry->processed())
            return false;
    }

    return true;
}

}
