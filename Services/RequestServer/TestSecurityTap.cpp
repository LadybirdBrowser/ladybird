/*
 * Copyright (c) 2025, RBSmith4
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibCore/File.h>
#include <RequestServer/SecurityTap.h>
#include <stdio.h>

using namespace RequestServer;

int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <file-to-scan>\n", argv[0]);
        return 1;
    }

    // Initialize SecurityTap
    auto security_tap_result = SecurityTap::create();
    if (security_tap_result.is_error()) {
        fprintf(stderr, "Failed to initialize SecurityTap: %s\n",
                security_tap_result.error().string_literal().characters_without_null_termination());
        fprintf(stderr, "Make sure Sentinel is running!\n");
        return 1;
    }

    auto security_tap = security_tap_result.release_value();
    printf("SecurityTap initialized successfully\n");

    // Read test file
    StringView filename { argv[1], strlen(argv[1]) };
    auto file_result = Core::File::open(filename, Core::File::OpenMode::Read);
    if (file_result.is_error()) {
        fprintf(stderr, "Failed to open file: %s\n", argv[1]);
        return 1;
    }

    auto file = file_result.release_value();
    auto content_result = file->read_until_eof();
    if (content_result.is_error()) {
        fprintf(stderr, "Failed to read file\n");
        return 1;
    }

    auto content = content_result.release_value();
    printf("Read %zu bytes from file\n", content.size());

    // Compute SHA256
    auto sha256_result = SecurityTap::compute_sha256(content.bytes());
    if (sha256_result.is_error()) {
        fprintf(stderr, "Failed to compute SHA256\n");
        return 1;
    }

    auto sha256 = sha256_result.release_value();
    printf("SHA256: %s\n", sha256.characters());

    // Create metadata
    SecurityTap::DownloadMetadata metadata {
        .url = ByteString("http://localhost:8000/test"),
        .filename = ByteString(argv[1]),
        .mime_type = ByteString("application/octet-stream"),
        .sha256 = sha256,
        .size_bytes = content.size()
    };

    // Scan the file
    printf("\nScanning file with Sentinel...\n");
    auto scan_result = security_tap->inspect_download(metadata, content.bytes());

    if (scan_result.is_error()) {
        fprintf(stderr, "Scan failed: %s\n",
                scan_result.error().string_literal().characters_without_null_termination());
        return 1;
    }

    auto result = scan_result.release_value();

    if (result.is_threat) {
        printf("\n🚨 THREAT DETECTED!\n");
        printf("Alert JSON: %s\n", result.alert_json.value().characters());
        return 2; // Exit code 2 = threat detected
    } else {
        printf("\n✅ No threats detected - file is clean\n");
        return 0;
    }
}
