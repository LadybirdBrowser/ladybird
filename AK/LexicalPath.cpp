/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Max Wipfli <max.wipfli@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

namespace AK {

char s_single_dot = '.';

LexicalPath::LexicalPath(ByteString path)
    : m_string(canonicalized_path(move(path)))
{
    if (m_string.is_empty()) {
        m_string = ".";
        m_dirname = m_string;
        m_basename = {};
        m_title = {};
        m_extension = {};
        m_parts.clear();
        return;
    }

    m_parts = m_string.split_view('/');

    auto last_slash_index = m_string.view().find_last('/');
    if (!last_slash_index.has_value()) {
        // The path contains a single part and is not absolute. m_dirname = "."sv
        m_dirname = { &s_single_dot, 1 };
    } else if (*last_slash_index == 0) {
        // The path contains a single part and is absolute. m_dirname = "/"sv
        m_dirname = m_string.substring_view(0, 1);
    } else {
        m_dirname = m_string.substring_view(0, *last_slash_index);
    }

    if (m_string == "/")
        m_basename = m_string;
    else {
        VERIFY(m_parts.size() > 0);
        m_basename = m_parts.last();
    }

    auto last_dot_index = m_basename.find_last('.');
    // NOTE: if the dot index is 0, this means we have ".foo", it's not an extension, as the title would then be "".
    if (last_dot_index.has_value() && *last_dot_index != 0) {
        m_title = m_basename.substring_view(0, *last_dot_index);
        m_extension = m_basename.substring_view(*last_dot_index + 1);
    } else {
        m_title = m_basename;
        m_extension = {};
    }
}

bool LexicalPath::is_absolute_path(StringView path)
{
    return path.starts_with('/');
}

bool LexicalPath::is_root() const
{
    return m_string == "/";
}

Vector<ByteString> LexicalPath::parts() const
{
    Vector<ByteString> vector;
    vector.ensure_capacity(m_parts.size());
    for (auto& part : m_parts)
        vector.unchecked_append(part);
    return vector;
}

bool LexicalPath::has_extension(StringView extension) const
{
    return m_string.ends_with(extension, CaseSensitivity::CaseInsensitive);
}

bool LexicalPath::is_child_of(LexicalPath const& possible_parent) const
{
    // Any relative path is a child of an absolute path.
    if (!this->is_absolute() && possible_parent.is_absolute())
        return true;
    // An absolute path can't meaningfully be a child of a relative path.
    if (this->is_absolute() && !possible_parent.is_absolute())
        return false;

    // Two relative paths and two absolute paths can be meaningfully compared.
    if (possible_parent.parts_view().size() > this->parts_view().size())
        return false;
    auto common_parts_with_parent = this->parts_view().span().trim(possible_parent.parts_view().size());
    return common_parts_with_parent == possible_parent.parts_view().span();
}

ByteString LexicalPath::canonicalized_path(ByteString path)
{
    // NOTE: We never allow an empty m_string, if it's empty, we just set it to '.'.
    if (path.is_empty())
        return ".";

    // NOTE: If there are no dots, no '//' and the path doesn't end with a slash, it is already canonical.
    if (!path.contains("."sv) && !path.contains("//"sv) && !path.ends_with('/'))
        return path;

    auto is_absolute = path[0] == '/';
    auto parts = path.split_view('/');
    size_t approximate_canonical_length = 0;
    Vector<ByteString> canonical_parts;

    for (auto& part : parts) {
        if (part == ".")
            continue;
        if (part == "..") {
            if (canonical_parts.is_empty()) {
                if (is_absolute) {
                    // At the root, .. does nothing.
                    continue;
                }
            } else {
                if (canonical_parts.last() != "..") {
                    // A .. and a previous non-.. part cancel each other.
                    canonical_parts.take_last();
                    continue;
                }
            }
        }
        approximate_canonical_length += part.length() + 1;
        canonical_parts.append(part);
    }

    if (canonical_parts.is_empty() && !is_absolute)
        canonical_parts.append(".");

    StringBuilder builder(approximate_canonical_length);
    if (is_absolute)
        builder.append('/');
    builder.join('/', canonical_parts);
    return builder.to_byte_string();
}

ByteString LexicalPath::absolute_path(ByteString dir_path, ByteString target)
{
    if (LexicalPath(target).is_absolute()) {
        return LexicalPath::canonicalized_path(target);
    }
    return LexicalPath::canonicalized_path(join(dir_path, target).string());
}

Optional<ByteString> LexicalPath::relative_path(StringView a_path, StringView a_prefix)
{
    if (!a_path.starts_with('/') || !a_prefix.starts_with('/'))
        return {};

    if (a_path == a_prefix)
        return ".";

    // NOTE: Strip optional trailing slashes, except if the full path is only "/".
    auto path = canonicalized_path(a_path);
    auto prefix = canonicalized_path(a_prefix);

    if (path == prefix)
        return ".";

    // NOTE: Handle this special case first.
    if (prefix == "/"sv)
        return path.substring_view(1);

    // NOTE: This means the path is a direct child of the prefix.
    if (path.starts_with(prefix) && path[prefix.length()] == '/') {
        return path.substring_view(prefix.length() + 1);
    }

    auto path_parts = path.split_view('/');
    auto prefix_parts = prefix.split_view('/');
    size_t index_of_first_part_that_differs = 0;
    for (; index_of_first_part_that_differs < path_parts.size() && index_of_first_part_that_differs < prefix_parts.size(); index_of_first_part_that_differs++) {
        if (path_parts[index_of_first_part_that_differs] != prefix_parts[index_of_first_part_that_differs])
            break;
    }

    StringBuilder builder;
    for (size_t part_index = index_of_first_part_that_differs; part_index < prefix_parts.size(); part_index++) {
        builder.append("../"sv);
    }
    for (size_t part_index = index_of_first_part_that_differs; part_index < path_parts.size(); part_index++) {
        builder.append(path_parts[part_index]);
        if (part_index != path_parts.size() - 1) // We don't need a slash after the file name or the name of the last directory
            builder.append('/');
    }

    return builder.to_byte_string();
}

LexicalPath LexicalPath::append(StringView value) const
{
    return LexicalPath::join(m_string, value);
}

LexicalPath LexicalPath::prepend(StringView value) const
{
    return LexicalPath::join(value, m_string);
}

LexicalPath LexicalPath::parent() const
{
    return append(".."sv);
}

}
