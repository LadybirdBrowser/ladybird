/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefPtr.h>
#include <AK/StringView.h>
#include <LibCore/Resource.h>
#include <LibCore/ResourceImplementation.h>

namespace Core {

class ResourceImplementationFile : public ResourceImplementation {
public:
    explicit ResourceImplementationFile(String base_directory);

    virtual ErrorOr<NonnullRefPtr<Resource>> load_from_resource_scheme_uri(StringView) override;
    virtual Vector<String> child_names_for_resource_scheme(Resource const&) override;
    virtual String filesystem_path_for_resource_scheme(String const&) override;

private:
    String m_base_directory;
};

}
