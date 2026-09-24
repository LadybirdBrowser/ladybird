/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <LibMediaClient/Forward.h>

namespace MediaClient {

// The renderer's end of a media stream that the media server demuxes: bytes go to the server as they arrive, and the
// server asks for the offset it needs next.
class RemoteMediaStream : public RefCounted<RemoteMediaStream> {
public:
    static NonnullRefPtr<RemoteMediaStream> create();
    ~RemoteMediaStream();

    u64 id() const { return m_id; }
    bool is_connected() const { return m_client != nullptr; }

    using DataRequestCallback = Function<void(Optional<u64> offset)>;
    void set_data_request_callback(DataRequestCallback);
    void set_may_idle(bool);

    void add_chunk_at(u64 offset, ReadonlyBytes);
    void set_expected_size(u64);
    void close();

    void data_requested(Badge<Client>, Optional<u64> offset);

private:
    RemoteMediaStream(RefPtr<Client>, u64 id);

    RefPtr<Client> m_client;
    u64 m_id { 0 };
    DataRequestCallback m_data_request_callback;
};

}
