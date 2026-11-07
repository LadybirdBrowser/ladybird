/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibIPC/TransportHandle.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// Launches ImageDecoders as processes first ask for them, and ends each once the last process it serves is gone.
class WEBVIEW_API ImageDecoderManager {
public:
    AK_ALLOC_WITH_KMALLOC;

    static ImageDecoderManager& the();

    ImageDecoderManager();
    ~ImageDecoderManager();

    // Connects a renderer or worker to its ImageDecoder, launching one if need be. A process that lost its decoder asks
    // here again, and gets a new one.
    ErrorOr<IPC::TransportHandle> connect_new_client(u64 client_child_id, IsPrivate);

    // Answers a renderer or worker that asked for a decoder, which may have exited by the time its request is handled.
    Optional<IPC::TransportHandle> connect_requesting_process(u64 client_child_id, IsPrivate);

    void process_did_exit(u64 child_id);

private:
    struct Instance {
        NonnullRefPtr<ImageDecoderControlClient> control_client;
        HashTable<u64> client_child_ids;
    };

    Vector<Instance> m_instances;
};

}
