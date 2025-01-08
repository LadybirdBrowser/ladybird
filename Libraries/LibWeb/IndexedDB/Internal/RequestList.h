/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Root.h>
#include <LibWeb/IndexedDB/IDBRequest.h>

namespace Web::IndexedDB {

class RequestList : public AK::Vector<GC::Root<IDBRequest>> {
public:
    bool all_requests_processed() const;
    bool all_previous_requests_processed(GC::Ref<IDBRequest> const& request) const;
};

}
