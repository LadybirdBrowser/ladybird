/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/DirIterator.h>
#include <LibCore/ResourceImplementation.h>
#include <LibCore/ResourceImplementationFile.h>
#include <LibCore/System.h>

namespace Core {

static OwnPtr<ResourceImplementation> s_the;

void ResourceImplementation::install(OwnPtr<ResourceImplementation> the)
{
    s_the = move(the);
}

ResourceImplementation& ResourceImplementation::the()
{
    if (!s_the)
        install(make<ResourceImplementationFile>("/res"_string));
    return *s_the;
}

NonnullRefPtr<Resource> ResourceImplementation::make_resource(String full_path, NonnullOwnPtr<Core::MappedFile> file, time_t modified_time)
{
    return adopt_ref(*new Resource(move(full_path), Resource::Scheme::Resource, move(file), modified_time));
}

NonnullRefPtr<Resource> ResourceImplementation::make_resource(String full_path, ByteBuffer buffer, time_t modified_time)
{
    return adopt_ref(*new Resource(move(full_path), Resource::Scheme::Resource, move(buffer), modified_time));
}

NonnullRefPtr<Resource> ResourceImplementation::make_directory_resource(String full_path, time_t modified_time)
{
    return adopt_ref(*new Resource(move(full_path), Resource::Scheme::Resource, Resource::DirectoryTag {}, modified_time));
}

ErrorOr<NonnullRefPtr<Resource>> ResourceImplementation::load_from_uri(StringView uri)
{
    StringView const file_scheme = "file://"sv;
    StringView const resource_scheme = "resource://"sv;

    if (uri.starts_with(resource_scheme))
        return load_from_resource_scheme_uri(uri);

    if (uri.starts_with(file_scheme)) {
        auto path = uri.substring_view(file_scheme.length());
        auto utf8_path = TRY(String::from_utf8(path));
        auto st = TRY(System::stat(utf8_path));
        if (S_ISDIR(st.st_mode))
            return adopt_ref(*new Resource(utf8_path, Resource::Scheme::File, Resource::DirectoryTag {}, st.st_mtime));
        auto mapped_file = TRY(MappedFile::map(path));
        return adopt_ref(*new Resource(utf8_path, Resource::Scheme::File, move(mapped_file), st.st_mtime));
    }

    dbgln("ResourceImplementation: Unknown scheme for {}", uri);
    return Error::from_string_literal("Invalid scheme");
}

Vector<String> ResourceImplementation::child_names(Resource const& resource)
{
    if (!resource.is_directory())
        return {};

    if (resource.m_scheme == Resource::Scheme::Resource)
        return child_names_for_resource_scheme(resource);

    VERIFY(resource.m_scheme == Resource::Scheme::File);

    Vector<String> children;
    Core::DirIterator it(resource.filesystem_path().to_byte_string(), Core::DirIterator::SkipParentAndBaseDir);
    while (it.has_next())
        children.append(MUST(String::from_byte_string(it.next_path())));

    return children;
}

String ResourceImplementation::filesystem_path(Resource const& resource)
{
    if (resource.m_scheme == Resource::Scheme::Resource)
        return filesystem_path_for_resource_scheme(resource.m_path);

    VERIFY(resource.m_scheme == Resource::Scheme::File);

    return resource.m_path;
}

}
