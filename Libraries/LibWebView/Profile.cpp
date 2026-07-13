/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Hex.h>
#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <LibCore/Directory.h>
#include <LibCrypto/Hash/SHA2.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebView/Profile.h>

namespace WebView {

static ByteString profile_path(StringView root, StringView identifier)
{
    return LexicalPath::join(root, "Ladybird"sv, "Profiles"sv, identifier).string();
}

static ErrorOr<void> create_profile_directories(ProfilePaths const& paths)
{
    TRY(Core::Directory::create(paths.config, Core::Directory::CreateDirectories::Yes, 0700));
    TRY(Core::Directory::create(paths.data, Core::Directory::CreateDirectories::Yes, 0700));
    TRY(Core::Directory::create(paths.cache, Core::Directory::CreateDirectories::Yes, 0700));
    TRY(Core::Directory::create(paths.runtime, Core::Directory::CreateDirectories::Yes, 0700));
    return {};
}

Profile::Profile(ByteString identifier, ProfilePaths paths, Optional<ByteString> temporary_root)
    : m_identifier(move(identifier))
    , m_paths(move(paths))
    , m_temporary_root(move(temporary_root))
{
}

Profile::Profile(Profile&& other)
    : m_identifier(move(other.m_identifier))
    , m_paths(move(other.m_paths))
    , m_temporary_root(move(other.m_temporary_root))
{
    other.m_temporary_root.clear();
}

Profile& Profile::operator=(Profile&& other)
{
    if (this == &other)
        return *this;

    remove_temporary_root();
    m_identifier = move(other.m_identifier);
    m_paths = move(other.m_paths);
    m_temporary_root = move(other.m_temporary_root);
    other.m_temporary_root.clear();
    return *this;
}

Profile::~Profile()
{
    remove_temporary_root();
}

void Profile::remove_temporary_root()
{
    if (!m_temporary_root.has_value())
        return;
    if (FileSystem::exists(*m_temporary_root)) {
        if (auto result = FileSystem::remove(*m_temporary_root, FileSystem::RecursionMode::Allowed); result.is_error())
            warnln("Unable to remove temporary Ladybird profile '{}': {}", *m_temporary_root, result.error());
    }
    m_temporary_root.clear();
}

bool Profile::is_valid_name(StringView name)
{
    if (name.is_empty())
        return false;

    for (auto character : name.bytes()) {
        if (!is_ascii_lower_alpha(character) && !is_ascii_digit(character) && character != '_' && character != '-')
            return false;
    }
    return true;
}

ByteString Profile::routing_identifier(StringView canonical_profile_root)
{
    auto digest = Crypto::Hash::SHA256::hash(canonical_profile_root);
    return encode_hex(digest.bytes().slice(0, 16));
}

ErrorOr<Profile> Profile::create(ProfileSelection const& selection, ProfileRoots const& roots)
{
    auto selector_count = static_cast<unsigned>(selection.name.has_value())
        + static_cast<unsigned>(selection.path.has_value())
        + static_cast<unsigned>(selection.temporary);
    if (selector_count > 1)
        return Error::from_string_literal("Profile selectors are mutually exclusive");

    if (selection.name.has_value() && !is_valid_name(*selection.name))
        return Error::from_string_literal("Profile names may only contain lowercase ASCII letters, digits, '_' and '-'");

    if (selection.path.has_value()) {
        auto root = LexicalPath { *selection.path };
        if (!root.is_absolute())
            return Error::from_string_literal("Profile path must be absolute");

        TRY(Core::Directory::create(root, Core::Directory::CreateDirectories::Yes, 0700));
        root = LexicalPath { TRY(FileSystem::real_path(root.string())) };
        auto canonical_root = root.string();
        ProfilePaths paths {
            .config = root.append("config"sv).string(),
            .data = root.append("data"sv).string(),
            .cache = root.append("cache"sv).string(),
            .runtime = root.append("runtime"sv).string(),
            .identity = canonical_root,
        };
        TRY(create_profile_directories(paths));
        return Profile { "custom"sv, move(paths) };
    }

    if (selection.temporary) {
        TRY(Core::Directory::create(roots.temporary, Core::Directory::CreateDirectories::Yes, 0700));

        auto root = LexicalPath::join(roots.temporary, ByteString::formatted("ladybird-profile-{:016x}-{:016x}", get_random<u64>(), get_random<u64>())).string();
        TRY(Core::Directory::create(root, Core::Directory::CreateDirectories::Yes, 0700));
        root = TRY(FileSystem::real_path(root));

        ProfilePaths paths {
            .config = LexicalPath::join(root, "config"sv).string(),
            .data = LexicalPath::join(root, "data"sv).string(),
            .cache = LexicalPath::join(root, "cache"sv).string(),
            .runtime = LexicalPath::join(root, "runtime"sv).string(),
            .identity = root,
        };
        auto result = create_profile_directories(paths);
        if (result.is_error()) {
            MUST(FileSystem::remove(root, FileSystem::RecursionMode::Allowed));
            return result.release_error();
        }
        return Profile { "temporary"sv, move(paths), move(root) };
    }

    auto identifier = selection.name.value_or("default"sv);

    ProfilePaths paths {
        .config = profile_path(roots.config, identifier),
        .data = profile_path(roots.data, identifier),
        .cache = profile_path(roots.cache, identifier),
        .runtime = profile_path(roots.runtime, identifier),
        .identity = profile_path(roots.data, identifier),
    };

    TRY(create_profile_directories(paths));
    paths.identity = TRY(FileSystem::real_path(paths.identity));
    return Profile { move(identifier), move(paths) };
}

ErrorOr<Profile> Profile::create_legacy(ProfileRoots const& roots)
{
    ProfilePaths paths {
        .config = LexicalPath::join(roots.config, "Ladybird"sv).string(),
        .data = LexicalPath::join(roots.data, "Ladybird"sv).string(),
        .cache = LexicalPath::join(roots.cache, "Ladybird"sv).string(),
        .runtime = roots.runtime,
        .identity = LexicalPath::join(roots.data, "Ladybird"sv).string(),
    };

    TRY(create_profile_directories(paths));
    paths.identity = TRY(FileSystem::real_path(paths.identity));
    return Profile { "legacy"sv, move(paths) };
}

}
