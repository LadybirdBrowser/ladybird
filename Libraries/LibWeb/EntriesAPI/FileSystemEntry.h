/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/FileSystemEntry.h>
#include <LibWeb/Bindings/Wrappable.h>

namespace Web::EntriesAPI {

enum class EntryType {
    File,
    Directory,
};

class FileSystemEntry final : public Bindings::Wrappable {
    WEB_WRAPPABLE(FileSystemEntry, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(FileSystemEntry);

public:
    static GC::Ref<FileSystemEntry> create(EntryType entry_type, ByteString name);
    virtual ~FileSystemEntry() override = default;

    bool is_file() const;
    bool is_directory() const;
    ByteString name() const;

private:
    FileSystemEntry(EntryType entry_type, ByteString name);

    EntryType m_entry_type;
    ByteString m_name;
};

}
