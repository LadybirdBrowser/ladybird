/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Max Wipfli <max.wipfli@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Vector.h>

// On Linux distros that use mlibc `basename` is defined as a macro that expands to `__mlibc_gnu_basename` or `__mlibc_gnu_basename_c`, so we undefine it.
#if defined(AK_OS_LINUX) && defined(basename)
#    undef basename
#endif

namespace AK {

class LexicalPath {
public:
    enum StripExtension {
        No,
        Yes
    };

    explicit LexicalPath(ByteString);

    static bool is_absolute_path(StringView path);
    bool is_absolute() const { return is_absolute_path(m_string); }
    bool is_root() const;

    ByteString const& string() const { return m_string; }

    StringView dirname() const { return m_dirname; }
    StringView basename(StripExtension s = StripExtension::No) const { return s == StripExtension::No ? m_basename : m_title; }
    StringView title() const { return m_title; }
    StringView extension() const { return m_extension; }

    Vector<StringView> const& parts_view() const { return m_parts; }
    [[nodiscard]] Vector<ByteString> parts() const;

    bool has_extension(StringView) const;
    bool is_child_of(LexicalPath const& possible_parent) const;

    [[nodiscard]] LexicalPath append(StringView) const;
    [[nodiscard]] LexicalPath prepend(StringView) const;
    [[nodiscard]] LexicalPath parent() const;

    [[nodiscard]] static ByteString canonicalized_path(ByteString);
    [[nodiscard]] static ByteString absolute_path(ByteString dir_path, ByteString target);
    [[nodiscard]] static Optional<ByteString> relative_path(StringView absolute_path, StringView absolute_prefix);

    template<typename... S>
    [[nodiscard]] static LexicalPath join(StringView first, S&&... rest)
    {
        StringBuilder builder;
        builder.append(first);
        // NOTE: On Windows slashes will be converted to backslashes in LexicalPath constructor
        ((builder.append('/'), builder.append(forward<S>(rest))), ...);

        return LexicalPath { builder.to_byte_string() };
    }

    [[nodiscard]] static ByteString dirname(ByteString path)
    {
        auto lexical_path = LexicalPath(move(path));
        return lexical_path.dirname();
    }

    [[nodiscard]] static ByteString basename(ByteString path, StripExtension s = StripExtension::No)
    {
        auto lexical_path = LexicalPath(move(path));
        return lexical_path.basename(s);
    }

    [[nodiscard]] static ByteString title(ByteString path)
    {
        auto lexical_path = LexicalPath(move(path));
        return lexical_path.title();
    }

    [[nodiscard]] static ByteString extension(ByteString path)
    {
        auto lexical_path = LexicalPath(move(path));
        return lexical_path.extension();
    }

private:
    Vector<StringView> m_parts;
    ByteString m_string;
    StringView m_dirname;
    StringView m_basename;
    StringView m_title;
    StringView m_extension; // doesn't include the dot
};

template<>
struct Formatter<LexicalPath> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, LexicalPath const& value)
    {
        return Formatter<StringView>::format(builder, value.string());
    }
};

};

#if USING_AK_GLOBALLY
using AK::LexicalPath;
#endif
