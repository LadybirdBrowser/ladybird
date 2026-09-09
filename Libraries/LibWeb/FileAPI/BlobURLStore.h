/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibGC/Ptr.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>

namespace Web::FileAPI {

// https://w3c.github.io/FileAPI/#blob-url-entry
struct BlobURLEntry {
    using Object = Variant<GC::Ref<Blob>, GC::Ref<MediaSourceExtensions::MediaSource>>;

    Object object;
    GC::Ref<HTML::EnvironmentSettingsObject> environment;
};

Utf16String generate_new_blob_url();
ErrorOr<Utf16String> add_entry_to_blob_url_store(BlobURLEntry::Object);
WEB_API bool check_for_same_partition_blob_url_usage(URL::Origin const& blob_url_entry_origin, URL::Origin const& environment_origin);
bool check_for_same_partition_blob_url_usage(URL::Origin const& blob_url_entry_origin, GC::Ref<HTML::Environment>);
struct TopLevelNavigation { };
struct TopLevelSelfFetch { };
WEB_API Optional<URL::BlobURLEntry::Object> obtain_a_blob_object(URL::BlobURLEntry const&, Variant<GC::Ref<HTML::Environment>, TopLevelNavigation, TopLevelSelfFetch> environment);
WEB_API void remove_entry_from_blob_url_store(URL::URL const& url);
Optional<URL::BlobURLEntry> resolve_a_blob_url(URL::URL const&);
WEB_API Optional<URL::BlobURLEntry> blob_url_entry_in_the_user_agent_store(Page&, URL::URL const&);
Optional<BlobURLEntry const&> local_blob_url_entry(URL::URL const&);

void run_unloading_cleanup_steps(GC::Ref<DOM::Document>);

}
