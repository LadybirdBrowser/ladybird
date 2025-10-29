/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibCore/LocalServer.h>
#include <LibCore/Socket.h>

namespace Sentinel {

class SentinelServer {
public:
    static ErrorOr<NonnullOwnPtr<SentinelServer>> create();
    ~SentinelServer() = default;

private:
    SentinelServer(NonnullRefPtr<Core::LocalServer>);

    void handle_client(NonnullOwnPtr<Core::LocalSocket>);
    ErrorOr<void> process_message(Core::LocalSocket&, String const& message);

    ErrorOr<ByteString> scan_file(ByteString const& file_path);
    ErrorOr<ByteString> scan_content(ReadonlyBytes content);

    NonnullRefPtr<Core::LocalServer> m_server;
    Vector<NonnullOwnPtr<Core::LocalSocket>> m_clients;
};

}
