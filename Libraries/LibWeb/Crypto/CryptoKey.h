/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCrypto/KeyHandle.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/CryptoKey.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::Bindings {

enum class KeyType : u8;

}

namespace Web::Crypto {

using ::Crypto::OKPPrivateKey;
using ::Crypto::OKPPublicKey;

using KeyUsage = Bindings::KeyUsage;

enum class KeyType {
    Public,
    Private,
    Secret,
};

class WEB_API CryptoKey final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(CryptoKey, crypto_key)

public:
    using InternalKeyData = ::Crypto::KeyHandle;

    struct KeyAlgorithmData {
        Utf16String const& name() const { return m_name; }

        Utf16String m_name;
    };

    struct RsaKeyAlgorithmData {
        Utf16String const& name() const { return m_name; }
        u32 modulus_length() const { return m_modulus_length; }
        ByteBuffer const& public_exponent() const { return m_public_exponent; }

        Utf16String m_name;
        u32 m_modulus_length { 0 };
        ByteBuffer m_public_exponent;
    };

    struct RsaHashedKeyAlgorithmData {
        Utf16String const& name() const { return m_name; }
        u32 modulus_length() const { return m_modulus_length; }
        ByteBuffer const& public_exponent() const { return m_public_exponent; }
        Utf16String const& hash() const { return m_hash; }

        Utf16String m_name;
        u32 m_modulus_length { 0 };
        ByteBuffer m_public_exponent;
        Utf16String m_hash;
    };

    struct EcKeyAlgorithmData {
        Utf16String const& name() const { return m_name; }
        Utf16String const& named_curve() const { return m_named_curve; }

        Utf16String m_name;
        Utf16String m_named_curve;
    };

    struct AesKeyAlgorithmData {
        Utf16String const& name() const { return m_name; }
        u16 length() const { return m_length; }

        Utf16String m_name;
        u16 m_length { 0 };
    };

    struct HmacKeyAlgorithmData {
        Utf16String const& name() const { return m_name; }
        Utf16String const& hash() const { return m_hash; }
        WebIDL::UnsignedLong length() const { return m_length; }

        Utf16String m_name;
        Utf16String m_hash;
        WebIDL::UnsignedLong m_length { 0 };
    };

    struct KmacKeyAlgorithmData {
        Utf16String const& name() const { return m_name; }
        WebIDL::UnsignedLong length() const { return m_length; }

        Utf16String m_name;
        WebIDL::UnsignedLong m_length { 0 };
    };

    using InternalAlgorithmData = Variant<KeyAlgorithmData, RsaKeyAlgorithmData, RsaHashedKeyAlgorithmData, EcKeyAlgorithmData, AesKeyAlgorithmData, HmacKeyAlgorithmData, KmacKeyAlgorithmData>;

    [[nodiscard]] static GC::Ref<CryptoKey> create(InternalKeyData);

    bool extractable() const;
    KeyType type() const;

    Vector<KeyUsage> internal_usages() const;
    EcKeyAlgorithmData ec_algorithm() const;

    void set_extractable(bool);
    void set_type(KeyType);
    void set_algorithm(InternalAlgorithmData);
    void set_usages(Vector<KeyUsage>);

    InternalKeyData const& handle() const;
    Utf16String algorithm_name() const;
};

}

namespace Web::Bindings {

WEB_API GC::Ref<JS::HostObject> wrap(WrapperWorld&, JS::Realm&, GC::Ref<Crypto::CryptoKey>);

}
