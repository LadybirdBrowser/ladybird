/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <LibCore/Resource.h>
#include <LibCore/ResourceImplementation.h>
#include <LibCore/System.h>

namespace Core {

Resource::Resource(String path, Scheme scheme, NonnullOwnPtr<Core::MappedFile> file, time_t modified_time)
    : m_path(move(path))
    , m_scheme(scheme)
    , m_data(move(file))
    , m_modified_time(modified_time)
{
}

Resource::Resource(String path, Scheme scheme, ByteBuffer buffer, time_t modified_time)
    : m_path(move(path))
    , m_scheme(scheme)
    , m_data(move(buffer))
    , m_modified_time(modified_time)
{
}

Resource::Resource(String path, Scheme scheme, DirectoryTag, time_t modified_time)
    : m_path(move(path))
    , m_scheme(scheme)
    , m_data(DirectoryTag {})
    , m_modified_time(modified_time)
{
}

ErrorOr<NonnullRefPtr<Resource>> Resource::load_from_filesystem(StringView path)
{
    auto filepath = LexicalPath(path);

    if (filepath.is_absolute())
        return load_from_uri(TRY(String::formatted("file://{}", path)));

    auto cwd = TRY(Core::System::getcwd());
    return load_from_uri(TRY(String::formatted("file://{}", filepath.prepend(cwd).string())));
}

ErrorOr<NonnullRefPtr<Resource>> Resource::load_from_uri(StringView uri)
{
    return ResourceImplementation::the().load_from_uri(uri);
}

String Resource::uri() const
{
    return MUST(String::formatted("{}://{}", m_scheme == Scheme::Resource ? "resource"sv : "file"sv, m_path));
}

String Resource::filesystem_path() const
{
    return ResourceImplementation::the().filesystem_path(*this);
}

String Resource::file_url() const
{
    if (m_scheme == Scheme::File)
        return uri();

    return MUST(String::formatted("file://{}", filesystem_path()));
}

Optional<time_t> Resource::modified_time() const
{
    return m_modified_time;
}

String Resource::filename() const
{
    return MUST(String::from_utf8(LexicalPath(m_path.bytes_as_string_view()).basename()));
}

Vector<String> Resource::children() const
{
    return ResourceImplementation::the().child_names(*this);
}

ByteBuffer Resource::clone_data() const
{
    return m_data.visit(
        [](NonnullOwnPtr<Core::MappedFile> const& file) { return MUST(ByteBuffer::copy(file->bytes())); },
        [](ByteBuffer const& buffer) { return buffer; },
        [](DirectoryTag) -> ByteBuffer { VERIFY_NOT_REACHED(); });
}

ByteBuffer Resource::release_data() &&
{
    VERIFY(!m_data.has<DirectoryTag>());

    if (m_data.has<NonnullOwnPtr<Core::MappedFile>>())
        return MUST(ByteBuffer::copy(m_data.get<NonnullOwnPtr<Core::MappedFile>>()->bytes()));
    return move(m_data).get<ByteBuffer>();
}

ReadonlyBytes Resource::data() const
{
    return m_data.visit(
        [](NonnullOwnPtr<Core::MappedFile> const& file) { return file->bytes(); },
        [](ByteBuffer const& buffer) { return buffer.bytes(); },
        [](DirectoryTag) -> ReadonlyBytes { VERIFY_NOT_REACHED(); });
}

FixedMemoryStream Resource::stream() const
{
    return FixedMemoryStream(data());
}

}
