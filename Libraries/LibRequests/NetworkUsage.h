/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibIPC/Forward.h>

namespace Requests {

struct NetworkUsage {
    i32 process_id { 0 };
    u64 page_id { 0 };
    u64 download_bytes { 0 };
    u64 upload_bytes { 0 };
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Requests::NetworkUsage const&);
template<>
ErrorOr<Requests::NetworkUsage> decode(Decoder&);

}
