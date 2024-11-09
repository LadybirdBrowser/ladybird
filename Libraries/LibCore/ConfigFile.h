/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, networkException <networkexception@serenityos.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Forward.h>
#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibCore/File.h>

namespace Core {

class ConfigFile : public RefCounted<ConfigFile> {
public:
    enum class AllowWriting {
        Yes,
        No,
    };

    static ErrorOr<NonnullRefPtr<ConfigFile>> open_for_lib(ByteString const& lib_name, AllowWriting = AllowWriting::No);
    static ErrorOr<NonnullRefPtr<ConfigFile>> open_for_app(ByteString const& app_name, AllowWriting = AllowWriting::No);
    static ErrorOr<NonnullRefPtr<ConfigFile>> open_for_system(ByteString const& app_name, AllowWriting = AllowWriting::No);
    static ErrorOr<NonnullRefPtr<ConfigFile>> open(ByteString const& filename, AllowWriting = AllowWriting::No);
    static ErrorOr<NonnullRefPtr<ConfigFile>> open(ByteString const& filename, int fd);
    static ErrorOr<NonnullRefPtr<ConfigFile>> open(ByteString const& filename, NonnullOwnPtr<Core::File>);
    ~ConfigFile();

    bool has_group(ByteString const&) const;
    bool has_key(ByteString const& group, ByteString const& key) const;

    Vector<ByteString> groups() const;
    Vector<ByteString> keys(ByteString const& group) const;

    size_t num_groups() const { return m_groups.size(); }

    ByteString read_entry(ByteString const& group, ByteString const& key, ByteString const& default_value = {}) const
    {
        return read_entry_optional(group, key).value_or(default_value);
    }
    Optional<ByteString> read_entry_optional(ByteString const& group, ByteString const& key) const;
    bool read_bool_entry(ByteString const& group, ByteString const& key, bool default_value = false) const;

    template<Integral T = int>
    T read_num_entry(ByteString const& group, ByteString const& key, T default_value = 0) const
    {
        if (!has_key(group, key))
            return default_value;

        return read_entry(group, key, "").to_number<T>().value_or(default_value);
    }

    void write_entry(ByteString const& group, ByteString const& key, ByteString const& value);
    void write_bool_entry(ByteString const& group, ByteString const& key, bool value);

    template<Integral T = int>
    void write_num_entry(ByteString const& group, ByteString const& key, T value)
    {
        write_entry(group, key, ByteString::number(value));
    }

    void dump() const;

    bool is_dirty() const { return m_dirty; }

    ErrorOr<void> sync();

    void add_group(ByteString const& group);
    void remove_group(ByteString const& group);
    void remove_entry(ByteString const& group, ByteString const& key);

    ByteString const& filename() const { return m_filename; }

private:
    ConfigFile(ByteString const& filename, OwnPtr<InputBufferedFile> open_file);

    ErrorOr<void> reparse();

    ByteString m_filename;
    OwnPtr<InputBufferedFile> m_file;
    HashMap<ByteString, HashMap<ByteString, ByteString>> m_groups;
    bool m_dirty { false };
};

}
