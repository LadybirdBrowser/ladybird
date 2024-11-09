/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <AK/StringView.h>
#include <LibCore/Resource.h>

namespace Core {

class ResourceImplementation {
public:
    ErrorOr<NonnullRefPtr<Resource>> load_from_uri(StringView);
    Vector<String> child_names(Resource const&);
    String filesystem_path(Resource const&);

    virtual ~ResourceImplementation() = default;

    static void install(OwnPtr<ResourceImplementation>);
    static ResourceImplementation& the();

protected:
    virtual ErrorOr<NonnullRefPtr<Resource>> load_from_resource_scheme_uri(StringView) = 0;
    virtual Vector<String> child_names_for_resource_scheme(Resource const&) = 0;
    virtual String filesystem_path_for_resource_scheme(String const&) = 0;

    static NonnullRefPtr<Resource> make_resource(String full_path, NonnullOwnPtr<Core::MappedFile>, time_t modified_time);
    static NonnullRefPtr<Resource> make_resource(String full_path, ByteBuffer, time_t modified_time);
    static NonnullRefPtr<Resource> make_directory_resource(String full_path, time_t modified_time);
};

}
