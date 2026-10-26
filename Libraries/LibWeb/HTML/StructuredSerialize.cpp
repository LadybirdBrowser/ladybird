/*
 * Copyright (c) 2022, Daniel Ehrenberg <dan@littledan.dev>
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2023, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/BitCast.h>
#include <AK/Checked.h>
#include <AK/Debug.h>
#include <AK/Endian.h>
#include <AK/LEB128.h>
#include <AK/StdLibExtras.h>
#include <AK/String.h>
#include <AK/UnicodeUtils.h>
#include <LibCrypto/ASN1/DER.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibGC/RootVector.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibIPC/File.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/CryptoKey.h>
#include <LibWeb/Bindings/DOMException.h>
#include <LibWeb/Bindings/DOMMatrix.h>
#include <LibWeb/Bindings/DOMMatrixReadOnly.h>
#include <LibWeb/Bindings/DOMPoint.h>
#include <LibWeb/Bindings/DOMPointReadOnly.h>
#include <LibWeb/Bindings/DOMQuad.h>
#include <LibWeb/Bindings/DOMRect.h>
#include <LibWeb/Bindings/DOMRectReadOnly.h>
#include <LibWeb/Bindings/File.h>
#include <LibWeb/Bindings/FileList.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PredefinedColorSpace.h>
#include <LibWeb/Bindings/QuotaExceededError.h>
#include <LibWeb/Bindings/Serializable.h>
#include <LibWeb/Bindings/Transferable.h>
#include <LibWeb/Crypto/CryptoKey.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/FileAPI/File.h>
#include <LibWeb/FileAPI/FileList.h>
#include <LibWeb/HTML/ImageData.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/QuotaExceededError.h>

namespace Web::HTML {

// Text storage uses Utf16String; converting back enforces String's strict-UTF-8 invariant.
static ErrorOr<String> utf16_to_utf8_text(Utf16String const& text)
{
    return text.utf16_view().to_utf8(AK::UnicodeUtils::AllowLonelySurrogates::No);
}

// Explicit return types keep these non-capturing lambdas function-pointer compatible.
static constexpr auto s_serializable_storage_registry = to_array<SerializableRegistryEntry>({
    { Bindings::InterfaceName::Blob, "Blob"sv },
    { Bindings::InterfaceName::CryptoKey, "CryptoKey"sv },
    { Bindings::InterfaceName::DOMException, "DOMException"sv },
    { Bindings::InterfaceName::DOMMatrix, "DOMMatrix"sv },
    { Bindings::InterfaceName::DOMMatrixReadOnly, "DOMMatrixReadOnly"sv },
    { Bindings::InterfaceName::DOMPoint, "DOMPoint"sv },
    { Bindings::InterfaceName::DOMPointReadOnly, "DOMPointReadOnly"sv },
    { Bindings::InterfaceName::DOMQuad, "DOMQuad"sv },
    { Bindings::InterfaceName::DOMRect, "DOMRect"sv },
    { Bindings::InterfaceName::DOMRectReadOnly, "DOMRectReadOnly"sv },
    { Bindings::InterfaceName::File, "File"sv },
    { Bindings::InterfaceName::FileList, "FileList"sv },
    { Bindings::InterfaceName::ImageBitmap, "ImageBitmap"sv },
    { Bindings::InterfaceName::ImageData, "ImageData"sv },
    { Bindings::InterfaceName::Module, "Module"sv },
    { Bindings::InterfaceName::QuotaExceededError, "QuotaExceededError"sv },
});

ReadonlySpan<SerializableRegistryEntry> serializable_storage_registry()
{
    return s_serializable_storage_registry.span();
}

static ErrorOr<StringView> serializable_interface_name_to_storage_identifier(Bindings::InterfaceName interface_name)
{
    for (auto const& entry : s_serializable_storage_registry) {
        if (entry.interface_name == interface_name)
            return entry.identifier;
    }
    return Error::from_string_literal("Invalid structured serialize interface name");
}

static ErrorOr<Bindings::InterfaceName> storage_identifier_to_serializable_interface_name(StringView identifier)
{
    for (auto const& entry : s_serializable_storage_registry) {
        if (entry.identifier == identifier)
            return entry.interface_name;
    }
    return Error::from_string_literal("Unknown structured serialize interface name");
}

static ErrorOr<StringView> key_type_to_storage_identifier(Bindings::KeyType key_type)
{
    switch (key_type) {
    case Bindings::KeyType::Public:
        return "public"sv;
    case Bindings::KeyType::Private:
        return "private"sv;
    case Bindings::KeyType::Secret:
        return "secret"sv;
    }
    return Error::from_string_literal("Invalid structured serialize key type");
}

static ErrorOr<Bindings::KeyType> storage_identifier_to_key_type(StringView identifier)
{
    if (identifier == "public"sv)
        return Bindings::KeyType::Public;
    if (identifier == "private"sv)
        return Bindings::KeyType::Private;
    if (identifier == "secret"sv)
        return Bindings::KeyType::Secret;
    return Error::from_string_literal("Unknown structured serialize key type");
}

static ErrorOr<StringView> key_usage_to_storage_identifier(Bindings::KeyUsage key_usage)
{
    switch (key_usage) {
    case Bindings::KeyUsage::Encrypt:
        return "encrypt"sv;
    case Bindings::KeyUsage::Decrypt:
        return "decrypt"sv;
    case Bindings::KeyUsage::Sign:
        return "sign"sv;
    case Bindings::KeyUsage::Verify:
        return "verify"sv;
    case Bindings::KeyUsage::DeriveKey:
        return "deriveKey"sv;
    case Bindings::KeyUsage::DeriveBits:
        return "deriveBits"sv;
    case Bindings::KeyUsage::WrapKey:
        return "wrapKey"sv;
    case Bindings::KeyUsage::UnwrapKey:
        return "unwrapKey"sv;
    case Bindings::KeyUsage::EncapsulateKey:
        return "encapsulateKey"sv;
    case Bindings::KeyUsage::EncapsulateBits:
        return "encapsulateBits"sv;
    case Bindings::KeyUsage::DecapsulateKey:
        return "decapsulateKey"sv;
    case Bindings::KeyUsage::DecapsulateBits:
        return "decapsulateBits"sv;
    }
    return Error::from_string_literal("Invalid structured serialize key usage");
}

static ErrorOr<Bindings::KeyUsage> storage_identifier_to_key_usage(StringView identifier)
{
    if (identifier == "encrypt"sv)
        return Bindings::KeyUsage::Encrypt;
    if (identifier == "decrypt"sv)
        return Bindings::KeyUsage::Decrypt;
    if (identifier == "sign"sv)
        return Bindings::KeyUsage::Sign;
    if (identifier == "verify"sv)
        return Bindings::KeyUsage::Verify;
    if (identifier == "deriveKey"sv)
        return Bindings::KeyUsage::DeriveKey;
    if (identifier == "deriveBits"sv)
        return Bindings::KeyUsage::DeriveBits;
    if (identifier == "wrapKey"sv)
        return Bindings::KeyUsage::WrapKey;
    if (identifier == "unwrapKey"sv)
        return Bindings::KeyUsage::UnwrapKey;
    if (identifier == "encapsulateKey"sv)
        return Bindings::KeyUsage::EncapsulateKey;
    if (identifier == "encapsulateBits"sv)
        return Bindings::KeyUsage::EncapsulateBits;
    if (identifier == "decapsulateKey"sv)
        return Bindings::KeyUsage::DecapsulateKey;
    if (identifier == "decapsulateBits"sv)
        return Bindings::KeyUsage::DecapsulateBits;
    return Error::from_string_literal("Unknown structured serialize key usage");
}

static ErrorOr<StringView> color_space_to_storage_identifier(Bindings::PredefinedColorSpace color_space)
{
    switch (color_space) {
    case Bindings::PredefinedColorSpace::Srgb:
        return "srgb"sv;
    case Bindings::PredefinedColorSpace::SrgbLinear:
        return "srgb-linear"sv;
    case Bindings::PredefinedColorSpace::DisplayP3:
        return "display-p3"sv;
    case Bindings::PredefinedColorSpace::DisplayP3Linear:
        return "display-p3-linear"sv;
    }
    return Error::from_string_literal("Invalid structured serialize color space");
}

static ErrorOr<Bindings::PredefinedColorSpace> storage_identifier_to_color_space(StringView identifier)
{
    if (identifier == "srgb"sv)
        return Bindings::PredefinedColorSpace::Srgb;
    if (identifier == "srgb-linear"sv)
        return Bindings::PredefinedColorSpace::SrgbLinear;
    if (identifier == "display-p3"sv)
        return Bindings::PredefinedColorSpace::DisplayP3;
    if (identifier == "display-p3-linear"sv)
        return Bindings::PredefinedColorSpace::DisplayP3Linear;
    return Error::from_string_literal("Unknown structured serialize color space");
}

static ErrorOr<StringView> bitmap_format_to_storage_identifier(Gfx::BitmapFormat format)
{
    switch (format) {
    case Gfx::BitmapFormat::Invalid:
        break;
    case Gfx::BitmapFormat::BGRx8888:
        return "BGRx8888"sv;
    case Gfx::BitmapFormat::BGRA8888:
        return "BGRA8888"sv;
    case Gfx::BitmapFormat::RGBx8888:
        return "RGBx8888"sv;
    case Gfx::BitmapFormat::RGBA8888:
        return "RGBA8888"sv;
    }
    return Error::from_string_literal("Invalid structured serialize bitmap format");
}

static ErrorOr<Gfx::BitmapFormat> storage_identifier_to_bitmap_format(StringView identifier)
{
    if (identifier == "BGRx8888"sv)
        return Gfx::BitmapFormat::BGRx8888;
    if (identifier == "BGRA8888"sv)
        return Gfx::BitmapFormat::BGRA8888;
    if (identifier == "RGBx8888"sv)
        return Gfx::BitmapFormat::RGBx8888;
    if (identifier == "RGBA8888"sv)
        return Gfx::BitmapFormat::RGBA8888;
    return Error::from_string_literal("Unknown structured serialize bitmap format");
}

static ErrorOr<StringView> alpha_type_to_storage_identifier(Gfx::AlphaType alpha_type)
{
    switch (alpha_type) {
    case Gfx::AlphaType::Premultiplied:
        return "premultiplied"sv;
    case Gfx::AlphaType::Unpremultiplied:
        return "unpremultiplied"sv;
    }
    return Error::from_string_literal("Invalid structured serialize alpha type");
}

static ErrorOr<Gfx::AlphaType> storage_identifier_to_alpha_type(StringView identifier)
{
    if (identifier == "premultiplied"sv)
        return Gfx::AlphaType::Premultiplied;
    if (identifier == "unpremultiplied"sv)
        return Gfx::AlphaType::Unpremultiplied;
    return Error::from_string_literal("Unknown structured serialize alpha type");
}

class StructuredSerializeDataEncoder {
public:
    AK_ALLOC_WITH_KMALLOC;

    virtual ~StructuredSerializeDataEncoder() = default;

    virtual SerializationType type() const = 0;

    virtual void encode(bool) = 0;
    virtual void encode(u8) = 0;
    virtual void encode(u16) = 0;
    virtual void encode(i32) = 0;
    virtual void encode(u32) = 0;
    virtual void encode(i64) = 0;
    virtual void encode(u64) = 0;
    virtual void encode(double) = 0;
    virtual void encode(Utf16String const&) = 0;
    virtual void encode(ByteBuffer const&) = 0;
    virtual void encode(ReadonlyBytes) = 0;

    virtual void append(IPCSerializationRecord&&) { VERIFY_NOT_REACHED(); }
    virtual IPCSerializationRecord take_ipc_record() { VERIFY_NOT_REACHED(); }
    virtual StorageSerializationRecord take_storage_record() { VERIFY_NOT_REACHED(); }
};

class StructuredSerializeDataDecoder {
public:
    AK_ALLOC_WITH_KMALLOC;

    virtual ~StructuredSerializeDataDecoder() = default;

    virtual SerializationType type() const = 0;

    // True once all input has been consumed. Only the storage path checks this (to reject trailing bytes).
    virtual bool is_at_end() const { return true; }

    virtual ErrorOr<void> decode(bool&) = 0;
    virtual ErrorOr<void> decode(u8&) = 0;
    virtual ErrorOr<void> decode(u16&) = 0;
    virtual ErrorOr<void> decode(i32&) = 0;
    virtual ErrorOr<void> decode(u32&) = 0;
    virtual ErrorOr<void> decode(i64&) = 0;
    virtual ErrorOr<void> decode(u64&) = 0;
    virtual ErrorOr<void> decode(double&) = 0;
    virtual ErrorOr<void> decode(Utf16String&) = 0;
    virtual ErrorOr<void> decode(ByteBuffer&) = 0;
};

class IPCStructuredSerializeDataEncoder final : public StructuredSerializeDataEncoder {
public:
    virtual SerializationType type() const override { return SerializationType::IPC; }

    virtual void encode(bool value) override { MUST(m_encoder.encode(value)); }
    virtual void encode(u8 value) override { MUST(m_encoder.encode(value)); }
    virtual void encode(u16 value) override { MUST(m_encoder.encode(value)); }
    virtual void encode(i32 value) override { MUST(m_encoder.encode(value)); }
    virtual void encode(u32 value) override { MUST(m_encoder.encode(value)); }
    virtual void encode(i64 value) override { MUST(m_encoder.encode(value)); }
    virtual void encode(u64 value) override { MUST(m_encoder.encode(value)); }
    virtual void encode(double value) override { MUST(m_encoder.encode(value)); }
    virtual void encode(Utf16String const& value) override { MUST(m_encoder.encode(value)); }
    virtual void encode(ByteBuffer const& value) override { MUST(m_encoder.encode(value)); }
    virtual void encode(ReadonlyBytes value) override { MUST(m_encoder.encode(value)); }

    virtual void append(IPCSerializationRecord&& record) override
    {
        m_encoder.append(move(record));
    }

    virtual IPCSerializationRecord take_ipc_record() override
    {
        return IPCSerializationRecord { m_encoder.take_buffer().take_data() };
    }

private:
    TransferDataEncoder m_encoder;
};

class IPCStructuredSerializeDataDecoder final : public StructuredSerializeDataDecoder {
public:
    explicit IPCStructuredSerializeDataDecoder(IPCSerializationRecord const& record)
        : m_decoder(record)
    {
    }

    explicit IPCStructuredSerializeDataDecoder(TransferDataEncoder&& data_holder)
        : m_decoder(move(data_holder))
    {
    }

    virtual SerializationType type() const override { return SerializationType::IPC; }

    virtual ErrorOr<void> decode(bool& value) override { return decode_from_ipc(value); }
    virtual ErrorOr<void> decode(u8& value) override { return decode_from_ipc(value); }
    virtual ErrorOr<void> decode(u16& value) override { return decode_from_ipc(value); }
    virtual ErrorOr<void> decode(i32& value) override { return decode_from_ipc(value); }
    virtual ErrorOr<void> decode(u32& value) override { return decode_from_ipc(value); }
    virtual ErrorOr<void> decode(i64& value) override { return decode_from_ipc(value); }
    virtual ErrorOr<void> decode(u64& value) override { return decode_from_ipc(value); }
    virtual ErrorOr<void> decode(double& value) override { return decode_from_ipc(value); }
    virtual ErrorOr<void> decode(Utf16String& value) override { return decode_from_ipc(value); }
    virtual ErrorOr<void> decode(ByteBuffer& value) override
    {
        value = TRY(m_decoder.decode<ByteBuffer>());
        return {};
    }

private:
    template<typename T>
    ErrorOr<void> decode_from_ipc(T& value)
    {
        value = TRY(m_decoder.decode<T>());
        return {};
    }

    TransferDataDecoder m_decoder;
};

class StorageStructuredSerializeDataEncoder final : public StructuredSerializeDataEncoder {
public:
    StorageStructuredSerializeDataEncoder()
    {
        append_storage_format_prologue(m_data);
    }

    virtual SerializationType type() const override { return SerializationType::Storage; }

    virtual void encode(bool value) override { encode(static_cast<u8>(value ? 1 : 0)); }

    virtual void encode(u8 value) override { m_data.append(value); }

    virtual void encode(u16 value) override { append_leb128(value); }

    virtual void encode(u32 value) override { append_leb128(value); }

    virtual void encode(i32 value) override { append_leb128(value); }

    virtual void encode(u64 value) override { append_leb128(value); }

    virtual void encode(i64 value) override { append_leb128(value); }

    virtual void encode(double value) override { append_little_endian(bit_cast<u64>(value)); }

    virtual void encode(Utf16String const& string) override
    {
        auto view = string.utf16_view();
        auto length = view.length_in_code_units();
        encode((static_cast<u64>(length) << 1) | (view.has_ascii_storage() ? 1u : 0u));
        if (view.has_ascii_storage()) {
            m_data.append(view.bytes());
        } else if constexpr (AK::HostIsLittleEndian) {
            m_data.append(reinterpret_cast<u8 const*>(view.utf16_span().data()), length * sizeof(char16_t));
        } else {
            for (char16_t code_unit : view.utf16_span())
                append_little_endian(static_cast<u16>(code_unit));
        }
    }

    virtual void encode(ByteBuffer const& buffer) override
    {
        encode(buffer.bytes());
    }

    virtual void encode(ReadonlyBytes bytes) override
    {
        encode(static_cast<u64>(bytes.size()));
        m_data.append(bytes.data(), bytes.size());
    }

    virtual StorageSerializationRecord take_storage_record() override
    {
        return StorageSerializationRecord { move(m_data) };
    }

private:
    template<typename T>
    void append_leb128(T value)
    {
        LEB128<T>::append_to(m_data, value);
    }

    template<typename T>
    void append_little_endian(T value)
    {
        AK::LittleEndian<T> const little_endian { value };
        m_data.append(reinterpret_cast<u8 const*>(&little_endian), sizeof(little_endian));
    }

    ByteBuffer m_data;
};

class StorageStructuredSerializeDataDecoder final : public StructuredSerializeDataDecoder {
public:
    explicit StorageStructuredSerializeDataDecoder(StorageSerializationRecord const& record)
        : m_stream(record.data.span())
    {
        auto magic = read_bytes(sizeof(storage_format_magic));
        if (magic.is_error()) {
            m_error = magic.release_error();
            return;
        }
        if (magic.value() != storage_format_magic) {
            m_error = Error::from_string_literal("Invalid structured serialize storage magic");
            return;
        }

        u64 version = 0;
        if (auto result = decode(version); result.is_error()) {
            m_error = result.release_error();
            return;
        }
        if (version != storage_format_version) {
            m_error = Error::from_string_literal("Unsupported structured serialize storage version");
            return;
        }

        u64 flags = 0;
        if (auto result = decode(flags); result.is_error()) {
            m_error = result.release_error();
            return;
        }
        if (flags != storage_format_flags)
            m_error = Error::from_string_literal("Unsupported structured serialize storage flags");
    }

    virtual SerializationType type() const override { return SerializationType::Storage; }
    virtual bool is_at_end() const override { return m_stream.is_eof(); }

    virtual ErrorOr<void> decode(bool& result) override
    {
        u8 value = 0;
        TRY(decode(value));
        if (value > 1)
            return Error::from_string_literal("Invalid structured serialize bool");
        result = value != 0;
        return {};
    }

    virtual ErrorOr<void> decode(u8& value) override
    {
        auto bytes = TRY(read_bytes(sizeof(u8)));
        value = bytes[0];
        return {};
    }

    virtual ErrorOr<void> decode(u16& value) override { return read_leb128(value); }

    virtual ErrorOr<void> decode(u32& value) override { return read_leb128(value); }

    virtual ErrorOr<void> decode(i32& value) override { return read_leb128(value); }

    virtual ErrorOr<void> decode(u64& value) override { return read_leb128(value); }

    virtual ErrorOr<void> decode(i64& value) override { return read_leb128(value); }

    virtual ErrorOr<void> decode(double& value) override
    {
        u64 raw_value = 0;
        TRY(read_little_endian(raw_value));
        value = bit_cast<double>(raw_value);
        return {};
    }

    virtual ErrorOr<void> decode(Utf16String& string) override
    {
        u64 header = 0;
        TRY(decode(header));
        bool is_ascii = (header & 1) != 0;
        size_t length = TRY(host_size_from_stable_size(header >> 1));
        if (length == 0) {
            string = {};
            return {};
        }
        // Validate the byte count before try_resize(), which multiplies length without an overflow check.
        Checked<size_t> byte_count = length;
        if (!is_ascii)
            byte_count *= sizeof(u16);
        if (byte_count.has_overflow() || byte_count.value() > m_stream.remaining())
            return Error::from_string_literal("Truncated structured serialize storage record");
        auto bytes = TRY(read_bytes(byte_count.value()));
        if (is_ascii) {
            if (!StringView { bytes }.is_ascii())
                return Error::from_string_literal("Invalid ASCII in structured serialize storage record");
            string = Utf16String::from_ascii_without_validation(bytes);
            return {};
        }
        // The stream offset can be odd, so the bytes cannot be viewed as char16_t in place.
        Vector<char16_t> code_units;
        TRY(code_units.try_resize(length));
        if constexpr (AK::HostIsLittleEndian) {
            memcpy(code_units.data(), bytes.data(), byte_count.value());
        } else {
            for (size_t i = 0; i < length; ++i)
                code_units[i] = static_cast<char16_t>(bytes[2 * i] | (bytes[2 * i + 1] << 8));
        }
        string = Utf16String::from_utf16({ code_units.data(), code_units.size() });
        return {};
    }

    virtual ErrorOr<void> decode(ByteBuffer& buffer) override
    {
        auto length = TRY(decode_size());
        auto bytes = TRY(read_bytes(length));
        buffer = TRY(ByteBuffer::copy(bytes));
        return {};
    }

private:
    ErrorOr<void> fail_if_error() const
    {
        if (m_error.has_value())
            return Error::copy(m_error.value());
        return {};
    }

    static ErrorOr<size_t> host_size_from_stable_size(u64 size)
    {
        if (!AK::is_within_range<size_t>(size))
            return Error::from_string_literal("Structured serialize storage size does not fit on this platform");
        return static_cast<size_t>(size);
    }

    ErrorOr<size_t> decode_size()
    {
        u64 size = 0;
        TRY(decode(size));
        return host_size_from_stable_size(size);
    }

    ErrorOr<ReadonlyBytes> read_bytes(size_t size)
    {
        TRY(fail_if_error());
        return m_stream.read_in_place<u8 const>(size);
    }

    template<typename T>
    ErrorOr<void> read_leb128(T& value)
    {
        TRY(fail_if_error());
        value = TRY(m_stream.read_value<LEB128<T>>());
        return {};
    }

    template<typename T>
    ErrorOr<void> read_little_endian(T& value)
    {
        TRY(fail_if_error());
        value = TRY(m_stream.read_value<AK::LittleEndian<T>>());
        return {};
    }

    FixedMemoryStream m_stream;
    Optional<AK::Error> m_error;
};

StructuredSerializeWriter StructuredSerializeWriter::create_ipc()
{
    return StructuredSerializeWriter { make<IPCStructuredSerializeDataEncoder>() };
}

StructuredSerializeWriter StructuredSerializeWriter::create_storage()
{
    return StructuredSerializeWriter { make<StorageStructuredSerializeDataEncoder>() };
}

StructuredSerializeWriter::StructuredSerializeWriter(NonnullOwnPtr<StructuredSerializeDataEncoder> encoder)
    : m_encoder(move(encoder))
{
}

StructuredSerializeWriter::~StructuredSerializeWriter() = default;

SerializationType StructuredSerializeWriter::type() const
{
    return m_encoder->type();
}

template<typename T>
static void encode_value(StructuredSerializeDataEncoder& encoder, T const& value)
requires requires { encoder.encode(value); }
{
    encoder.encode(value);
}

static void encode_value(StructuredSerializeDataEncoder& encoder, int value)
{
    VERIFY(AK::is_within_range<i32>(value));
    encoder.encode(static_cast<i32>(value));
}

static void encode_value(StructuredSerializeDataEncoder& encoder, long value)
{
    encoder.encode(static_cast<i64>(value));
}

static void encode_value(StructuredSerializeDataEncoder& encoder, long long value)
{
    encoder.encode(static_cast<i64>(value));
}

static void encode_value(StructuredSerializeDataEncoder& encoder, unsigned long value)
{
    encoder.encode(static_cast<u64>(value));
}

static void encode_value(StructuredSerializeDataEncoder& encoder, unsigned long long value)
{
    encoder.encode(static_cast<u64>(value));
}

static void encode_value(StructuredSerializeDataEncoder& encoder, Bindings::InterfaceName value)
{
    if (encoder.type() == SerializationType::IPC)
        encoder.encode(to_underlying(value));
    else
        encoder.encode(Utf16String::from_utf8(MUST(serializable_interface_name_to_storage_identifier(value))));
}

[[maybe_unused]] static void encode_value(StructuredSerializeDataEncoder& encoder, Bindings::KeyType value)
{
    auto identifier = MUST(key_type_to_storage_identifier(value));
    if (encoder.type() == SerializationType::IPC)
        encoder.encode(to_underlying(value));
    else
        encoder.encode(Utf16String::from_utf8(identifier));
}

[[maybe_unused]] static void encode_value(StructuredSerializeDataEncoder& encoder, Bindings::KeyUsage value)
{
    auto identifier = MUST(key_usage_to_storage_identifier(value));
    if (encoder.type() == SerializationType::IPC)
        encoder.encode(to_underlying(value));
    else
        encoder.encode(Utf16String::from_utf8(identifier));
}

static void encode_value(StructuredSerializeDataEncoder& encoder, Crypto::KeyType value)
{
    encoder.encode(to_underlying(value));
}

[[maybe_unused]] static void encode_value(StructuredSerializeDataEncoder& encoder, Bindings::PredefinedColorSpace value)
{
    auto identifier = MUST(color_space_to_storage_identifier(value));
    if (encoder.type() == SerializationType::IPC)
        encoder.encode(to_underlying(value));
    else
        encoder.encode(Utf16String::from_utf8(identifier));
}

[[maybe_unused]] static void encode_value(StructuredSerializeDataEncoder& encoder, Gfx::BitmapFormat value)
{
    auto identifier = MUST(bitmap_format_to_storage_identifier(value));
    if (encoder.type() == SerializationType::IPC)
        encoder.encode(to_underlying(value));
    else
        encoder.encode(Utf16String::from_utf8(identifier));
}

[[maybe_unused]] static void encode_value(StructuredSerializeDataEncoder& encoder, Gfx::AlphaType value)
{
    auto identifier = MUST(alpha_type_to_storage_identifier(value));
    if (encoder.type() == SerializationType::IPC)
        encoder.encode(to_underlying(value));
    else
        encoder.encode(Utf16String::from_utf8(identifier));
}

template<typename T, size_t inline_capacity>
static void encode_value(StructuredSerializeDataEncoder& encoder, Vector<T, inline_capacity> const& value)
{
    encoder.encode(static_cast<u64>(value.size()));
    for (auto const& element : value)
        encode_value(encoder, element);
}

template<typename T>
static void encode_value(StructuredSerializeDataEncoder& encoder, Optional<T> const& value)
{
    encoder.encode(value.has_value());
    if (value.has_value())
        encode_value(encoder, value.value());
}

template<typename T>
void StructuredSerializeWriter::encode(T const& value)
{
    encode_value(*m_encoder, value);
}

// The same-process SharedArrayBuffer aliases of an IPCSerializationRecord. Only this file creates side tables, so the
// reader can rely on a record's side table being one of these.
class SharedArrayBufferSideTable final : public SameProcessSerializationSideTable {
public:
    explicit SharedArrayBufferSideTable(Vector<GC::Root<JS::ArrayBuffer>> array_buffers)
        : m_array_buffers(move(array_buffers))
    {
    }

    Vector<GC::Root<JS::ArrayBuffer>> const& array_buffers() const { return m_array_buffers; }

private:
    Vector<GC::Root<JS::ArrayBuffer>> m_array_buffers;
};

void StructuredSerializeWriter::append(IPCSerializationRecord&& record)
{
    // TransferDataEncoder only appends record.data. Silently accepting a side table here would
    // produce a record whose SharedArrayBuffer references cannot be decoded.
    VERIFY(!record.same_process_side_table);
    m_encoder->append(move(record));
}

u32 StructuredSerializeWriter::add_shared_array_buffer(JS::ArrayBuffer& array_buffer)
{
    VERIFY(m_shared_array_buffers.size() < NumericLimits<u32>::max());
    m_shared_array_buffers.append(GC::make_root(array_buffer));
    return static_cast<u32>(m_shared_array_buffers.size() - 1);
}

IPCSerializationRecord StructuredSerializeWriter::take_ipc_record()
{
    auto record = m_encoder->take_ipc_record();
    if (!m_shared_array_buffers.is_empty())
        record.same_process_side_table = adopt_ref(*new SharedArrayBufferSideTable(move(m_shared_array_buffers)));
    return record;
}

StorageSerializationRecord StructuredSerializeWriter::take_storage_record()
{
    return m_encoder->take_storage_record();
}

ReadonlySpan<GC::Root<JS::ArrayBuffer>> same_process_shared_array_buffers(IPCSerializationRecord const& record)
{
    if (!record.same_process_side_table)
        return {};
    return static_cast<SharedArrayBufferSideTable const&>(*record.same_process_side_table).array_buffers();
}

StructuredSerializeReader::StructuredSerializeReader(IPCSerializationRecord const& record)
    : m_decoder(make<IPCStructuredSerializeDataDecoder>(record))
    , m_shared_array_buffers(same_process_shared_array_buffers(record))
{
}

StructuredSerializeReader::StructuredSerializeReader(StorageSerializationRecord const& record)
    : m_decoder(make<StorageStructuredSerializeDataDecoder>(record))
{
}

StructuredSerializeReader::StructuredSerializeReader(TransferDataEncoder&& data_holder)
    : m_decoder(make<IPCStructuredSerializeDataDecoder>(move(data_holder)))
{
}

StructuredSerializeReader::~StructuredSerializeReader() = default;

SerializationType StructuredSerializeReader::type() const
{
    return m_decoder->type();
}

bool StructuredSerializeReader::is_at_end() const
{
    return m_decoder->is_at_end();
}

template<typename T>
static ErrorOr<void> decode_value(StructuredSerializeDataDecoder& decoder, T& value)
requires requires { decoder.decode(value); }
{
    return decoder.decode(value);
}

static ErrorOr<void> decode_value(StructuredSerializeDataDecoder& decoder, int& value)
{
    i32 decoded = 0;
    TRY(decoder.decode(decoded));
    if (!AK::is_within_range<int>(decoded))
        return Error::from_string_literal("Structured serialize i32 does not fit in int");
    value = static_cast<int>(decoded);
    return {};
}

template<typename NativeType, typename WireType>
static ErrorOr<void> decode_native_integer(StructuredSerializeDataDecoder& decoder, NativeType& value)
{
    WireType decoded = 0;
    TRY(decoder.decode(decoded));
    if (!AK::is_within_range<NativeType>(decoded))
        return Error::from_string_literal("Structured serialize integer does not fit in native integer type");
    value = static_cast<NativeType>(decoded);
    return {};
}

static ErrorOr<void> decode_value(StructuredSerializeDataDecoder& decoder, long& value)
{
    return decode_native_integer<long, i64>(decoder, value);
}

static ErrorOr<void> decode_value(StructuredSerializeDataDecoder& decoder, long long& value)
{
    return decode_native_integer<long long, i64>(decoder, value);
}

static ErrorOr<void> decode_value(StructuredSerializeDataDecoder& decoder, unsigned long& value)
{
    return decode_native_integer<unsigned long, u64>(decoder, value);
}

static ErrorOr<void> decode_value(StructuredSerializeDataDecoder& decoder, unsigned long long& value)
{
    return decode_native_integer<unsigned long long, u64>(decoder, value);
}

static ErrorOr<String> decode_utf8_text(StructuredSerializeDataDecoder& decoder)
{
    Utf16String text;
    TRY(decoder.decode(text));
    return utf16_to_utf8_text(text);
}

static ErrorOr<void> decode_value(StructuredSerializeDataDecoder& decoder, Bindings::InterfaceName& value)
{
    if (decoder.type() == SerializationType::IPC) {
        u16 decoded = 0;
        TRY(decoder.decode(decoded));
        auto interface_name = static_cast<Bindings::InterfaceName>(decoded);
        TRY(serializable_interface_name_to_storage_identifier(interface_name));
        value = interface_name;
        return {};
    }

    auto identifier = TRY(decode_utf8_text(decoder));
    value = TRY(storage_identifier_to_serializable_interface_name(identifier.bytes_as_string_view()));
    return {};
}

[[maybe_unused]] static ErrorOr<void> decode_value(StructuredSerializeDataDecoder& decoder, Bindings::KeyType& value)
{
    if (decoder.type() == SerializationType::IPC) {
        u8 decoded = 0;
        TRY(decoder.decode(decoded));
        auto key_type = static_cast<Bindings::KeyType>(decoded);
        TRY(key_type_to_storage_identifier(key_type));
        value = key_type;
        return {};
    }

    auto identifier = TRY(decode_utf8_text(decoder));
    value = TRY(storage_identifier_to_key_type(identifier.bytes_as_string_view()));
    return {};
}

[[maybe_unused]] static ErrorOr<void> decode_value(StructuredSerializeDataDecoder& decoder, Bindings::KeyUsage& value)
{
    if (decoder.type() == SerializationType::IPC) {
        u8 decoded = 0;
        TRY(decoder.decode(decoded));
        auto key_usage = static_cast<Bindings::KeyUsage>(decoded);
        TRY(key_usage_to_storage_identifier(key_usage));
        value = key_usage;
        return {};
    }

    auto identifier = TRY(decode_utf8_text(decoder));
    value = TRY(storage_identifier_to_key_usage(identifier.bytes_as_string_view()));
    return {};
}

static ErrorOr<void> decode_value(StructuredSerializeDataDecoder& decoder, Crypto::KeyType& value)
{
    int decoded = 0;
    TRY(decoder.decode(decoded));
    auto key_type = static_cast<Crypto::KeyType>(decoded);
    switch (key_type) {
    case Crypto::KeyType::Public:
    case Crypto::KeyType::Private:
    case Crypto::KeyType::Secret:
        value = key_type;
        return {};
    }
    return Error::from_string_literal("Unknown structured serialize key type");
}

[[maybe_unused]] static ErrorOr<void> decode_value(StructuredSerializeDataDecoder& decoder, Bindings::PredefinedColorSpace& value)
{
    if (decoder.type() == SerializationType::IPC) {
        u8 decoded = 0;
        TRY(decoder.decode(decoded));
        auto color_space = static_cast<Bindings::PredefinedColorSpace>(decoded);
        TRY(color_space_to_storage_identifier(color_space));
        value = color_space;
        return {};
    }

    auto identifier = TRY(decode_utf8_text(decoder));
    value = TRY(storage_identifier_to_color_space(identifier.bytes_as_string_view()));
    return {};
}

[[maybe_unused]] static ErrorOr<void> decode_value(StructuredSerializeDataDecoder& decoder, Gfx::BitmapFormat& value)
{
    if (decoder.type() == SerializationType::IPC) {
        u32 decoded = 0;
        TRY(decoder.decode(decoded));
        auto format = static_cast<Gfx::BitmapFormat>(decoded);
        TRY(bitmap_format_to_storage_identifier(format));
        value = format;
        return {};
    }

    auto identifier = TRY(decode_utf8_text(decoder));
    value = TRY(storage_identifier_to_bitmap_format(identifier.bytes_as_string_view()));
    return {};
}

[[maybe_unused]] static ErrorOr<void> decode_value(StructuredSerializeDataDecoder& decoder, Gfx::AlphaType& value)
{
    if (decoder.type() == SerializationType::IPC) {
        u32 decoded = 0;
        TRY(decoder.decode(decoded));
        auto alpha_type = static_cast<Gfx::AlphaType>(decoded);
        TRY(alpha_type_to_storage_identifier(alpha_type));
        value = alpha_type;
        return {};
    }

    auto identifier = TRY(decode_utf8_text(decoder));
    value = TRY(storage_identifier_to_alpha_type(identifier.bytes_as_string_view()));
    return {};
}

template<typename T, size_t inline_capacity>
static ErrorOr<void> decode_value(StructuredSerializeDataDecoder& decoder, Vector<T, inline_capacity>& value)
{
    u64 size = 0;
    TRY(decoder.decode(size));
    value.clear_with_capacity();
    for (u64 i = 0; i < size; ++i) {
        T element {};
        TRY(decode_value(decoder, element));
        TRY(value.try_append(move(element)));
    }
    return {};
}

template<typename T>
static ErrorOr<void> decode_value(StructuredSerializeDataDecoder& decoder, Optional<T>& value)
{
    bool has_value = false;
    TRY(decoder.decode(has_value));
    if (!has_value) {
        value = OptionalNone {};
        return {};
    }

    T decoded {};
    TRY(decode_value(decoder, decoded));
    value = move(decoded);
    return {};
}

template<typename T>
ErrorOr<T> StructuredSerializeReader::decode()
{
    T value {};
    TRY(decode_value(*m_decoder, value));
    return value;
}

// These templates are declared in the public header but their implementation
// uses the private encode_value/decode_value overload set above. Explicitly
// instantiate the concrete wire types used by LibWeb so users of the header do
// not end up with unresolved template symbols at link time.
template WEB_API void StructuredSerializeWriter::encode(ByteBuffer const&);
template WEB_API void StructuredSerializeWriter::encode(Optional<::Crypto::ASN1::ObjectIdentifier> const&);
template WEB_API void StructuredSerializeWriter::encode(Optional<double> const&);
template WEB_API void StructuredSerializeWriter::encode(Span<u8 const> const&);
template WEB_API void StructuredSerializeWriter::encode(Bindings::InterfaceName const&);
template WEB_API void StructuredSerializeWriter::encode(Bindings::KeyType const&);
template WEB_API void StructuredSerializeWriter::encode(Vector<Bindings::KeyUsage> const&);
template WEB_API void StructuredSerializeWriter::encode(Bindings::KeyUsage const&);
template WEB_API void StructuredSerializeWriter::encode(Gfx::AlphaType const&);
template WEB_API void StructuredSerializeWriter::encode(Gfx::BitmapFormat const&);
template WEB_API void StructuredSerializeWriter::encode(Bindings::PredefinedColorSpace const&);
template WEB_API void StructuredSerializeWriter::encode(Crypto::KeyType const&);
template WEB_API void StructuredSerializeWriter::encode(bool const&);
template WEB_API void StructuredSerializeWriter::encode(double const&);
template WEB_API void StructuredSerializeWriter::encode(int const&);
template WEB_API void StructuredSerializeWriter::encode(Utf16String const&);
template WEB_API void StructuredSerializeWriter::encode(Optional<Utf16String> const&);
template WEB_API void StructuredSerializeWriter::encode(Optional<bool> const&);
template WEB_API void StructuredSerializeWriter::encode(Optional<u64> const&);
template WEB_API void StructuredSerializeWriter::encode(long const&);
template WEB_API void StructuredSerializeWriter::encode(long long const&);
template WEB_API void StructuredSerializeWriter::encode(unsigned long const&);
template WEB_API void StructuredSerializeWriter::encode(unsigned long long const&);
template WEB_API void StructuredSerializeWriter::encode(u8 const&);
template WEB_API void StructuredSerializeWriter::encode(u32 const&);
template WEB_API void StructuredSerializeWriter::encode(u16 const&);

template WEB_API ErrorOr<ByteBuffer> StructuredSerializeReader::decode();
template WEB_API ErrorOr<int> StructuredSerializeReader::decode();
template WEB_API ErrorOr<Optional<::Crypto::ASN1::ObjectIdentifier>> StructuredSerializeReader::decode();
template WEB_API ErrorOr<Optional<double>> StructuredSerializeReader::decode();
template WEB_API ErrorOr<Bindings::InterfaceName> StructuredSerializeReader::decode();
template WEB_API ErrorOr<Bindings::KeyType> StructuredSerializeReader::decode();
template WEB_API ErrorOr<Vector<Bindings::KeyUsage>> StructuredSerializeReader::decode();
template WEB_API ErrorOr<Bindings::KeyUsage> StructuredSerializeReader::decode();
template WEB_API ErrorOr<Gfx::AlphaType> StructuredSerializeReader::decode();
template WEB_API ErrorOr<Gfx::BitmapFormat> StructuredSerializeReader::decode();
template WEB_API ErrorOr<Bindings::PredefinedColorSpace> StructuredSerializeReader::decode();
template WEB_API ErrorOr<Crypto::KeyType> StructuredSerializeReader::decode();
template WEB_API ErrorOr<bool> StructuredSerializeReader::decode();
template WEB_API ErrorOr<double> StructuredSerializeReader::decode();
template WEB_API ErrorOr<Utf16String> StructuredSerializeReader::decode();
template WEB_API ErrorOr<Optional<Utf16String>> StructuredSerializeReader::decode();
template WEB_API ErrorOr<Optional<bool>> StructuredSerializeReader::decode();
template WEB_API ErrorOr<Optional<u64>> StructuredSerializeReader::decode();
template WEB_API ErrorOr<long> StructuredSerializeReader::decode();
template WEB_API ErrorOr<long long> StructuredSerializeReader::decode();
template WEB_API ErrorOr<unsigned long> StructuredSerializeReader::decode();
template WEB_API ErrorOr<unsigned long long> StructuredSerializeReader::decode();
template WEB_API ErrorOr<u8> StructuredSerializeReader::decode();
template WEB_API ErrorOr<u32> StructuredSerializeReader::decode();
template WEB_API ErrorOr<u16> StructuredSerializeReader::decode();

WebIDL::Exception data_clone_error_from_serialization_error(JS::Realm& realm, AK::Error const& error)
{
    dbgln_if(STRUCTURED_SERIALIZE_DEBUG, "Rejecting structured serialized data: {}", error);
    return WebIDL::Exception { WebIDL::DataCloneError::create(realm, "Unable to process structured serialized data"_utf16) };
}

// The algorithms of structured serialization, which Libraries/LibWeb/Bindings/Rust/src/html/structured_serialize runs.
extern "C" {
JSCompletion web_rust_structured_serialize_internal(JSVM*, StructuredSerializeWriter*, JSValue, bool for_storage, SerializationMemory*, u8 allow_shared_array_buffers);
JSCompletion web_rust_structured_deserialize_internal(JSVM*, void* target_realm, StructuredSerializeReader*, DeserializationMemory*, bool check_fully_consumed);
JSCompletion web_rust_structured_serialize_with_transfer(JSVM*, void* realm, StructuredSerializeWriter*, SerializedTransferRecord*, JSValue, JSObject* const* transfer_list, size_t count);
JSCompletion web_rust_structured_deserialize_with_transfer(JSVM*, void* target_realm, SerializedTransferRecord*, Vector<GC::Root<JS::Object>>* transferred_values);
JSCompletion web_rust_structured_deserialize_with_transfer_internal(JSVM*, void* target_realm, TransferDataDecoder*);
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializewithtransfer
WebIDL::ExceptionOr<SerializedTransferRecord> structured_serialize_with_transfer(JS::Realm& realm, JS::Value value, ReadonlySpan<GC::Ref<JS::Object>> transfer_list)
{
    auto serialized_writer = StructuredSerializeWriter::create_ipc();
    serialized_writer.enable_shared_buffers();
    SerializedTransferRecord record;

    Vector<JSObject*> transfer_list_abi;
    transfer_list_abi.ensure_capacity(transfer_list.size());
    for (auto const& transferable : transfer_list)
        transfer_list_abi.unchecked_append(JS::HostABI::object_to_abi(transferable.ptr()));

    TRY(JS::HostABI::completion_from_abi<JS::Value>(web_rust_structured_serialize_with_transfer(JS::HostABI::vm_to_abi(realm.vm()), &realm, &serialized_writer, &record, JS::HostABI::value_to_abi(value), transfer_list_abi.data(), transfer_list_abi.size())));

    record.serialized = serialized_writer.take_ipc_record();
    record.shared_buffers = serialized_writer.take_shared_buffers();
    return record;
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structureddeserializewithtransfer
WebIDL::ExceptionOr<DeserializedTransferRecord> structured_deserialize_with_transfer(SerializedTransferRecord& serialize_with_transfer_result, JS::Realm& target_realm)
{
    Vector<GC::Root<JS::Object>> transferred_values;
    auto deserialized = TRY(JS::HostABI::completion_from_abi<JS::Value>(web_rust_structured_deserialize_with_transfer(JS::HostABI::vm_to_abi(target_realm.vm()), &target_realm, &serialize_with_transfer_result, &transferred_values)));
    return DeserializedTransferRecord { .deserialized = deserialized, .transferred_values = move(transferred_values) };
}

// AD-HOC: This non-standard overload is meant to extract just one transferrable value from a serialized transfer record.
//         It's primarily useful for an object's transfer receiving steps to deserialize a nested value.
WebIDL::ExceptionOr<JS::Value> structured_deserialize_with_transfer_internal(TransferDataDecoder& decoder, JS::Realm& target_realm)
{
    return TRY(JS::HostABI::completion_from_abi<JS::Value>(web_rust_structured_deserialize_with_transfer_internal(JS::HostABI::vm_to_abi(target_realm.vm()), &target_realm, &decoder)));
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserialize
WebIDL::ExceptionOr<IPCSerializationRecord> structured_serialize(JS::VM& vm, JS::Value value)
{
    return structured_serialize(vm, value, AllowSharedArrayBuffers::CrossOriginIsolatedOnly);
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserialize
WebIDL::ExceptionOr<IPCSerializationRecord> structured_serialize(JS::VM& vm, JS::Value value, AllowSharedArrayBuffers allow_shared_array_buffers)
{
    // 1. Return ? StructuredSerializeInternal(value, false).
    SerializationMemory memory = {};
    auto serialized = StructuredSerializeWriter::create_ipc();
    TRY(structured_serialize_internal(vm, serialized, value, false, memory, allow_shared_array_buffers));
    return serialized.take_ipc_record();
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserialize
// AD-HOC: For a record that reaches agents in other processes. The record itself is bytes-only, so a shared-memory-
//         backed SharedArrayBuffer is handed over beside it, in shared_buffers, by file descriptor — and every
//         receiver references the same [[ArrayBufferData]], rather than rebuilding the buffer from a byte copy.
WebIDL::ExceptionOr<IPCSerializationRecord> structured_serialize(JS::VM& vm, JS::Value value, Vector<Core::AnonymousBuffer>& shared_buffers)
{
    // 1. Return ? StructuredSerializeInternal(value, false).
    SerializationMemory memory = {};
    auto serialized = StructuredSerializeWriter::create_ipc();
    serialized.enable_shared_buffers();
    TRY(structured_serialize_internal(vm, serialized, value, false, memory));
    shared_buffers = serialized.take_shared_buffers();
    return serialized.take_ipc_record();
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializeforstorage
WebIDL::ExceptionOr<StorageSerializationRecord> structured_serialize_for_storage(JS::VM& vm, JS::Value value)
{
    // 1. Return ? StructuredSerializeInternal(value, true).
    SerializationMemory memory = {};
    auto serialized = StructuredSerializeWriter::create_storage();
    TRY(structured_serialize_internal(vm, serialized, value, true, memory));
    return serialized.take_storage_record();
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializeforstorage
// NB: StructuredSerializeInternal returns undefined and null as primitives at step 4, which needs no VM.
StorageSerializationRecord structured_serialize_undefined_or_null_for_storage(JS::Value value)
{
    VERIFY(value.is_undefined() || value.is_null());
    return value.is_undefined() ? storage_serialization_record_for_undefined() : storage_serialization_record_for_null();
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializeinternal
WebIDL::ExceptionOr<void> structured_serialize_internal(JS::VM& vm, StructuredSerializeWriter& serialized, JS::Value value, bool for_storage, SerializationMemory& memory, AllowSharedArrayBuffers allow_shared_array_buffers)
{
    TRY(JS::HostABI::completion_from_abi<JS::Value>(web_rust_structured_serialize_internal(JS::HostABI::vm_to_abi(vm), &serialized, JS::HostABI::value_to_abi(value), for_storage, &memory, to_underlying(allow_shared_array_buffers))));
    return {};
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structureddeserialize
WebIDL::ExceptionOr<JS::Value> structured_deserialize(JS::VM& vm, IPCSerializationRecord const& serialized, JS::Realm& target_realm, Optional<DeserializationMemory> memory, Vector<Core::AnonymousBuffer> const* shared_buffers)
{
    TemporaryExecutionContext execution_context { target_realm };

    if (!memory.has_value())
        memory = DeserializationMemory {};

    StructuredSerializeReader decoder { serialized };
    if (shared_buffers)
        decoder.set_shared_buffers(*shared_buffers);
    return structured_deserialize_internal(vm, decoder, target_realm, *memory);
}

WebIDL::ExceptionOr<JS::Value> structured_deserialize(JS::VM& vm, StorageSerializationRecord const& serialized, JS::Realm& target_realm, Optional<DeserializationMemory> memory)
{
    TemporaryExecutionContext execution_context { target_realm };

    if (!memory.has_value())
        memory = DeserializationMemory {};

    StructuredSerializeReader decoder { serialized };
    return structured_deserialize_internal(vm, decoder, target_realm, *memory, CheckFullyConsumed::Yes);
}

WebIDL::ExceptionOr<JS::Value> structured_deserialize_internal(JS::VM& vm, StructuredSerializeReader& serialized, JS::Realm& target_realm, DeserializationMemory& memory, CheckFullyConsumed check_fully_consumed)
{
    return TRY(JS::HostABI::completion_from_abi<JS::Value>(web_rust_structured_deserialize_internal(JS::HostABI::vm_to_abi(vm), &target_realm, &serialized, &memory, check_fully_consumed == CheckFullyConsumed::Yes)));
}

TransferDataDecoder::TransferDataDecoder(IPCSerializationRecord const& record)
    : m_stream(record.data.span())
    , m_decoder(m_stream, m_attachments)
{
}

TransferDataDecoder::TransferDataDecoder(TransferDataEncoder&& data_holder)
    : m_buffer(data_holder.take_buffer())
    , m_stream(m_buffer.data().span())
    , m_decoder(m_stream, m_attachments)
{
    for (auto& attachment : m_buffer.take_attachments())
        m_attachments.enqueue(move(attachment));
}

WebIDL::ExceptionOr<ByteBuffer> TransferDataDecoder::decode_buffer()
{
    auto buffer = m_decoder.decode<ByteBuffer>();

    if (buffer.is_error()) {
        VERIFY(buffer.error().code() == ENOMEM);
        return WebIDL::DataCloneError::create("Unable to allocate memory for transferred buffer"_utf16);
    }
    return buffer.release_value();
}

void encode_unsigned_big_integer(StructuredSerializeWriter& writer, ::Crypto::UnsignedBigInteger const& value)
{
    auto buffer = MUST(ByteBuffer::create_zeroed(value.byte_length()));
    auto written = value.export_data(buffer.bytes());
    VERIFY(written.size() == buffer.size());
    writer.encode(buffer);
}

WebIDL::ExceptionOr<::Crypto::UnsignedBigInteger> decode_unsigned_big_integer(StructuredSerializeReader& reader, JS::Realm& realm)
{
    auto buffer = TRY(decode_or_throw_data_clone_error<ByteBuffer>(realm, reader));
    return ::Crypto::UnsignedBigInteger::import_data(buffer);
}

WebIDL::ExceptionOr<::Crypto::UnsignedBigInteger> TransferDataDecoder::decode_unsigned_big_integer()
{
    auto buffer = TRY(decode_buffer());
    return ::Crypto::UnsignedBigInteger::import_data(buffer);
}

void encode_signed_big_integer(StructuredSerializeWriter& writer, ::Crypto::SignedBigInteger const& value)
{
    auto buffer = MUST(ByteBuffer::create_zeroed(value.byte_length()));
    auto written = value.export_data(buffer.bytes());
    VERIFY(written.size() == buffer.size());
    writer.encode(buffer);
}

WebIDL::ExceptionOr<::Crypto::SignedBigInteger> decode_signed_big_integer(StructuredSerializeReader& reader, JS::Realm& realm)
{
    auto buffer = TRY(decode_or_throw_data_clone_error<ByteBuffer>(realm, reader));
    if (buffer.is_empty())
        return data_clone_error_from_serialization_error(realm, AK::Error::from_string_literal("Invalid BigInt"));
    return ::Crypto::SignedBigInteger::import_data(buffer);
}

}
