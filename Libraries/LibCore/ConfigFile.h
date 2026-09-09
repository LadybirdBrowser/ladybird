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
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/Vector.h>
#include <LibCore/Export.h>
#include <LibCore/File.h>

namespace Core {

class CORE_API ConfigFile : public RefCounted<ConfigFile> {
public:
    static ErrorOr<NonnullRefPtr<ConfigFile>> open(ByteString const& filename);
    static ErrorOr<NonnullRefPtr<ConfigFile>> open(ByteString const& filename, NonnullOwnPtr<Core::File>);
    ~ConfigFile();

    bool has_group(ByteString const&) const;
    bool has_key(ByteString const& group, ByteString const& key) const;

    Vector<ByteString> groups() const;
    Vector<ByteString> keys(ByteString const& group) const;

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

private:
    ConfigFile(ByteString const& filename, OwnPtr<InputBufferedFile> open_file);

    ErrorOr<void> parse();

    ByteString m_filename;
    OwnPtr<InputBufferedFile> m_file;
    HashMap<ByteString, HashMap<ByteString, ByteString>> m_groups;
};

}
