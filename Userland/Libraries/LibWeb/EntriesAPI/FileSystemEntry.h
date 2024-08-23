/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::EntriesAPI {

enum class EntryType {
    File,
    Directory,
};

class FileSystemEntry final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(FileSystemEntry, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(FileSystemEntry);

public:
    static JS::NonnullGCPtr<FileSystemEntry> create(JS::Realm&, EntryType entry_type, ByteString name);
    virtual ~FileSystemEntry() override = default;

    bool is_file() const;
    bool is_directory() const;
    ByteString name() const;

private:
    FileSystemEntry(JS::Realm&, EntryType entry_type, ByteString name);

    virtual void initialize(JS::Realm&) override;

    EntryType m_entry_type;
    ByteString m_name;
};

}
