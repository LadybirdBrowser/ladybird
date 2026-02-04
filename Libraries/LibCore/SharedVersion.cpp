/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <LibCore/SharedVersion.h>

namespace Core {

static size_t SHARED_VERSION_BUFFER_SIZE = PAGE_SIZE;
static size_t SHARED_VERSION_BUFFER_COUNT = SHARED_VERSION_BUFFER_SIZE / sizeof(SharedVersion);

AnonymousBuffer create_shared_version_buffer()
{
    return MUST(AnonymousBuffer::create_with_size(SHARED_VERSION_BUFFER_SIZE));
}

bool initialize_shared_version(AnonymousBuffer& shared_version_buffer, SharedVersionIndex shared_version_index)
{
    if (!shared_version_buffer.is_valid() || shared_version_index >= SHARED_VERSION_BUFFER_COUNT)
        return false;

    auto* shared_versions = shared_version_buffer.data<SharedVersion>();
    shared_versions[shared_version_index] = INITIAL_SHARED_VERSION;

    return true;
}

void increment_shared_version(AnonymousBuffer& shared_version_buffer, SharedVersionIndex shared_version_index)
{
    if (!shared_version_buffer.is_valid() || shared_version_index >= SHARED_VERSION_BUFFER_COUNT)
        return;

    auto* shared_versions = shared_version_buffer.data<SharedVersion>();
    ++shared_versions[shared_version_index];
}

Optional<SharedVersion> get_shared_version(AnonymousBuffer const& shared_version_buffer, SharedVersionIndex shared_version_index)
{
    if (!shared_version_buffer.is_valid() || shared_version_index >= SHARED_VERSION_BUFFER_COUNT)
        return {};

    auto const* shared_versions = shared_version_buffer.data<SharedVersion>();
    return shared_versions[shared_version_index];
}

}
