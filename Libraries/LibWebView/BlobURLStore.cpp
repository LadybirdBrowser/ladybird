/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Random.h>
#include <LibWeb/FileAPI/BlobURLStore.h>
#include <LibWebView/BlobURLStore.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebWorkerClient.h>

namespace WebView {

static void const* owner_pointer(BlobURLEntryOwner const& owner)
{
    return owner.visit([](auto const& client) -> void const* { return client.ptr(); });
}

static Optional<URL::BlobURLEntry::Token> token_of(Web::FileAPI::SerializedBlobURLEntry const& entry)
{
    if (auto const* blob = entry.object.get_pointer<Web::FileAPI::SerializedBlobURLEntry::Blob>())
        return blob->token;
    return {};
}

// https://w3c.github.io/FileAPI/#add-an-entry
URL::BlobURLEntry::Token BlobURLStore::add_entry(Utf16String url, Web::FileAPI::SerializedBlobURLEntry entry, BlobURLEntryOwner added_by)
{
    // NB: Blob URLs are random, so one that is already here came from a different process. Leave it alone.
    if (m_entries.contains(url))
        return 0;

    // NB: The token is unguessable and assigned here so that a process cannot name an entry it was not given.
    URL::BlobURLEntry::Token token = 0;
    if (auto* blob = entry.object.get_pointer<Web::FileAPI::SerializedBlobURLEntry::Blob>()) {
        token = get_random<URL::BlobURLEntry::Token>();
        blob->token = token;
        m_entry_urls_by_token.set(token, url);
    }

    m_entries.set(move(url), Entry { move(entry), move(added_by), false, 0 });
    return token;
}

void BlobURLStore::remove_entry(Utf16String const& url)
{
    auto entry = m_entries.get(url);
    if (!entry.has_value())
        return;

    if (auto token = token_of(entry->entry); token.has_value())
        m_entry_urls_by_token.remove(*token);
    m_entries.remove(url);
}

// https://w3c.github.io/FileAPI/#dfn-revokeObjectURL
void BlobURLStore::remove_entries(Vector<Utf16String> const& urls, URL::Origin const& environment_origin, BlobURLEntryOwner const& removed_by)
{
    for (auto const& url : urls) {
        // 3. Let entry be urlRecord’s blob URL entry.
        auto entry = m_entries.get(url);

        // 4. If entry is null, return.
        if (!entry.has_value() || entry->revoked)
            continue;

        // 5. Let isAuthorized be the result of checking for same-partition blob URL usage with entry and the current settings object.
        bool is_authorized = Web::FileAPI::check_for_same_partition_blob_url_usage(entry->entry.origin, environment_origin);

        // 6. If isAuthorized is false, then return.
        if (!is_authorized)
            continue;

        // 7. Remove an entry from the Blob URL Store for url.
        auto added_by = entry->added_by;
        auto handles = entry->handles;
        entry->revoked = true;

        // NB: The process that added the entry keeps it for its parser. Tell it, unless it is the one revoking.
        if (owner_pointer(added_by) != owner_pointer(removed_by)) {
            added_by.visit([&](auto const& client) {
                if (client)
                    client->async_blob_url_entry_removed(url);
            });
        }

        if (handles == 0)
            remove_entry(url);
    }
}

void BlobURLStore::remove_entries_added_by(BlobURLEntryOwner const& owner)
{
    auto const* owner_to_remove = owner_pointer(owner);
    if (!owner_to_remove)
        return;

    m_entries.remove_all_matching([&](auto const&, auto const& entry) {
        if (owner_pointer(entry.added_by) != owner_to_remove)
            return false;
        if (auto token = token_of(entry.entry); token.has_value())
            m_entry_urls_by_token.remove(*token);
        return true;
    });
}

void BlobURLStore::acquire(URL::BlobURLEntry::Token token)
{
    auto url = m_entry_urls_by_token.get(token);
    if (!url.has_value())
        return;

    if (auto entry = m_entries.get(*url); entry.has_value())
        ++entry->handles;
}

void BlobURLStore::release(URL::BlobURLEntry::Token token)
{
    auto url = m_entry_urls_by_token.get(token);
    if (!url.has_value())
        return;

    auto url_string = *url;
    auto entry = m_entries.get(url_string);
    if (!entry.has_value() || entry->handles == 0)
        return;

    if (--entry->handles == 0 && entry->revoked)
        remove_entry(url_string);
}

// https://w3c.github.io/FileAPI/#blob-url-resolve
Optional<Web::FileAPI::SerializedBlobURLEntry> BlobURLStore::resolve(Utf16String const& url, Optional<URL::BlobURLEntry::Token> token) const
{
    // NB: A revoked entry still answers to its token, but no longer to its URL. See URL::BlobURLEntry::Token.
    if (token.has_value()) {
        auto entry_url = m_entry_urls_by_token.get(*token);
        if (!entry_url.has_value())
            return {};
        if (auto entry = m_entries.get(*entry_url); entry.has_value())
            return entry->entry;
        return {};
    }

    auto entry = m_entries.get(url);
    if (!entry.has_value() || entry->revoked)
        return {};
    return entry->entry;
}

BlobURLHandle::BlobURLHandle(BlobURLStore& store, URL::BlobURLEntry::Token token)
    : m_store(store.make_weak_ptr())
    , m_token(token)
{
    store.acquire(token);
}

BlobURLHandle::BlobURLHandle(BlobURLHandle&& other)
    : m_store(move(other.m_store))
    , m_token(exchange(other.m_token, 0))
{
}

BlobURLHandle& BlobURLHandle::operator=(BlobURLHandle&& other)
{
    if (this != &other) {
        release();
        m_store = move(other.m_store);
        m_token = exchange(other.m_token, 0);
    }
    return *this;
}

void BlobURLHandle::release()
{
    if (m_token == 0)
        return;
    if (auto* store = m_store.ptr())
        store->release(m_token);
    m_token = 0;
}

BlobURLHandle BlobURLHandle::for_url(BlobURLStore* store, Optional<URL::URL> const& url)
{
    if (!store || !url.has_value() || !url->blob_url_entry().has_value())
        return {};

    auto const* blob = url->blob_url_entry()->object.get_pointer<URL::BlobURLEntry::Blob>();
    if (!blob || blob->token == 0)
        return {};

    return BlobURLHandle { *store, blob->token };
}

}
