/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibURL/URL.h>

namespace RequestServer {

struct ResourceSubstitution {
    ByteString file_path;
    Optional<String> content_type;
    u32 status_code { 200 };
};

class ResourceSubstitutionMap {
public:
    static ErrorOr<NonnullOwnPtr<ResourceSubstitutionMap>> load_from_file(StringView path);

    Optional<ResourceSubstitution const&> lookup(URL::URL const&) const;

private:
    ResourceSubstitutionMap() = default;

    HashMap<String, ResourceSubstitution> m_substitutions;
};

}
