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
#include <AK/Weakable.h>
#include <LibWeb/FileAPI/SerializedBlobURLEntry.h>
#include <LibWebView/Export.h>
#include <LibWebView/Forward.h>

namespace WebView {

using BlobURLEntryOwner = Variant<WeakPtr<WebContentClient>, WeakPtr<WebWorkerClient>>;

// https://w3c.github.io/FileAPI/#BlobURLStore
class WEBVIEW_API BlobURLStore : public Weakable<BlobURLStore> {
public:
    URL::BlobURLEntry::Token add_entry(Utf16String url, Web::FileAPI::SerializedBlobURLEntry, BlobURLEntryOwner added_by);
    void remove_entries(Vector<Utf16String> const& urls, URL::Origin const& environment_origin, BlobURLEntryOwner const& removed_by);
    void remove_entries_added_by(BlobURLEntryOwner const&);

    Optional<Web::FileAPI::SerializedBlobURLEntry> resolve(Utf16String const& url, Optional<URL::BlobURLEntry::Token>) const;

    // A revoked entry outlives its revocation for as long as handles are held on its token, and is dropped with the
    // last of them. Session history holds none, so a blob URL cannot be traversed back to once it is revoked.
    void acquire(URL::BlobURLEntry::Token);
    void release(URL::BlobURLEntry::Token);

private:
    struct Entry {
        Web::FileAPI::SerializedBlobURLEntry entry;
        BlobURLEntryOwner added_by;
        bool revoked { false };
        size_t handles { 0 };
    };

    void remove_entry(Utf16String const& url);

    HashMap<Utf16String, Entry> m_entries;
    HashMap<URL::BlobURLEntry::Token, Utf16String> m_entry_urls_by_token;
};

// Keeps a blob URL entry alive for as long as it is held.
class WEBVIEW_API BlobURLHandle {
public:
    BlobURLHandle() = default;
    BlobURLHandle(BlobURLStore&, URL::BlobURLEntry::Token);
    ~BlobURLHandle() { release(); }

    BlobURLHandle(BlobURLHandle&&);
    BlobURLHandle& operator=(BlobURLHandle&&);

    BlobURLHandle(BlobURLHandle const&) = delete;
    BlobURLHandle& operator=(BlobURLHandle const&) = delete;

    // A handle on the blob URL entry `url` names, if it names one and the store still has it.
    static BlobURLHandle for_url(BlobURLStore*, Optional<URL::URL> const& url);

private:
    void release();

    WeakPtr<BlobURLStore> m_store;
    URL::BlobURLEntry::Token m_token { 0 };
};

}
