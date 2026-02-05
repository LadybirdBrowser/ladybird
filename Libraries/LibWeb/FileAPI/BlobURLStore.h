/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/String.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibURL/URL.h>
#include <LibWeb/Forward.h>

namespace Web::FileAPI {

// https://w3c.github.io/FileAPI/#blob-url-entry
struct BlobURLEntry {
    using Object = Variant<GC::Root<Blob>, GC::Root<MediaSourceExtensions::MediaSource>>;

    Object object;
    GC::Root<HTML::EnvironmentSettingsObject> environment;
};

// https://w3c.github.io/FileAPI/#BlobURLStore
using BlobURLStore = HashMap<String, BlobURLEntry>;

BlobURLStore& blob_url_store();
ErrorOr<Utf16String> generate_new_blob_url();
ErrorOr<Utf16String> add_entry_to_blob_url_store(BlobURLEntry::Object);
bool check_for_same_partition_blob_url_usage(URL::BlobURLEntry const&, GC::Ref<HTML::Environment>);
struct NavigationEnvironment { };
WEB_API Optional<URL::BlobURLEntry::Object> obtain_a_blob_object(URL::BlobURLEntry const&, Variant<GC::Ref<HTML::Environment>, NavigationEnvironment> environment);
void remove_entry_from_blob_url_store(URL::URL const& url);
Optional<BlobURLEntry const&> resolve_a_blob_url(URL::URL const&);

void run_unloading_cleanup_steps(GC::Ref<DOM::Document>);

}
