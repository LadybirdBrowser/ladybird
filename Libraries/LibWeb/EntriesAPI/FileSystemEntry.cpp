/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/EntriesAPI/FileSystemEntry.h>
#include <LibWeb/HTML/Window.h>

namespace Web::EntriesAPI {

GC_DEFINE_ALLOCATOR(FileSystemEntry);

GC::Ref<FileSystemEntry> FileSystemEntry::create(EntryType entry_type, ByteString name)
{
    return GC::Heap::the().allocate<FileSystemEntry>(entry_type, move(name));
}

FileSystemEntry::FileSystemEntry(EntryType entry_type, ByteString name)
    : Bindings::Wrappable()
    , m_entry_type(entry_type)
    , m_name(move(name))
{
}

// https://wicg.github.io/entries-api/#dom-filesystementry-isfile
bool FileSystemEntry::is_file() const
{
    // The isFile getter steps are to return true if this is a file entry and false otherwise.
    return m_entry_type == EntryType::File;
}

// https://wicg.github.io/entries-api/#dom-filesystementry-isdirectory
bool FileSystemEntry::is_directory() const
{
    // The isDirectory getter steps are to return true if this is a directory entry and false otherwise.
    return m_entry_type == EntryType::Directory;
}

// https://wicg.github.io/entries-api/#dom-filesystementry-name
ByteString FileSystemEntry::name() const
{
    // The name getter steps are to return this's name.
    return m_name;
}

}
