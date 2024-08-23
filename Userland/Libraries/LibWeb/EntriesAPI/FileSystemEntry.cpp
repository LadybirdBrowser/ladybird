/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/FileSystemEntryPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/EntriesAPI/FileSystemEntry.h>
#include <LibWeb/HTML/Window.h>

namespace Web::EntriesAPI {

JS_DEFINE_ALLOCATOR(FileSystemEntry);

JS::NonnullGCPtr<FileSystemEntry> FileSystemEntry::create(JS::Realm& realm, EntryType entry_type, ByteString name)
{
    return realm.heap().allocate<FileSystemEntry>(realm, realm, entry_type, name);
}

FileSystemEntry::FileSystemEntry(JS::Realm& realm, EntryType entry_type, ByteString name)
    : PlatformObject(realm)
    , m_entry_type(entry_type)
    , m_name(name)
{
}

void FileSystemEntry::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(FileSystemEntry);
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
