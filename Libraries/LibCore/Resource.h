/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/MemoryStream.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Variant.h>
#include <LibCore/File.h>
#include <LibCore/MappedFile.h>

namespace Core {

class Resource : public RefCounted<Resource> {
public:
    static ErrorOr<NonnullRefPtr<Resource>> load_from_filesystem(StringView);
    static ErrorOr<NonnullRefPtr<Resource>> load_from_uri(StringView);

    [[nodiscard]] bool is_file() const { return !m_data.has<DirectoryTag>(); }
    [[nodiscard]] bool is_directory() const { return m_data.has<DirectoryTag>(); }

    [[nodiscard]] String uri() const;
    [[nodiscard]] String filename() const;
    [[nodiscard]] String filesystem_path() const;
    [[nodiscard]] String file_url() const;
    [[nodiscard]] Optional<time_t> modified_time() const;

    [[nodiscard]] ByteBuffer clone_data() const;
    [[nodiscard]] ByteBuffer release_data() &&;
    [[nodiscard]] ReadonlyBytes data() const;
    [[nodiscard]] FixedMemoryStream stream() const;

    [[nodiscard]] Vector<String> children() const;
    // Depth-first
    template<IteratorFunction<Resource const&> Callback>
    IterationDecision for_each_descendant(Callback&&) const;

    template<IteratorFunction<Resource const&> Callback>
    void for_each_descendant_file(Callback&&) const;

    struct DirectoryTag { };

private:
    friend class ResourceImplementation;

    enum class Scheme {
        File,
        Resource,
    };

    Resource(String path, Scheme, NonnullOwnPtr<Core::MappedFile>, time_t modified_time);
    Resource(String path, Scheme, ByteBuffer, time_t modified_time);
    Resource(String path, Scheme, DirectoryTag, time_t modified_time);

    String m_path; // Relative to scheme root. File: abspath, Resource: resource root
    Scheme m_scheme;

    Variant<DirectoryTag, NonnullOwnPtr<Core::MappedFile>, ByteBuffer> m_data;
    time_t m_modified_time {};
};

template<IteratorFunction<Resource const&> Callback>
IterationDecision Resource::for_each_descendant(Callback&& callback) const
{
    auto children = this->children();
    for (auto const& child : children) {
        if (auto child_resource = load_from_uri(MUST(String::formatted("{}/{}", uri(), child))); !child_resource.is_error()) {
            if (callback(*child_resource.value()) == IterationDecision::Break)
                return IterationDecision::Break;
            if (child_resource.value()->for_each_descendant(callback) == IterationDecision::Break)
                return IterationDecision::Break;
        }
    }
    return IterationDecision::Continue;
}

template<IteratorFunction<Resource const&> Callback>
void Resource::for_each_descendant_file(Callback&& callback) const
{
    for_each_descendant([callback = forward<Callback>(callback)](Resource const& resource) {
        if (resource.is_directory())
            return IterationDecision::Continue;
        return callback(resource);
    });
}

}
