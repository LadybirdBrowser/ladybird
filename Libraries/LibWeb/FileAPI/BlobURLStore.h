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
#include <LibURL/Forward.h>
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
ErrorOr<void> remove_entry_from_blob_url_store(StringView url);
Optional<BlobURLEntry> resolve_a_blob_url(URL::URL const&);

void run_unloading_cleanup_steps(GC::Ref<DOM::Document>);

}
