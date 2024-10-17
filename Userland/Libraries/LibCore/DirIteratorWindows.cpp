/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/DirIterator.h>
#include <windows.h>

namespace Core {

namespace {

DirectoryEntry::Type directory_entry_type_from_win32(DWORD file_attributes)
{
    if (file_attributes & FILE_ATTRIBUTE_DIRECTORY)
        return DirectoryEntry::Type::Directory;
    if (file_attributes & FILE_ATTRIBUTE_DEVICE)
        return DirectoryEntry::Type::CharacterDevice;
    if (file_attributes & FILE_ATTRIBUTE_REPARSE_POINT)
        return DirectoryEntry::Type::SymbolicLink;
    return DirectoryEntry::Type::File;
}

DirectoryEntry directory_entry_from_find_data(WIN32_FIND_DATA const& de)
{
    return DirectoryEntry {
        .type = directory_entry_type_from_win32(de.dwFileAttributes),
        .name = de.cFileName,
    };
}

}

struct DirIterator::Impl {
    HANDLE handle { INVALID_HANDLE_VALUE };
    WIN32_FIND_DATA find_data;
    bool initialized { false };
};

DirIterator::DirIterator(ByteString path, Flags flags)
    : m_impl(make<Impl>())
    , m_path(move(path))
    , m_flags(flags)
{
}

DirIterator::~DirIterator()
{
    if (m_impl && m_impl->handle != INVALID_HANDLE_VALUE) {
        FindClose(m_impl->handle);
        m_impl->handle = INVALID_HANDLE_VALUE;
    }
}

DirIterator::DirIterator(DirIterator&& other)
    : m_impl(move(other.m_impl))
    , m_error(move(other.m_error))
    , m_next(move(other.m_next))
    , m_path(move(other.m_path))
    , m_flags(other.m_flags)
{
}

bool DirIterator::advance_next()
{
    if (!m_impl)
        return false;

    while (true) {
        if (!m_impl->initialized) {
            m_impl->initialized = true;
            auto path = ByteString::formatted("{}/*", m_path);
            m_impl->handle = FindFirstFile(path.characters(), &m_impl->find_data);
            if (m_impl->handle == INVALID_HANDLE_VALUE) {
                m_error = Error::from_windows_error(GetLastError());
                return false;
            }
        } else {
            if (!FindNextFile(m_impl->handle, &m_impl->find_data)) {
                m_error = Error::from_windows_error(GetLastError());
                return false;
            }
        }

        m_next = directory_entry_from_find_data(m_impl->find_data);

        if (m_next->name.is_empty())
            return false;

        if (m_flags & Flags::SkipDots && m_next->name.starts_with('.'))
            continue;

        if (m_flags & Flags::SkipParentAndBaseDir && (m_next->name == "." || m_next->name == ".."))
            continue;

        return !m_next->name.is_empty();
    }
}

bool DirIterator::has_next()
{
    if (m_next.has_value())
        return true;

    return advance_next();
}

Optional<DirectoryEntry> DirIterator::next()
{
    if (!m_next.has_value())
        advance_next();

    auto result = m_next;
    m_next.clear();
    return result;
}

ByteString DirIterator::next_path()
{
    auto entry = next();
    if (entry.has_value())
        return entry->name;
    return "";
}

ByteString DirIterator::next_full_path()
{
    StringBuilder builder;
    builder.append(m_path);
    if (!m_path.ends_with('/'))
        builder.append('/');
    builder.append(next_path());
    return builder.to_byte_string();
}

int DirIterator::fd() const
{
    dbgln("DirIterator::fd() not implemented");
    VERIFY_NOT_REACHED();
}

}
