/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Max Wipfli <max.wipfli@serenityos.org>
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>

namespace AK {

static bool is_root(auto const& parts)
{
    return parts.size() == 1 && LexicalPath::is_absolute_path(parts[0]);
}

LexicalPath::LexicalPath(ByteString path)
{
    m_string = canonicalized_path(path);
    m_parts = m_string.split_view('\\');

    auto last_slash_index = m_string.find_last('\\');
    if (!last_slash_index.has_value())
        m_dirname = "."sv;
    else
        m_dirname = m_string.substring_view(0, *last_slash_index);

    // NOTE: For "C:\", both m_dirname and m_basename are "C:", which matches the behavior of dirname/basename in Cygwin/MSYS/git (but not MinGW)
    m_basename = m_parts.last();

    auto last_dot_index = m_basename.find_last('.');
    // NOTE: If the last dot index is 0, it's not an extension: ".foo".
    if (last_dot_index.has_value() && *last_dot_index != 0 && m_basename != "..") {
        m_title = m_basename.substring_view(0, *last_dot_index);
        m_extension = m_basename.substring_view(*last_dot_index + 1);
    } else {
        m_title = m_basename;
        m_extension = {};
    }
}

bool LexicalPath::is_absolute_path(StringView path)
{
    return path.length() >= 2 && path[1] == ':';
}

bool LexicalPath::is_root() const
{
    return AK::is_root(m_parts);
}

Vector<ByteString> LexicalPath::parts() const
{
    Vector<ByteString> vector;
    for (auto part : m_parts)
        vector.append(part);
    return vector;
}

bool LexicalPath::has_extension(StringView extension) const
{
    if (extension.starts_with('.'))
        extension = extension.substring_view(1);
    return m_extension.equals_ignoring_ascii_case(extension);
}

bool LexicalPath::is_child_of(LexicalPath const& possible_parent) const
{
    // Any relative path is a child of an absolute path.
    if (!this->is_absolute() && possible_parent.is_absolute())
        return true;

    return m_string.starts_with(possible_parent.string())
        && m_string[possible_parent.string().length()] == '\\';
}

ByteString LexicalPath::canonicalized_path(ByteString path)
{
    path = path.replace("/"sv, "\\"sv);
    auto parts = path.split_view('\\');
    Vector<ByteString> canonical_parts;

    for (auto part : parts) {
        if (part == ".")
            continue;
        if (part == ".." && !canonical_parts.is_empty()) {
            // At the root, .. does nothing.
            if (AK::is_root(canonical_parts))
                continue;
            // A .. and a previous non-.. part cancel each other.
            if (canonical_parts.last() != "..") {
                canonical_parts.take_last();
                continue;
            }
        }
        canonical_parts.append(part);
    }

    StringBuilder builder;
    builder.join('\\', canonical_parts);
    // "X:" -> "X:\"
    if (AK::is_root(canonical_parts))
        builder.append('\\');
    path = builder.to_byte_string();
    return path == "" ? "." : path;
}

ByteString LexicalPath::absolute_path(ByteString dir_path, ByteString target)
{
    if (is_absolute_path(target))
        return canonicalized_path(target);

    return join(dir_path, target).string();
}

// Returns relative version of abs_path (relative to abs_prefix), such that join(abs_prefix, rel_path) == abs_path.
Optional<ByteString> LexicalPath::relative_path(StringView abs_path, StringView abs_prefix)
{
    if (!is_absolute_path(abs_path) || !is_absolute_path(abs_prefix)
        || abs_path[0] != abs_prefix[0]) // different drives
        return {};

    auto path = canonicalized_path(abs_path);
    auto prefix = canonicalized_path(abs_prefix);

    if (path == prefix)
        return ".";

    auto path_parts = path.split_view('\\');
    auto prefix_parts = prefix.split_view('\\');
    size_t first_mismatch = 0;
    for (; first_mismatch < min(path_parts.size(), prefix_parts.size()); first_mismatch++) {
        if (path_parts[first_mismatch] != prefix_parts[first_mismatch])
            break;
    }

    StringBuilder builder;
    builder.append_repeated("..\\"sv, prefix_parts.size() - first_mismatch);
    builder.join('\\', path_parts.span().slice(first_mismatch));
    return builder.to_byte_string();
}

LexicalPath LexicalPath::append(StringView value) const
{
    return join(m_string, value);
}

LexicalPath LexicalPath::prepend(StringView value) const
{
    return join(value, m_string);
}

LexicalPath LexicalPath::parent() const
{
    return append(".."sv);
}

}
