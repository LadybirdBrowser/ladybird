/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/EncryptedMediaExtensions/EncryptedMediaExtensions.h>

namespace Web::EncryptedMediaExtensions {

class KeySystem : public RefCounted<KeySystem> {
public:
    virtual ~KeySystem() = default;

    virtual bool supports_init_data_type(String const& init_data_type) const = 0;
    virtual bool supports_encryption_scheme(String const& encryption_scheme) const = 0;
    virtual bool supports_robustness(String const& robustness) const = 0;
    virtual bool definitely_supports_playback(String const& container, String const& media_types, Optional<String> encryption_scheme, String const& robustness, Bindings::MediaKeySystemConfiguration const& accumulated_configuration, MediaKeyRestrictions const& restrictions) const = 0;

private:
};

class ClearKeySystem : public KeySystem {
public:
    ClearKeySystem() = default;
    virtual ~ClearKeySystem() override = default;

    // https://w3c.github.io/encrypted-media/#clear-key-behavior
    virtual bool supports_init_data_type(String const& init_data_type) const override
    {
        // Implementations SHOULD support the "keyids" type
        // TODO: Implementations MAY support any combination of registered Initialization Data Types
        // https://www.w3.org/TR/eme-initdata-registry/
        auto registered_init_data_types = Vector {
            "keyids"_string,
        };

        return registered_init_data_types.contains_slow(init_data_type);
    }

    // https://w3c.github.io/encrypted-media/#clear-key-capabilities
    virtual bool supports_encryption_scheme(String const& encryption_scheme) const override
    {
        // encryptionScheme: Implementations MUST support the "cenc" scheme, and MAY support other schemes.
        return encryption_scheme == "cenc"_string;
    }

    // https://w3c.github.io/encrypted-media/#clear-key-capabilities
    virtual bool supports_robustness(String const& robustness) const override
    {
        // robustness: Only the empty string is supported.
        return robustness.is_empty();
    }

    virtual bool definitely_supports_playback(String const& container, String const& media_types, Optional<String> encryption_scheme, String const& robustness, Bindings::MediaKeySystemConfiguration const& accumulated_configuration, MediaKeyRestrictions const& restrictions) const override
    {
        (void)container;
        (void)media_types;
        (void)encryption_scheme;
        (void)robustness;
        (void)accumulated_configuration;
        (void)restrictions;

        // FIXME: Do some checks here
        return true;
    }
};

}
