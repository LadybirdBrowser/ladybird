/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Time.h>
#include <AK/Vector.h>

namespace RequestServer {

struct QuarantineMetadata {
    ByteString original_url;
    ByteString filename;
    ByteString detection_time;  // ISO 8601 format
    Vector<ByteString> rule_names;
    ByteString sha256;
    size_t file_size { 0 };
    ByteString quarantine_id;
};

class Quarantine {
public:
    // Initialize quarantine directory structure
    static ErrorOr<void> initialize();

    // Quarantine a file with metadata tracking
    // Returns the quarantine ID on success
    static ErrorOr<String> quarantine_file(
        String const& source_path,
        QuarantineMetadata const& metadata
    );

    // Retrieve metadata for a quarantined file
    static ErrorOr<QuarantineMetadata> get_metadata(String const& quarantine_id);

    // Get quarantine directory path
    static ErrorOr<String> get_quarantine_directory();

private:
    // Generate unique quarantine ID (timestamp + random suffix)
    static ErrorOr<String> generate_quarantine_id();

    // Write metadata JSON file
    static ErrorOr<void> write_metadata(
        String const& quarantine_id,
        QuarantineMetadata const& metadata
    );

    // Read metadata JSON file
    static ErrorOr<QuarantineMetadata> read_metadata(String const& quarantine_id);
};

}
