/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, networkException <networkexception@serenityos.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/Directory.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>

namespace Core {

ErrorOr<NonnullRefPtr<ConfigFile>> ConfigFile::open_for_lib(ByteString const& lib_name, AllowWriting allow_altering)
{
    ByteString directory_name = ByteString::formatted("{}/lib", StandardPaths::config_directory());
    auto directory = TRY(Directory::create(directory_name, Directory::CreateDirectories::Yes));
    auto path = ByteString::formatted("{}/{}.ini", directory, lib_name);
    return ConfigFile::open(path, allow_altering);
}

ErrorOr<NonnullRefPtr<ConfigFile>> ConfigFile::open_for_app(ByteString const& app_name, AllowWriting allow_altering)
{
    auto directory = TRY(Directory::create(StandardPaths::config_directory(), Directory::CreateDirectories::Yes));
    auto path = ByteString::formatted("{}/{}.ini", directory, app_name);
    return ConfigFile::open(path, allow_altering);
}

ErrorOr<NonnullRefPtr<ConfigFile>> ConfigFile::open_for_system(ByteString const& app_name, AllowWriting allow_altering)
{
    auto path = ByteString::formatted("/etc/{}.ini", app_name);
    return ConfigFile::open(path, allow_altering);
}

ErrorOr<NonnullRefPtr<ConfigFile>> ConfigFile::open(ByteString const& filename, AllowWriting allow_altering)
{
    auto maybe_file = File::open(filename, allow_altering == AllowWriting::Yes ? File::OpenMode::ReadWrite : File::OpenMode::Read);
    OwnPtr<InputBufferedFile> buffered_file;
    if (maybe_file.is_error()) {
        // If we attempted to open a read-only file that does not exist, we ignore the error, making it appear
        // the same as if we had opened an empty file. This behavior is a little weird, but is required by
        // user code, which does not check the config file exists before opening.
        if (!(allow_altering == AllowWriting::No && maybe_file.error().code() == ENOENT))
            return maybe_file.release_error();
    } else {
        buffered_file = TRY(InputBufferedFile::create(maybe_file.release_value()));
    }

    auto config_file = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) ConfigFile(filename, move(buffered_file))));
    TRY(config_file->reparse());
    return config_file;
}

ErrorOr<NonnullRefPtr<ConfigFile>> ConfigFile::open(ByteString const& filename, int fd)
{
    auto file = TRY(File::adopt_fd(fd, File::OpenMode::ReadWrite));
    return open(filename, move(file));
}

ErrorOr<NonnullRefPtr<ConfigFile>> ConfigFile::open(ByteString const& filename, NonnullOwnPtr<Core::File> file)
{
    auto buffered_file = TRY(InputBufferedFile::create(move(file)));

    auto config_file = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) ConfigFile(filename, move(buffered_file))));
    TRY(config_file->reparse());
    return config_file;
}

ConfigFile::ConfigFile(ByteString const& filename, OwnPtr<InputBufferedFile> open_file)
    : m_filename(filename)
    , m_file(move(open_file))
{
}

ConfigFile::~ConfigFile()
{
    MUST(sync());
}

ErrorOr<void> ConfigFile::reparse()
{
    m_groups.clear();
    if (!m_file)
        return {};

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

Optional<ByteString> ConfigFile::read_entry_optional(const AK::ByteString& group, const AK::ByteString& key) const
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

void ConfigFile::write_entry(ByteString const& group, ByteString const& key, ByteString const& value)
{
    m_groups.ensure(group).ensure(key) = value;
    m_dirty = true;
}

void ConfigFile::write_bool_entry(ByteString const& group, ByteString const& key, bool value)
{
    write_entry(group, key, value ? "true" : "false");
}

ErrorOr<void> ConfigFile::sync()
{
    if (!m_dirty)
        return {};

    if (!m_file)
        return Error::from_errno(ENOENT);

    TRY(m_file->truncate(0));
    TRY(m_file->seek(0, SeekMode::SetPosition));

    for (auto& it : m_groups) {
        TRY(m_file->write_until_depleted(ByteString::formatted("[{}]\n", it.key)));
        for (auto& jt : it.value)
            TRY(m_file->write_until_depleted(ByteString::formatted("{}={}\n", jt.key, jt.value)));
        TRY(m_file->write_until_depleted("\n"sv));
    }

    m_dirty = false;
    return {};
}

void ConfigFile::dump() const
{
    for (auto& it : m_groups) {
        outln("[{}]", it.key);
        for (auto& jt : it.value)
            outln("{}={}", jt.key, jt.value);
        outln();
    }
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

void ConfigFile::add_group(ByteString const& group)
{
    m_groups.ensure(group);
    m_dirty = true;
}

void ConfigFile::remove_group(ByteString const& group)
{
    m_groups.remove(group);
    m_dirty = true;
}

void ConfigFile::remove_entry(ByteString const& group, ByteString const& key)
{
    auto it = m_groups.find(group);
    if (it == m_groups.end())
        return;
    it->value.remove(key);
    m_dirty = true;
}

}
