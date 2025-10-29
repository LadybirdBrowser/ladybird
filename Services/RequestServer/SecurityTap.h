/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <LibCore/Socket.h>

namespace RequestServer {

class SecurityTap {
public:
    static ErrorOr<NonnullOwnPtr<SecurityTap>> create();
    ~SecurityTap() = default;

    struct DownloadMetadata {
        ByteString url;
        ByteString filename;
        ByteString mime_type;
        ByteString sha256;
        size_t size_bytes { 0 };
    };

    struct ScanResult {
        bool is_threat { false };
        Optional<ByteString> alert_json;
    };

    // Main inspection method - sends download to Sentinel for YARA scanning
    ErrorOr<ScanResult> inspect_download(
        DownloadMetadata const& metadata,
        ReadonlyBytes content
    );

    // Compute SHA256 hash of content
    static ErrorOr<ByteString> compute_sha256(ReadonlyBytes data);

private:
    SecurityTap(NonnullOwnPtr<Core::LocalSocket> socket);

    ErrorOr<ByteString> send_scan_request(
        DownloadMetadata const& metadata,
        ReadonlyBytes content
    );

    NonnullOwnPtr<Core::LocalSocket> m_sentinel_socket;
};

}
