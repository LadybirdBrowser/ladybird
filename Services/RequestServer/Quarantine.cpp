/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Quarantine.h"
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonParser.h>
#include <AK/JsonValue.h>
#include <AK/Random.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <time.h>

namespace RequestServer {

ErrorOr<void> Quarantine::initialize()
{
    // Get quarantine directory path
    auto quarantine_dir = TRY(get_quarantine_directory());
    auto quarantine_dir_byte_string = quarantine_dir.to_byte_string();

    // Create directory if it doesn't exist
    TRY(Core::Directory::create(quarantine_dir_byte_string, Core::Directory::CreateDirectories::Yes));

    // Set restrictive permissions on directory (owner only: rwx------)
    TRY(Core::System::chmod(quarantine_dir, 0700));

    dbgln("Quarantine: Initialized directory at {}", quarantine_dir);
    return {};
}

ErrorOr<String> Quarantine::get_quarantine_directory()
{
    // Use ~/.local/share/Ladybird/Quarantine/
    auto user_data_dir = Core::StandardPaths::user_data_directory();
    StringBuilder path_builder;
    path_builder.append(user_data_dir);
    path_builder.append("/Ladybird/Quarantine"sv);
    return path_builder.to_string();
}

ErrorOr<String> Quarantine::generate_quarantine_id()
{
    // Generate ID: YYYYMMDD_HHMMSS_<6_random_hex_chars>
    auto now = UnixDateTime::now();
    time_t timestamp = now.seconds_since_epoch();
    struct tm tm_buf;
    struct tm* tm_info = gmtime_r(&timestamp, &tm_buf);

    if (!tm_info) {
        return Error::from_string_literal("Failed to convert timestamp");
    }

    // Format timestamp
    StringBuilder id_builder;
    id_builder.appendff("{:04}{:02}{:02}_{:02}{:02}{:02}_",
        tm_info->tm_year + 1900,
        tm_info->tm_mon + 1,
        tm_info->tm_mday,
        tm_info->tm_hour,
        tm_info->tm_min,
        tm_info->tm_sec);

    // Add random suffix (6 hex characters)
    u32 random_value = get_random<u32>();
    id_builder.appendff("{:06x}", random_value & 0xFFFFFF);

    return id_builder.to_string();
}

ErrorOr<String> Quarantine::quarantine_file(
    String const& source_path,
    QuarantineMetadata const& metadata)
{
    // Ensure quarantine directory exists
    TRY(initialize());

    // Generate unique quarantine ID
    auto quarantine_id = TRY(generate_quarantine_id());
    auto quarantine_dir = TRY(get_quarantine_directory());

    // Build destination path for quarantined file
    StringBuilder dest_path_builder;
    dest_path_builder.append(quarantine_dir);
    dest_path_builder.append('/');
    dest_path_builder.append(quarantine_id);
    dest_path_builder.append(".bin"sv);
    auto dest_path = TRY(dest_path_builder.to_string());

    // Move file to quarantine directory (atomic operation)
    dbgln("Quarantine: Moving {} to {}", source_path, dest_path);
    TRY(FileSystem::move_file(dest_path, source_path, FileSystem::PreserveMode::Nothing));

    // Set restrictive permissions on quarantined file (owner read-only: r--------)
    TRY(Core::System::chmod(dest_path, 0400));

    // Update metadata with quarantine ID
    QuarantineMetadata updated_metadata = metadata;
    updated_metadata.quarantine_id = quarantine_id.to_byte_string();

    // Write metadata JSON file
    TRY(write_metadata(quarantine_id, updated_metadata));

    dbgln("Quarantine: Successfully quarantined file with ID: {}", quarantine_id);
    return quarantine_id;
}

ErrorOr<void> Quarantine::write_metadata(
    String const& quarantine_id,
    QuarantineMetadata const& metadata)
{
    auto quarantine_dir = TRY(get_quarantine_directory());

    // Build metadata file path
    StringBuilder metadata_path_builder;
    metadata_path_builder.append(quarantine_dir);
    metadata_path_builder.append('/');
    metadata_path_builder.append(quarantine_id);
    metadata_path_builder.append(".json"sv);
    auto metadata_path = TRY(metadata_path_builder.to_string());

    // Build JSON object
    JsonObject json;
    json.set("original_url"sv, JsonValue(metadata.original_url));
    json.set("filename"sv, JsonValue(metadata.filename));
    json.set("detection_time"sv, JsonValue(metadata.detection_time));
    json.set("sha256"sv, JsonValue(metadata.sha256));
    json.set("file_size"sv, JsonValue(static_cast<u64>(metadata.file_size)));
    json.set("quarantine_id"sv, JsonValue(metadata.quarantine_id));

    // Add rule names as JSON array
    JsonArray rules_array;
    for (auto const& rule : metadata.rule_names) {
        TRY(rules_array.append(JsonValue(rule)));
    }
    json.set("rule_names"sv, move(rules_array));

    // Serialize to JSON string
    auto json_string = json.serialized();

    // Write to file
    auto file = TRY(Core::File::open(metadata_path, Core::File::OpenMode::Write));
    TRY(file->write_until_depleted(json_string.bytes()));

    // Set restrictive permissions on metadata file (owner read-only: r--------)
    TRY(Core::System::chmod(metadata_path, 0400));

    dbgln("Quarantine: Wrote metadata to {}", metadata_path);
    return {};
}

ErrorOr<QuarantineMetadata> Quarantine::get_metadata(String const& quarantine_id)
{
    return read_metadata(quarantine_id);
}

ErrorOr<QuarantineMetadata> Quarantine::read_metadata(String const& quarantine_id)
{
    auto quarantine_dir = TRY(get_quarantine_directory());

    // Build metadata file path
    StringBuilder metadata_path_builder;
    metadata_path_builder.append(quarantine_dir);
    metadata_path_builder.append('/');
    metadata_path_builder.append(quarantine_id);
    metadata_path_builder.append(".json"sv);
    auto metadata_path = TRY(metadata_path_builder.to_string());

    // Read file contents
    auto file = TRY(Core::File::open(metadata_path, Core::File::OpenMode::Read));
    auto contents = TRY(file->read_until_eof());
    auto json_string = ByteString(contents.bytes());

    // Parse JSON
    auto json_result = JsonValue::from_string(json_string);
    if (json_result.is_error()) {
        return Error::from_string_literal("Failed to parse quarantine metadata JSON");
    }

    auto json = json_result.value();
    if (!json.is_object()) {
        return Error::from_string_literal("Expected JSON object in metadata file");
    }

    auto obj = json.as_object();

    // Extract fields
    QuarantineMetadata metadata;

    auto original_url = obj.get_string("original_url"sv);
    if (!original_url.has_value())
        return Error::from_string_literal("Missing 'original_url' in metadata");
    metadata.original_url = original_url.value().to_byte_string();

    auto filename = obj.get_string("filename"sv);
    if (!filename.has_value())
        return Error::from_string_literal("Missing 'filename' in metadata");
    metadata.filename = filename.value().to_byte_string();

    auto detection_time = obj.get_string("detection_time"sv);
    if (!detection_time.has_value())
        return Error::from_string_literal("Missing 'detection_time' in metadata");
    metadata.detection_time = detection_time.value().to_byte_string();

    auto sha256 = obj.get_string("sha256"sv);
    if (!sha256.has_value())
        return Error::from_string_literal("Missing 'sha256' in metadata");
    metadata.sha256 = sha256.value().to_byte_string();

    auto file_size = obj.get_u64("file_size"sv);
    if (!file_size.has_value())
        return Error::from_string_literal("Missing 'file_size' in metadata");
    metadata.file_size = static_cast<size_t>(file_size.value());

    auto quarantine_id_str = obj.get_string("quarantine_id"sv);
    if (!quarantine_id_str.has_value())
        return Error::from_string_literal("Missing 'quarantine_id' in metadata");
    metadata.quarantine_id = quarantine_id_str.value().to_byte_string();

    // Extract rule names array
    auto rules_array_opt = obj.get_array("rule_names"sv);
    if (!rules_array_opt.has_value())
        return Error::from_string_literal("Missing 'rule_names' in metadata");

    auto const& rules_array = rules_array_opt.value();
    for (size_t i = 0; i < rules_array.size(); i++) {
        auto rule = rules_array.at(i);
        if (rule.is_string()) {
            metadata.rule_names.append(rule.as_string().to_byte_string());
        }
    }

    return metadata;
}

ErrorOr<Vector<QuarantineMetadata>> Quarantine::list_all_entries()
{
    Vector<QuarantineMetadata> entries;

    auto quarantine_dir = TRY(get_quarantine_directory());
    auto quarantine_dir_byte_string = quarantine_dir.to_byte_string();

    // Check if directory exists
    if (!FileSystem::exists(quarantine_dir_byte_string)) {
        dbgln("Quarantine: Directory does not exist: {}", quarantine_dir);
        return entries; // Return empty vector
    }

    // Iterate through directory looking for .json metadata files
    TRY(Core::Directory::for_each_entry(quarantine_dir_byte_string, Core::DirIterator::SkipParentAndBaseDir, [&](auto const& entry, auto const&) -> ErrorOr<IterationDecision> {
        // Only process .json files
        if (!entry.name.ends_with(".json"sv))
            return IterationDecision::Continue;

        // Extract quarantine ID (remove .json extension)
        auto quarantine_id_byte = entry.name.substring(0, entry.name.length() - 5);
        auto quarantine_id_result = String::from_byte_string(quarantine_id_byte);

        if (quarantine_id_result.is_error()) {
            dbgln("Quarantine: Failed to convert quarantine ID: {}", entry.name);
            return IterationDecision::Continue;
        }

        auto quarantine_id = quarantine_id_result.release_value();

        // Read metadata
        auto metadata_result = read_metadata(quarantine_id);
        if (metadata_result.is_error()) {
            dbgln("Quarantine: Failed to read metadata for {}: {}", quarantine_id, metadata_result.error());
            return IterationDecision::Continue;
        }

        entries.append(metadata_result.release_value());
        return IterationDecision::Continue;
    }));

    dbgln("Quarantine: Found {} quarantined files", entries.size());
    return entries;
}

ErrorOr<void> Quarantine::restore_file(String const& quarantine_id, String const& destination_dir)
{
    auto quarantine_dir = TRY(get_quarantine_directory());

    // Build source paths
    StringBuilder source_file_builder;
    source_file_builder.append(quarantine_dir);
    source_file_builder.append('/');
    source_file_builder.append(quarantine_id);
    source_file_builder.append(".bin"sv);
    auto source_file = TRY(source_file_builder.to_string());

    // Check if quarantined file exists
    if (!FileSystem::exists(source_file)) {
        return Error::from_string_literal("Quarantined file does not exist");
    }

    // Read metadata to get original filename
    auto metadata = TRY(read_metadata(quarantine_id));

    // Build destination path
    StringBuilder dest_path_builder;
    dest_path_builder.append(destination_dir);
    dest_path_builder.append('/');
    dest_path_builder.append(metadata.filename);
    auto dest_path = TRY(dest_path_builder.to_string());

    // Check if destination already exists
    if (FileSystem::exists(dest_path)) {
        // Append number to make unique
        for (int i = 1; i < 1000; i++) {
            StringBuilder unique_dest_builder;
            unique_dest_builder.append(destination_dir);
            unique_dest_builder.append('/');
            unique_dest_builder.append(metadata.filename);
            unique_dest_builder.appendff("_({})", i);
            auto unique_dest = TRY(unique_dest_builder.to_string());

            if (!FileSystem::exists(unique_dest)) {
                dest_path = unique_dest;
                break;
            }
        }
    }

    // Move file from quarantine to destination
    dbgln("Quarantine: Restoring {} to {}", source_file, dest_path);
    TRY(FileSystem::move_file(dest_path, source_file, FileSystem::PreserveMode::Nothing));

    // Restore normal permissions (owner read/write: rw-------)
    TRY(Core::System::chmod(dest_path, 0600));

    // Delete metadata file
    StringBuilder metadata_path_builder;
    metadata_path_builder.append(quarantine_dir);
    metadata_path_builder.append('/');
    metadata_path_builder.append(quarantine_id);
    metadata_path_builder.append(".json"sv);
    auto metadata_path = TRY(metadata_path_builder.to_string());

    TRY(FileSystem::remove(metadata_path, FileSystem::RecursionMode::Disallowed));

    dbgln("Quarantine: Successfully restored file to {}", dest_path);
    return {};
}

ErrorOr<void> Quarantine::delete_file(String const& quarantine_id)
{
    auto quarantine_dir = TRY(get_quarantine_directory());

    // Build file paths
    StringBuilder file_path_builder;
    file_path_builder.append(quarantine_dir);
    file_path_builder.append('/');
    file_path_builder.append(quarantine_id);
    file_path_builder.append(".bin"sv);
    auto file_path = TRY(file_path_builder.to_string());

    StringBuilder metadata_path_builder;
    metadata_path_builder.append(quarantine_dir);
    metadata_path_builder.append('/');
    metadata_path_builder.append(quarantine_id);
    metadata_path_builder.append(".json"sv);
    auto metadata_path = TRY(metadata_path_builder.to_string());

    // Delete quarantined file (if it exists)
    if (FileSystem::exists(file_path)) {
        TRY(FileSystem::remove(file_path, FileSystem::RecursionMode::Disallowed));
        dbgln("Quarantine: Deleted file {}", file_path);
    }

    // Delete metadata file (if it exists)
    if (FileSystem::exists(metadata_path)) {
        TRY(FileSystem::remove(metadata_path, FileSystem::RecursionMode::Disallowed));
        dbgln("Quarantine: Deleted metadata {}", metadata_path);
    }

    dbgln("Quarantine: Successfully deleted quarantine entry {}", quarantine_id);
    return {};
}

}
