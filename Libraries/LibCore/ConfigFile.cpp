/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, networkException <networkexception@serenityos.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibCore/ConfigFile.h>

namespace Core {

ErrorOr<NonnullRefPtr<ConfigFile>> ConfigFile::open(ByteString const& filename)
{
    if (auto result = File::open(filename, File::OpenMode::Read); result.is_error()) {
        // If we attempted to open a file that does not exist, we ignore the error, making it appear the same as if we
        // had opened an empty file. This behavior is a little weird, but is required by user code, which does not check
        // the config file exists before opening.
        if (result.error().code() != ENOENT)
            return result.release_error();
    } else {
        return open(filename, result.release_value());
    }

    return adopt_nonnull_ref_or_enomem(new (nothrow) ConfigFile(filename, nullptr));
}

ErrorOr<NonnullRefPtr<ConfigFile>> ConfigFile::open(ByteString const& filename, NonnullOwnPtr<Core::File> file)
{
    auto buffered_file = TRY(InputBufferedFile::create(move(file)));

    auto config_file = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) ConfigFile(filename, move(buffered_file))));
    TRY(config_file->parse());
    return config_file;
}

ConfigFile::ConfigFile(ByteString const& filename, OwnPtr<InputBufferedFile> open_file)
    : m_filename(filename)
    , m_file(move(open_file))
{
}

ConfigFile::~ConfigFile() = default;

ErrorOr<void> ConfigFile::parse()
{
    VERIFY(m_file);
    m_groups.clear();

    HashMap<ByteString, ByteString>* current_group = nullptr;

    auto buffer = TRY(ByteBuffer::create_uninitialized(4096));
    while (TRY(m_file->can_read_line())) {
        auto line = TRY(m_file->read_line(buffer));
        size_t i = 0;

        while (i < line.length() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\n'))
            ++i;

        if (i >= line.length())
            continue;

        switch (line[i]) {
        case '#': // Comment, skip entire line.
        case ';': // -||-
            continue;
        case '[': { // Start of new group.
            StringBuilder builder;
            ++i; // Skip the '['
            while (i < line.length() && (line[i] != ']')) {
                builder.append(line[i]);
                ++i;
            }
            current_group = &m_groups.ensure(builder.to_byte_string());
            break;
        }
        default: { // Start of key
            StringBuilder key_builder;
            StringBuilder value_builder;
            while (i < line.length() && (line[i] != '=')) {
                key_builder.append(line[i]);
                ++i;
            }
            ++i; // Skip the '='
            while (i < line.length() && (line[i] != '\n')) {
                value_builder.append(line[i]);
                ++i;
            }
            if (!current_group) {
                // We're not in a group yet, create one with the name ""...
                current_group = &m_groups.ensure("");
            }
            auto value_string = value_builder.to_byte_string();
            current_group->set(key_builder.to_byte_string(), value_string.trim_whitespace(TrimMode::Right));
        }
        }
    }
    return {};
}

Optional<ByteString> ConfigFile::read_entry_optional(AK::ByteString const& group, AK::ByteString const& key) const
{
    if (!has_key(group, key))
        return {};
    auto it = m_groups.find(group);
    auto jt = it->value.find(key);
    return jt->value;
}

bool ConfigFile::read_bool_entry(ByteString const& group, ByteString const& key, bool default_value) const
{
    auto value = read_entry(group, key, default_value ? "true" : "false");
    return value == "1" || value.equals_ignoring_ascii_case("true"sv);
}

Vector<ByteString> ConfigFile::groups() const
{
    return m_groups.keys();
}

Vector<ByteString> ConfigFile::keys(ByteString const& group) const
{
    auto it = m_groups.find(group);
    if (it == m_groups.end())
        return {};
    return it->value.keys();
}

bool ConfigFile::has_key(ByteString const& group, ByteString const& key) const
{
    auto it = m_groups.find(group);
    if (it == m_groups.end())
        return {};
    return it->value.contains(key);
}

bool ConfigFile::has_group(ByteString const& group) const
{
    return m_groups.contains(group);
}

}
