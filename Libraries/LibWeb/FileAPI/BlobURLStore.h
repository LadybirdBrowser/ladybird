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
    GC::Root<Blob> object; // FIXME: This could also be a MediaSource after we implement MSE.
    GC::Root<HTML::EnvironmentSettingsObject> environment;
};

// https://w3c.github.io/FileAPI/#BlobURLStore
using BlobURLStore = HashMap<String, BlobURLEntry>;

BlobURLStore& blob_url_store();
ErrorOr<String> generate_new_blob_url();
ErrorOr<String> add_entry_to_blob_url_store(GC::Ref<Blob> object);
bool check_for_same_partition_blob_url_usage(URL::BlobURLEntry const&, GC::Ref<HTML::Environment>);
struct NavigationEnvironment { };
Optional<URL::BlobURLEntry::Object> obtain_a_blob_object(URL::BlobURLEntry const&, Variant<GC::Ref<HTML::Environment>, NavigationEnvironment> environment);
void remove_entry_from_blob_url_store(URL::URL const& url);
Optional<BlobURLEntry const&> resolve_a_blob_url(URL::URL const&);

void run_unloading_cleanup_steps(GC::Ref<DOM::Document>);

}
