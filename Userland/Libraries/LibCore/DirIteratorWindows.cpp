/*
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibCore/DirIterator.h>
#include <io.h>

namespace Core {

DirIterator::DirIterator(ByteString path, Flags flags)
    : m_path(move(path))
    , m_flags(flags)
{
}

DirIterator::~DirIterator()
{
    if (m_handle != INVALID_HANDLE_VALUE) {
        FindClose(m_handle);
        m_handle = INVALID_HANDLE_VALUE;
    }
}

DirIterator::DirIterator(DirIterator&& other)
    : m_handle(other.m_handle)
    , m_error(move(other.m_error))
    , m_next(move(other.m_next))
    , m_path(move(other.m_path))
    , m_flags(other.m_flags)
{
    other.m_handle = nullptr;
}

bool DirIterator::advance_next()
{

    while (true) {
        if (!m_initialized) {
            m_initialized = true;
            auto path = ByteString::formatted("{}/*", m_path);
            m_handle = FindFirstFile(path.characters(), &m_find_data);
            if (m_handle == INVALID_HANDLE_VALUE) {
                m_error = Error::from_windows_error(GetLastError());
                return false;
            }
        } else {
            if (!FindNextFile(m_handle, &m_find_data)) {
                m_error = Error::from_windows_error(GetLastError());
                return false;
            }
        }

        m_next = DirectoryEntry::from_find_data(m_find_data);

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
    if (!m_handle)
        return -1;
    auto* handle = CreateFile(m_path.characters(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return -1;
    return _open_osfhandle((intptr_t)handle, 0);
}

}
