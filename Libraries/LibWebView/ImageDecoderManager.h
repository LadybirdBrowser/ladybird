/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <LibIPC/TransportHandle.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

// Launches ImageDecoders as processes first ask for them, and ends each once each process it serves is gone or has let go
// of it.
//
// A decoder sees the images of every process it serves, so a malicious image that takes one over gets at those images. The
// processes of a site may share a decoder, because under site isolation they hold nothing of any other site's that they
// could not fetch for themselves. The site is that of the documents a process hosts, not their top-level site: a frame
// embedded in another site never shares a decoder with its embedder, unless it has an opaque origin, which keeps it in the
// process of the document that made it. Each browsing session has decoders of its own.
// Without site isolation a process may host anything, as may a process that has no site or has hosted several, so each of
// those gets a decoder of its own, and a process that comes to host a second site leaves the decoder of the first.
class WEBVIEW_API ImageDecoderManager {
public:
    AK_ALLOC_WITH_KMALLOC;

    static ImageDecoderManager& the();

    ImageDecoderManager();
    ~ImageDecoderManager();

    // Connects a renderer or worker to the ImageDecoder of its site, or to one of its own if it has no site, launching
    // one if need be. A process that lost its decoder asks here again, and gets a new one. A worker can outlive its
    // session, in which case it has none and shares with nobody.
    ErrorOr<IPC::TransportHandle> connect_new_client(u64 client_child_id, RefPtr<BrowsingSession> const&, Optional<Utf16String> site);

    // Answers a renderer or worker that asked for a decoder, which may have exited by the time its request is handled.
    // Each of them has one client at a time, and keeps its generation in `client_generation`; see below.
    Optional<IPC::TransportHandle> connect_requesting_process(u64 client_child_id, RefPtr<BrowsingSession> const&, Optional<Utf16String> site, Optional<u64>& client_generation);

    // Returns whether the process was served by the decoder of a site, and so has to disconnect from it.
    bool remove_client_from_decoder_of_site(u64 client_child_id);

    // Lets go of one client that a process connected to its decoder, which ends once no process has a client left.
    void process_did_release_decoder(u64 client_child_id);

    // A client can be let go of long after its process left the decoder of that client, or lost it. So a renderer or
    // worker notes the generation of its process as it connects, and a release from an older generation is about a
    // decoder the process has left. One that connects again has lost its last client without saying so, which counts
    // as a release.
    u64 client_generation(u64 client_child_id) const { return m_client_generations.get(client_child_id).value_or(0); }
    void note_connected_client(u64 client_child_id, Optional<u64>& client_generation);
    void process_did_release_decoder(u64 client_child_id, u64 client_generation);

    void process_did_exit(u64 child_id);

private:
    struct Instance {
        NonnullRefPtr<ImageDecoderControlClient> control_client;
        WeakPtr<BrowsingSession> session;
        Optional<Utf16String> site;
        // NB: The threads of a process each have a client of their own, and they let go of them one at a time.
        HashMap<u64, size_t> client_counts;
    };

    void did_lose_decoder(Instance const&);

    Vector<Instance> m_instances;
    HashMap<u64, u64> m_client_generations;
};

}
