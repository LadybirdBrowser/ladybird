/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibRequests/NetworkUsage.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Requests::NetworkUsage const& usage)
{
    TRY(encoder.encode(usage.process_id));
    TRY(encoder.encode(usage.page_id));
    TRY(encoder.encode(usage.download_bytes));
    TRY(encoder.encode(usage.upload_bytes));
    return {};
}
template<>
ErrorOr<Requests::NetworkUsage> decode(Decoder& decoder)
{
    auto process_id = TRY(decoder.decode<i32>());
    auto page_id = TRY(decoder.decode<u64>());
    auto download_bytes = TRY(decoder.decode<u64>());
    auto upload_bytes = TRY(decoder.decode<u64>());
    return Requests::NetworkUsage { process_id, page_id, download_bytes, upload_bytes };
}

}
