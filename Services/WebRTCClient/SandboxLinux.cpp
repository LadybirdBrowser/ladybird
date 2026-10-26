/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibSandbox/Sandbox.h>
#include <LibSandbox/Seccomp.h>
#include <WebRTCClient/Sandbox.h>

namespace WebRTCClient {

ErrorOr<void> apply_sandbox(StringView, NetworkAccess network_access)
{
    TRY(Sandbox::install_no_new_privileges());
    TRY(Sandbox::configure_runtime());

    // Nothing on the file system is ours to read. Even host names are looked up by the renderer, through RequestServer.
    TRY(Sandbox::restrict_filesystem_with_landlock({}));

    Sandbox::SeccompPolicy policy;
    policy.allow_readonly_file_opens();
    policy.allow_filesystem_metadata_queries();
    policy.allow_file_descriptor_operations();
    policy.allow_ipc();
    policy.allow_epoll();
    // tokio's signal driver wakes its runtime through a socket pair, which the turn crate turns on for us.
    policy.allow_socket_pairs();
    if (network_access == NetworkAccess::Allowed)
        policy.allow_datagram_network();
    policy.allow_common_runtime();
    TRY(policy.install());

    return {};
}

}
