/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/CharacterTypes.h>
#include <AK/String.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Environment.h>
#include <LibCore/System.h>
#include <LibSandbox/Sandbox.h>
#include <LibSandbox/Seccomp.h>
#include <RequestServer/ResourceSubstitutionMap.h>
#include <RequestServer/Sandbox.h>
#include <curl/curl.h>
#include <openssl/conf.h>
#include <openssl/crypto.h>
#include <openssl/x509.h>
#include <string.h>
#include <sys/stat.h>

namespace RequestServer {

// The links are named after eight hexadecimal digits of the hash and a decimal counter, such as 002c0b4f.0.
static bool is_certificate_hash_link_name(StringView name)
{
    if (name.length() < 10 || name[8] != '.')
        return false;
    return all_of(name.substring_view(0, 8), is_ascii_hex_digit) && all_of(name.substring_view(9), is_ascii_digit);
}

// OpenSSL looks certificates up in a directory through links named after a hash of their subject, and distributions
// point those links at files that live elsewhere, such as /usr/share/ca-certificates. A rule on the directory doesn't
// reach those files, so grant each of them as well.
static ErrorOr<void> add_certificate_directory(Vector<Sandbox::PathRule>& paths, StringView directory)
{
    // OpenSSL skips the directories in a list that it can't read, so one of those must not keep the sandbox from
    // applying.
    if (auto metadata = Core::System::stat(directory); metadata.is_error() || !S_ISDIR(metadata.value().st_mode))
        return {};

    // Nothing is added for a directory that an earlier list already named.
    auto rule_count = paths.size();
    TRY(Sandbox::add_path_if_exists(paths, directory, Sandbox::PathRule::Access::ReadOnly));
    if (paths.size() == rule_count)
        return {};

    Core::DirIterator iterator { directory.to_byte_string(), Core::DirIterator::SkipParentAndBaseDir };
    while (iterator.has_next()) {
        auto entry = iterator.next();
        if (entry->type != Core::DirectoryEntry::Type::SymbolicLink && entry->type != Core::DirectoryEntry::Type::Unknown)
            continue;
        if (!is_certificate_hash_link_name(entry->name))
            continue;
        auto link = ByteString::formatted("{}/{}", directory, entry->name);

        // A link to a directory would grant everything below it, and a certificate is always a file.
        auto target_metadata = Core::System::stat(link);
        if (target_metadata.is_error() || !S_ISREG(target_metadata.value().st_mode))
            continue;

        // The rule is bound to whatever the link resolves to when the sandbox opens it.
        TRY(Sandbox::add_path_if_exists(paths, link, Sandbox::PathRule::Access::ReadOnly));
    }

    return {};
}

// OpenSSL and libcurl both accept a list of certificate directories separated by colons.
static ErrorOr<void> add_certificate_directories(Vector<Sandbox::PathRule>& paths, StringView directories)
{
    for (auto directory : directories.split_view(':'))
        TRY(add_certificate_directory(paths, directory));
    return {};
}

// Landlock binds to inodes. Open each bundle path to grant its symlink target, including targets outside /etc/ssl.
static ErrorOr<void> add_certificate_store_paths(Vector<Sandbox::PathRule>& paths)
{
    // OpenSSL may load OPENSSL_CONF during initialization.
    if (auto* config_file = CONF_get1_default_config_file()) {
        auto result = Sandbox::add_path_if_exists(paths, StringView { config_file, strlen(config_file) }, Sandbox::PathRule::Access::ReadOnly);
        OPENSSL_free(config_file);
        TRY(result);
    }

    if (auto const* version_info = curl_version_info(CURLVERSION_NOW)) {
        if (version_info->cainfo)
            TRY(Sandbox::add_path_if_exists(paths, ByteString { version_info->cainfo }, Sandbox::PathRule::Access::ReadOnly));
        if (version_info->capath)
            TRY(add_certificate_directories(paths, StringView { version_info->capath, strlen(version_info->capath) }));
    }

    // libcurl is built with CURL_CA_FALLBACK, so OpenSSL's default store is consulted when no bundle is set.
    TRY(Sandbox::add_path_if_exists(paths, ByteString { X509_get_default_cert_file() }, Sandbox::PathRule::Access::ReadOnly));
    auto const* default_certificate_directory = X509_get_default_cert_dir();
    TRY(add_certificate_directories(paths, StringView { default_certificate_directory, strlen(default_certificate_directory) }));

    auto const* certificate_file_variable = X509_get_default_cert_file_env();
    if (auto value = Core::Environment::get(StringView { certificate_file_variable, strlen(certificate_file_variable) }); value.has_value() && !value->is_empty())
        TRY(Sandbox::add_path_if_exists(paths, *value, Sandbox::PathRule::Access::ReadOnly));

    auto const* certificate_directory_variable = X509_get_default_cert_dir_env();
    if (auto value = Core::Environment::get(StringView { certificate_directory_variable, strlen(certificate_directory_variable) }); value.has_value())
        TRY(add_certificate_directories(paths, *value));

    return {};
}

ErrorOr<void> apply_sandbox(StringView, Vector<ByteString> const& certificates, Optional<StringView> disk_cache_directory)
{
    TRY(Sandbox::install_no_new_privileges());
    TRY(Sandbox::configure_runtime());

    Vector<Sandbox::PathRule> paths;

    TRY(add_certificate_store_paths(paths));
    // The process that launched RequestServer resolves names with the system resolver for it, so none of the files that
    // the system resolver reads, like /etc/resolv.conf, are needed here.

    for (auto const& certificate : certificates)
        TRY(Sandbox::add_path_if_exists(paths, certificate, Sandbox::PathRule::Access::ReadOnly));

    if (g_resource_substitution_map) {
        TRY(g_resource_substitution_map->for_each_substitution([&](auto const& substitution) -> ErrorOr<void> {
            TRY(Sandbox::add_path_if_exists(paths, substitution.file_path, Sandbox::PathRule::Access::ReadOnly));
            return {};
        }));
    }

    if (disk_cache_directory.has_value())
        TRY(Sandbox::add_path_if_exists(paths, *disk_cache_directory, Sandbox::PathRule::Access::ReadWrite));

    TRY(Sandbox::restrict_filesystem(paths.span()));

    Sandbox::SeccompPolicy policy;
    policy.allow_readonly_file_opens();
    policy.allow_filesystem_metadata_queries();
    policy.allow_filesystem_writes();
    policy.allow_file_descriptor_operations();
    policy.allow_ipc();
    policy.allow_socket_pairs();
    policy.allow_network();
    policy.allow_common_runtime();
    TRY(policy.install());

    return {};
}

}
