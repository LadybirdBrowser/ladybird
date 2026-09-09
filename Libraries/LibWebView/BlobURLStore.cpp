/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/FileAPI/BlobURLStore.h>
#include <LibWebView/BlobURLStore.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebWorkerClient.h>

namespace WebView {

static void const* owner_pointer(BlobURLEntryOwner const& owner)
{
    return owner.visit([](auto const& client) -> void const* { return client.ptr(); });
}

// https://w3c.github.io/FileAPI/#add-an-entry
void BlobURLStore::add_entry(Utf16String url, Web::FileAPI::SerializedBlobURLEntry entry, BlobURLEntryOwner added_by)
{
    // NB: Blob URLs are random, so one that is already here came from a different process. Leave it alone.
    if (m_entries.contains(url))
        return;

    m_entries.set(move(url), Entry { move(entry), move(added_by) });
}

// https://w3c.github.io/FileAPI/#dfn-revokeObjectURL
void BlobURLStore::remove_entries(Vector<Utf16String> const& urls, URL::Origin const& environment_origin, BlobURLEntryOwner const& removed_by)
{
    for (auto const& url : urls) {
        // 3. Let entry be urlRecord’s blob URL entry.
        auto entry = m_entries.get(url);

        // 4. If entry is null, return.
        if (!entry.has_value())
            continue;

        // 5. Let isAuthorized be the result of checking for same-partition blob URL usage with entry and the current settings object.
        bool is_authorized = Web::FileAPI::check_for_same_partition_blob_url_usage(entry->entry.origin, environment_origin);

        // 6. If isAuthorized is false, then return.
        if (!is_authorized)
            continue;

        // 7. Remove an entry from the Blob URL Store for url.
        auto added_by = entry->added_by;
        m_entries.remove(url);

        // NB: The process that added the entry keeps it for its parser. Tell it, unless it is the one revoking.
        if (owner_pointer(added_by) != owner_pointer(removed_by)) {
            added_by.visit([&](auto const& client) {
                if (client)
                    client->async_blob_url_entry_removed(url);
            });
        }
    }
}

void BlobURLStore::remove_entries_added_by(BlobURLEntryOwner const& owner)
{
    m_entries.remove_all_matching([&](auto const&, auto const& entry) { return owner_pointer(entry.added_by) == owner_pointer(owner); });
}

// https://w3c.github.io/FileAPI/#blob-url-resolve
Optional<Web::FileAPI::SerializedBlobURLEntry> BlobURLStore::resolve(Utf16String const& url) const
{
    auto entry = m_entries.get(url);
    if (!entry.has_value())
        return {};
    return entry->entry;
}

}
