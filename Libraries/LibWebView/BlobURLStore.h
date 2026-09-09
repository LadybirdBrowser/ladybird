/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Utf16String.h>
#include <AK/Variant.h>
#include <AK/WeakPtr.h>
#include <LibWeb/FileAPI/SerializedBlobURLEntry.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

using BlobURLEntryOwner = Variant<WeakPtr<WebContentClient>, WeakPtr<WebWorkerClient>>;

// https://w3c.github.io/FileAPI/#BlobURLStore
class WEBVIEW_API BlobURLStore {
public:
    void add_entry(Utf16String url, Web::FileAPI::SerializedBlobURLEntry, BlobURLEntryOwner added_by);
    void remove_entries(Vector<Utf16String> const& urls, URL::Origin const& environment_origin, BlobURLEntryOwner const& removed_by);
    void remove_entries_added_by(BlobURLEntryOwner const&);

    Optional<Web::FileAPI::SerializedBlobURLEntry> resolve(Utf16String const& url) const;

private:
    struct Entry {
        Web::FileAPI::SerializedBlobURLEntry entry;
        BlobURLEntryOwner added_by;
    };

    HashMap<Utf16String, Entry> m_entries;
};

}
