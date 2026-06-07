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
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibIPC/File.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/BigIntObject.h>
#include <LibJS/Runtime/BooleanObject.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/NumberObject.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibJS/Runtime/Set.h>
#include <LibJS/Runtime/StringObject.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>
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
#include <LibWeb/Bindings/ImageBitmap.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MessagePort.h>
#include <LibWeb/Bindings/QuotaExceededError.h>
#include <LibWeb/Bindings/ReadableStream.h>
#include <LibWeb/Bindings/Serializable.h>
#include <LibWeb/Bindings/Transferable.h>
#include <LibWeb/Bindings/TransformStream.h>
#include <LibWeb/Bindings/WritableStream.h>
#include <LibWeb/Crypto/CryptoKey.h>
#include <LibWeb/FileAPI/Blob.h>
#include <LibWeb/FileAPI/File.h>
#include <LibWeb/FileAPI/FileList.h>
#include <LibWeb/Geometry/DOMMatrix.h>
#include <LibWeb/Geometry/DOMMatrixReadOnly.h>
#include <LibWeb/Geometry/DOMPoint.h>
#include <LibWeb/Geometry/DOMPointReadOnly.h>
#include <LibWeb/Geometry/DOMQuad.h>
#include <LibWeb/Geometry/DOMRect.h>
#include <LibWeb/Geometry/DOMRectReadOnly.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/ImageData.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/Streams/ReadableStream.h>
#include <LibWeb/Streams/TransformStream.h>
#include <LibWeb/Streams/WritableStream.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/QuotaExceededError.h>

namespace Web::HTML {

enum class ErrorType : u8 {
    Error,
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    ClassName,
    JS_ENUMERATE_NATIVE_ERRORS
#undef __JS_ENUMERATE
};

static ErrorOr<StringView> error_type_to_name(ErrorType type)
{
    switch (type) {
    case ErrorType::Error:
        return "Error"sv;
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    case ErrorType::ClassName:                                                           \
        return #ClassName##sv;
        JS_ENUMERATE_NATIVE_ERRORS
#undef __JS_ENUMERATE
    }
    return Error::from_string_literal("Invalid structured serialize error type");
}

static ErrorType error_name_to_type(Utf16View name)
{
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    if (name == #ClassName##sv)                                                          \
        return ErrorType::ClassName;
    JS_ENUMERATE_NATIVE_ERRORS
#undef __JS_ENUMERATE
    return ErrorType::Error;
}

// Text storage uses Utf16String; converting back enforces String's strict-UTF-8 invariant.
static ErrorOr<String> utf16_to_utf8_text(Utf16String const& text)
{
    return text.utf16_view().to_utf8(AK::UnicodeUtils::AllowLonelySurrogates::No);
}

// Explicit return types keep these non-capturing lambdas function-pointer compatible.
static constexpr Array<SerializableRegistryEntry, 15> s_serializable_storage_registry { {
    { Bindings::InterfaceName::Blob, "Blob"sv, [](JS::Realm& realm) -> GC::Ref<Bindings::PlatformObject> { return FileAPI::Blob::create(realm); } },
    { Bindings::InterfaceName::File, "File"sv, [](JS::Realm& realm) -> GC::Ref<Bindings::PlatformObject> { return FileAPI::File::create(realm); } },
    { Bindings::InterfaceName::FileList, "FileList"sv, [](JS::Realm& realm) -> GC::Ref<Bindings::PlatformObject> { return FileAPI::FileList::create(realm); } },
    { Bindings::InterfaceName::DOMException, "DOMException"sv, [](JS::Realm& realm) -> GC::Ref<Bindings::PlatformObject> { return WebIDL::DOMException::create(realm); } },
    { Bindings::InterfaceName::DOMMatrixReadOnly, "DOMMatrixReadOnly"sv, [](JS::Realm& realm) -> GC::Ref<Bindings::PlatformObject> { return Geometry::DOMMatrixReadOnly::create(realm); } },
    { Bindings::InterfaceName::DOMMatrix, "DOMMatrix"sv, [](JS::Realm& realm) -> GC::Ref<Bindings::PlatformObject> { return Geometry::DOMMatrix::create(realm); } },
    { Bindings::InterfaceName::DOMPointReadOnly, "DOMPointReadOnly"sv, [](JS::Realm& realm) -> GC::Ref<Bindings::PlatformObject> { return Geometry::DOMPointReadOnly::create(realm); } },
    { Bindings::InterfaceName::DOMPoint, "DOMPoint"sv, [](JS::Realm& realm) -> GC::Ref<Bindings::PlatformObject> { return Geometry::DOMPoint::create(realm); } },
    { Bindings::InterfaceName::DOMRectReadOnly, "DOMRectReadOnly"sv, [](JS::Realm& realm) -> GC::Ref<Bindings::PlatformObject> { return Geometry::DOMRectReadOnly::create(realm); } },
    { Bindings::InterfaceName::DOMRect, "DOMRect"sv, [](JS::Realm& realm) -> GC::Ref<Bindings::PlatformObject> { return Geometry::DOMRect::create(realm); } },
    { Bindings::InterfaceName::CryptoKey, "CryptoKey"sv, [](JS::Realm& realm) -> GC::Ref<Bindings::PlatformObject> { return Crypto::CryptoKey::create(realm); } },
    { Bindings::InterfaceName::DOMQuad, "DOMQuad"sv, [](JS::Realm& realm) -> GC::Ref<Bindings::PlatformObject> { return Geometry::DOMQuad::create(realm); } },
    { Bindings::InterfaceName::ImageData, "ImageData"sv, [](JS::Realm& realm) -> GC::Ref<Bindings::PlatformObject> { return ImageData::create(realm); } },
    { Bindings::InterfaceName::ImageBitmap, "ImageBitmap"sv, [](JS::Realm& realm) -> GC::Ref<Bindings::PlatformObject> { return ImageBitmap::create(realm); } },
    { Bindings::InterfaceName::QuotaExceededError, "QuotaExceededError"sv, [](JS::Realm& realm) -> GC::Ref<Bindings::PlatformObject> { return WebIDL::QuotaExceededError::create(realm); } },
} };

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
    case Bindings::KeyUsage::Derivekey:
        return "deriveKey"sv;
    case Bindings::KeyUsage::Derivebits:
        return "deriveBits"sv;
    case Bindings::KeyUsage::Wrapkey:
        return "wrapKey"sv;
    case Bindings::KeyUsage::Unwrapkey:
        return "unwrapKey"sv;
    case Bindings::KeyUsage::Encapsulatekey:
        return "encapsulateKey"sv;
    case Bindings::KeyUsage::Encapsulatebits:
        return "encapsulateBits"sv;
    case Bindings::KeyUsage::Decapsulatekey:
        return "decapsulateKey"sv;
    case Bindings::KeyUsage::Decapsulatebits:
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
        return Bindings::KeyUsage::Derivekey;
    if (identifier == "deriveBits"sv)
        return Bindings::KeyUsage::Derivebits;
    if (identifier == "wrapKey"sv)
        return Bindings::KeyUsage::Wrapkey;
    if (identifier == "unwrapKey"sv)
        return Bindings::KeyUsage::Unwrapkey;
    if (identifier == "encapsulateKey"sv)
        return Bindings::KeyUsage::Encapsulatekey;
    if (identifier == "encapsulateBits"sv)
        return Bindings::KeyUsage::Encapsulatebits;
    if (identifier == "decapsulateKey"sv)
        return Bindings::KeyUsage::Decapsulatekey;
    if (identifier == "decapsulateBits"sv)
        return Bindings::KeyUsage::Decapsulatebits;
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

static constexpr Array<u8, 4> storage_format_magic = { 'L', 'B', 'S', 'C' };

// Flags are for optional envelope features (e.g. compression); changes to the value encoding bump the version instead.
static constexpr u64 storage_format_flags = 0;

class StructuredSerializeDataEncoder {
public:
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
        m_data.append(storage_format_magic.data(), storage_format_magic.size());
        encode(storage_format_version);
        encode(storage_format_flags);
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

static void encode_value(StructuredSerializeDataEncoder& encoder, ValueTag value)
{
    encoder.encode(to_underlying(value));
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

static void encode_value(StructuredSerializeDataEncoder& encoder, ErrorType value)
{
    auto identifier = MUST(error_type_to_name(value));
    if (encoder.type() == SerializationType::IPC)
        encoder.encode(to_underlying(value));
    else
        encoder.encode(Utf16String::from_utf8(identifier));
}

static void encode_value(StructuredSerializeDataEncoder& encoder, Bindings::InterfaceName value)
{
    if (encoder.type() == SerializationType::IPC)
        encoder.encode(to_underlying(value));
    else
        encoder.encode(Utf16String::from_utf8(MUST(serializable_interface_name_to_storage_identifier(value))));
}

static void encode_value(StructuredSerializeDataEncoder& encoder, Bindings::KeyType value)
{
    auto identifier = MUST(key_type_to_storage_identifier(value));
    if (encoder.type() == SerializationType::IPC)
        encoder.encode(to_underlying(value));
    else
        encoder.encode(Utf16String::from_utf8(identifier));
}

static void encode_value(StructuredSerializeDataEncoder& encoder, Bindings::KeyUsage value)
{
    auto identifier = MUST(key_usage_to_storage_identifier(value));
    if (encoder.type() == SerializationType::IPC)
        encoder.encode(to_underlying(value));
    else
        encoder.encode(Utf16String::from_utf8(identifier));
}

static void encode_value(StructuredSerializeDataEncoder& encoder, Bindings::PredefinedColorSpace value)
{
    auto identifier = MUST(color_space_to_storage_identifier(value));
    if (encoder.type() == SerializationType::IPC)
        encoder.encode(to_underlying(value));
    else
        encoder.encode(Utf16String::from_utf8(identifier));
}

static void encode_value(StructuredSerializeDataEncoder& encoder, Gfx::BitmapFormat value)
{
    auto identifier = MUST(bitmap_format_to_storage_identifier(value));
    if (encoder.type() == SerializationType::IPC)
        encoder.encode(to_underlying(value));
    else
        encoder.encode(Utf16String::from_utf8(identifier));
}

static void encode_value(StructuredSerializeDataEncoder& encoder, Gfx::AlphaType value)
{
    auto identifier = MUST(alpha_type_to_storage_identifier(value));
    if (encoder.type() == SerializationType::IPC)
        encoder.encode(to_underlying(value));
    else
        encoder.encode(Utf16String::from_utf8(identifier));
}

template<typename T>
static void encode_value(StructuredSerializeDataEncoder& encoder, Vector<T> const& value)
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

void StructuredSerializeWriter::append(IPCSerializationRecord&& record)
{
    m_encoder->append(move(record));
}

IPCSerializationRecord StructuredSerializeWriter::take_ipc_record()
{
    return m_encoder->take_ipc_record();
}

StorageSerializationRecord StructuredSerializeWriter::take_storage_record()
{
    return m_encoder->take_storage_record();
}

StructuredSerializeReader::StructuredSerializeReader(IPCSerializationRecord const& record)
    : m_decoder(make<IPCStructuredSerializeDataDecoder>(record))
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

static ErrorOr<String> decode_utf8_text(StructuredSerializeDataDecoder& decoder)
{
    Utf16String text;
    TRY(decoder.decode(text));
    return utf16_to_utf8_text(text);
}

static ErrorOr<void> decode_value(StructuredSerializeDataDecoder& decoder, ErrorType& value)
{
    if (decoder.type() == SerializationType::IPC) {
        u8 decoded = 0;
        TRY(decoder.decode(decoded));
        auto type = static_cast<ErrorType>(decoded);
        TRY(error_type_to_name(type));
        value = type;
        return {};
    }

    Utf16String name;
    TRY(decoder.decode(name));
    value = error_name_to_type(name.utf16_view());
    return {};
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

static ErrorOr<void> decode_value(StructuredSerializeDataDecoder& decoder, Bindings::KeyType& value)
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

static ErrorOr<void> decode_value(StructuredSerializeDataDecoder& decoder, Bindings::KeyUsage& value)
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

static ErrorOr<void> decode_value(StructuredSerializeDataDecoder& decoder, Bindings::PredefinedColorSpace& value)
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

static ErrorOr<void> decode_value(StructuredSerializeDataDecoder& decoder, Gfx::BitmapFormat& value)
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

static ErrorOr<void> decode_value(StructuredSerializeDataDecoder& decoder, Gfx::AlphaType& value)
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

template<typename T>
static ErrorOr<void> decode_value(StructuredSerializeDataDecoder& decoder, Vector<T>& value)
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

WebIDL::Exception data_clone_error_from_serialization_error(JS::Realm& realm, AK::Error const& error)
{
    dbgln_if(STRUCTURED_SERIALIZE_DEBUG, "Rejecting structured serialized data: {}", error);
    return WebIDL::Exception { WebIDL::DataCloneError::create(realm, "Unable to process structured serialized data"_utf16) };
}

WebIDL::ExceptionOr<String> decode_utf8_text_or_throw_data_clone_error(JS::Realm& realm, StructuredSerializeReader& reader)
{
    auto text = reader.decode<Utf16String>();
    if (text.is_error())
        return data_clone_error_from_serialization_error(realm, text.release_error());
    auto utf8 = utf16_to_utf8_text(text.value());
    if (utf8.is_error())
        return data_clone_error_from_serialization_error(realm, utf8.release_error());
    return utf8.release_value();
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializeinternal
static WebIDL::ExceptionOr<void> serialize_array_buffer(JS::VM& vm, StructuredSerializeWriter& data_holder, JS::ArrayBuffer const& array_buffer, bool for_storage)
{
    // 13. Otherwise, if value has an [[ArrayBufferData]] internal slot, then:

    // 1. If IsSharedArrayBuffer(value) is true, then:
    if (array_buffer.is_shared_array_buffer()) {
        // 1. If the current settings object's cross-origin isolated capability is false, then throw a "DataCloneError" DOMException.
        // NOTE: This check is only needed when serializing (and not when deserializing) as the cross-origin isolated capability cannot change
        //       over time and a SharedArrayBuffer cannot leave an agent cluster.
        if (current_settings_object().cross_origin_isolated_capability() == CanUseCrossOriginIsolatedAPIs::No)
            return WebIDL::DataCloneError::create(*vm.current_realm(), "Cannot serialize SharedArrayBuffer when cross-origin isolated"_utf16);

        // 2. If forStorage is true, then throw a "DataCloneError" DOMException.
        if (for_storage)
            return WebIDL::DataCloneError::create(*vm.current_realm(), "Cannot serialize SharedArrayBuffer for storage"_utf16);

        if (!array_buffer.is_fixed_length()) {
            // 3. If value has an [[ArrayBufferMaxByteLength]] internal slot, then set serialized to { [[Type]]: "GrowableSharedArrayBuffer",
            //           [[ArrayBufferData]]: value.[[ArrayBufferData]], [[ArrayBufferByteLengthData]]: value.[[ArrayBufferByteLengthData]],
            //           [[ArrayBufferMaxByteLength]]: value.[[ArrayBufferMaxByteLength]],
            //           FIXME: [[AgentCluster]]: the surrounding agent's agent cluster }.
            data_holder.encode(ValueTag::GrowableSharedArrayBuffer);
            data_holder.encode(MUST(array_buffer.copy_to_byte_buffer()));
            data_holder.encode(static_cast<u64>(array_buffer.max_byte_length()));
        } else {
            // 4. Otherwise, set serialized to { [[Type]]: "SharedArrayBuffer", [[ArrayBufferData]]: value.[[ArrayBufferData]],
            //           [[ArrayBufferByteLength]]: value.[[ArrayBufferByteLength]],
            //           FIXME: [[AgentCluster]]: the surrounding agent's agent cluster }.
            data_holder.encode(ValueTag::SharedArrayBuffer);
            data_holder.encode(MUST(array_buffer.copy_to_byte_buffer()));
        }
    }
    // 2. Otherwise:
    else {
        // 1. If IsDetachedBuffer(value) is true, then throw a "DataCloneError" DOMException.
        if (array_buffer.is_detached())
            return WebIDL::DataCloneError::create(*vm.current_realm(), "Cannot serialize detached ArrayBuffer"_utf16);

        // 2. Let size be value.[[ArrayBufferByteLength]].
        auto size = array_buffer.byte_length();

        // 3. Let dataCopy be ? CreateByteDataBlock(size).
        //    NOTE: This can throw a RangeError exception upon allocation failure.
        auto data_copy = TRY(JS::create_byte_data_block(vm, size));

        // 4. Perform CopyDataBlockBytes(dataCopy, 0, value.[[ArrayBufferData]], 0, size).
        array_buffer.copy_data_to(data_copy, 0, 0, size);

        // 5. If value has an [[ArrayBufferMaxByteLength]] internal slot, then set serialized to { [[Type]]: "ResizableArrayBuffer",
        //    [[ArrayBufferData]]: dataCopy, [[ArrayBufferByteLength]]: size, [[ArrayBufferMaxByteLength]]: value.[[ArrayBufferMaxByteLength]] }.
        if (!array_buffer.is_fixed_length()) {
            data_holder.encode(ValueTag::ResizeableArrayBuffer);
            data_holder.encode(MUST(data_copy.copy_to_byte_buffer()));
            data_holder.encode(static_cast<u64>(array_buffer.max_byte_length()));
        }
        // 6. Otherwise, set serialized to { [[Type]]: "ArrayBuffer", [[ArrayBufferData]]: dataCopy, [[ArrayBufferByteLength]]: size }.
        else {
            data_holder.encode(ValueTag::ArrayBuffer);
            data_holder.encode(MUST(data_copy.copy_to_byte_buffer()));
        }
    }
    return {};
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializeinternal
template<OneOf<JS::TypedArrayBase, JS::DataView> ViewType>
static WebIDL::ExceptionOr<void> serialize_viewed_array_buffer(JS::VM& vm, StructuredSerializeWriter& data_holder, ViewType const& view, bool for_storage, SerializationMemory& memory)
{
    // 14. Otherwise, if value has a [[ViewedArrayBuffer]] internal slot, then:

    // 1. If IsArrayBufferViewOutOfBounds(value) is true, then throw a "DataCloneError" DOMException.
    if constexpr (IsSame<ViewType, JS::DataView>) {
        auto view_record = JS::make_data_view_with_buffer_witness_record(view, JS::ArrayBuffer::Order::SeqCst);
        if (JS::is_view_out_of_bounds(view_record))
            return WebIDL::DataCloneError::create(*vm.current_realm(), Utf16String::formatted(JS::ErrorType::BufferOutOfBounds.format(), "DataView"sv));
    } else {
        auto typed_array_record = JS::make_typed_array_with_buffer_witness_record(view, JS::ArrayBuffer::Order::SeqCst);
        if (JS::is_typed_array_out_of_bounds(typed_array_record))
            return WebIDL::DataCloneError::create(*vm.current_realm(), Utf16String::formatted(JS::ErrorType::BufferOutOfBounds.format(), "TypedArray"sv));
    }

    // 2. Let buffer be the value of value's [[ViewedArrayBuffer]] internal slot.
    JS::Value buffer = view.viewed_array_buffer();

    auto serialize_byte_length = [&](JS::ByteLength byte_length) {
        VERIFY(!byte_length.is_detached());

        data_holder.encode(byte_length.is_auto());
        if (!byte_length.is_auto())
            data_holder.encode(byte_length.length());
    };

    // 5. If value has a [[DataView]] internal slot, then set serialized to { [[Type]]: "ArrayBufferView", [[Constructor]]: "DataView",
    //    [[ArrayBufferSerialized]]: bufferSerialized, [[ByteLength]]: value.[[ByteLength]], [[ByteOffset]]: value.[[ByteOffset]] }.
    if constexpr (IsSame<ViewType, JS::DataView>) {
        data_holder.encode(ValueTag::ArrayBufferView);
        // 3. Let bufferSerialized be ? StructuredSerializeInternal(buffer, forStorage, memory).
        TRY(structured_serialize_internal(vm, data_holder, buffer, for_storage, memory)); // [[ArrayBufferSerialized]]
        data_holder.encode("DataView"_utf16);                                             // [[Constructor]]
        serialize_byte_length(view.byte_length());
        data_holder.encode(view.byte_offset());
    }
    // 6. Otherwise:
    else {
        // 1. Assert: value has a [[TypedArrayName]] internal slot.
        //    NOTE: Handled by constexpr check and template constraints
        // 2. Set serialized to { [[Type]]: "ArrayBufferView", [[Constructor]]: value.[[TypedArrayName]],
        //    [[ArrayBufferSerialized]]: bufferSerialized, [[ByteLength]]: value.[[ByteLength]],
        //    [[ByteOffset]]: value.[[ByteOffset]], [[ArrayLength]]: value.[[ArrayLength]] }.
        data_holder.encode(ValueTag::ArrayBufferView);
        TRY(structured_serialize_internal(vm, data_holder, buffer, for_storage, memory)); // [[ArrayBufferSerialized]]
        data_holder.encode(view.element_name().to_utf16_string());                        // [[Constructor]]
        serialize_byte_length(view.byte_length());
        data_holder.encode(view.byte_offset());
        serialize_byte_length(view.array_length());
    }

    return {};
}

// Serializing and deserializing are each two passes:
// 1. Fill up the memory with all the values, but without translating references
// 2. Translate all the references into the appropriate form
class Serializer {
public:
    Serializer(JS::VM& vm, StructuredSerializeWriter& serialized, SerializationMemory& memory, bool for_storage)
        : m_vm(vm)
        , m_serialized(serialized)
        , m_memory(memory)
        , m_for_storage(for_storage)
    {
    }

    // https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializeinternal
    WebIDL::ExceptionOr<void> serialize(JS::Value value)
    {
        if (m_vm.did_reach_stack_space_limit())
            return m_vm.throw_completion<JS::InternalError>(JS::ErrorType::CallStackSizeExceeded);

        auto& serialized = m_serialized;

        // 2. If memory[value] exists, then return memory[value].
        if (m_memory.contains(value)) {
            encode(ValueTag::ObjectReference);
            encode(m_memory.get(value).value());
            return {};
        }

        // 3. Let deep be false.
        auto deep = false;

        // 4. If value is undefined, null, a Boolean, a Number, a BigInt, or a String, then return { [[Type]]: "primitive", [[Value]]: value }.
        bool return_primitive_type = true;

        if (value.is_undefined()) {
            encode(ValueTag::UndefinedPrimitive);
        } else if (value.is_null()) {
            encode(ValueTag::NullPrimitive);
        } else if (value.is_boolean()) {
            encode(ValueTag::BooleanPrimitive);
            encode(value.as_bool());
        } else if (value.is_int32()) {
            encode(ValueTag::Int32Primitive);
            encode(value.as_i32());
        } else if (value.is_number()) {
            encode(ValueTag::NumberPrimitive);
            encode(value.as_double());
        } else if (value.is_bigint()) {
            encode(ValueTag::BigIntPrimitive);
            encode_signed_big_integer(m_serialized, value.as_bigint().big_integer());
        } else if (value.is_string()) {
            encode(ValueTag::StringPrimitive);
            encode(value.as_string().utf16_string());
        } else {
            return_primitive_type = false;
        }

        if (return_primitive_type)
            return {};

        // 5. If value is a Symbol, then throw a "DataCloneError" DOMException.
        if (value.is_symbol())
            return WebIDL::DataCloneError::create(*m_vm.current_realm(), "Cannot serialize Symbol"_utf16);

        // 6. Let serialized be an uninitialized value.
        // NOTE: We created the serialized value above.
        if (auto object = value.as_if<JS::Object>()) {
            // 7. If value has a [[BooleanData]] internal slot, then set serialized to { [[Type]]: "Boolean", [[BooleanData]]: value.[[BooleanData]] }.
            if (auto const* boolean_object = as_if<JS::BooleanObject>(*object)) {
                encode(ValueTag::BooleanObject);
                encode(boolean_object->boolean());
            }

            // 8. Otherwise, if value has a [[NumberData]] internal slot, then set serialized to { [[Type]]: "Number", [[NumberData]]: value.[[NumberData]] }.
            else if (auto const* number_object = as_if<JS::NumberObject>(*object)) {
                encode(ValueTag::NumberObject);
                encode(number_object->number());
            }

            // 9. Otherwise, if value has a [[BigIntData]] internal slot, then set serialized to { [[Type]]: "BigInt", [[BigIntData]]: value.[[BigIntData]] }.
            else if (auto const* big_int_object = as_if<JS::BigIntObject>(*object)) {
                encode(ValueTag::BigIntObject);
                encode_signed_big_integer(m_serialized, big_int_object->bigint().big_integer());
            }

            // 10. Otherwise, if value has a [[StringData]] internal slot, then set serialized to { [[Type]]: "String", [[StringData]]: value.[[StringData]] }.
            else if (auto const* string_object = as_if<JS::StringObject>(*object)) {
                encode(ValueTag::StringObject);
                encode(string_object->primitive_string().utf16_string());
            }

            // 11. Otherwise, if value has a [[DateValue]] internal slot, then set serialized to { [[Type]]: "Date", [[DateValue]]: value.[[DateValue]] }.
            else if (auto const* date = as_if<JS::Date>(*object)) {
                encode(ValueTag::DateObject);
                encode(date->date_value());
            }

            // 12. Otherwise, if value has a [[RegExpMatcher]] internal slot, then set serialized to
            //     { [[Type]]: "RegExp", [[RegExpMatcher]]: value.[[RegExpMatcher]], [[OriginalSource]]: value.[[OriginalSource]],
            //       [[OriginalFlags]]: value.[[OriginalFlags]] }.
            else if (auto const* reg_exp_object = as_if<JS::RegExpObject>(*object)) {
                // NOTE: ECMAScriptRegex is perfectly happy to be reconstructed with just the source+flags.
                //       In the future, we could optimize the work being done on the deserialize step by serializing
                //       more of the internal state (the [[RegExpMatcher]] internal slot).
                encode(ValueTag::RegExpObject);
                encode(reg_exp_object->pattern());
                encode(reg_exp_object->flags());
            }

            // 13. Otherwise, if value has an [[ArrayBufferData]] internal slot, then:
            else if (auto const* array_buffer = as_if<JS::ArrayBuffer>(*object)) {
                TRY(serialize_array_buffer(m_vm, serialized, *array_buffer, m_for_storage));
            }

            // 14. Otherwise, if value has a [[ViewedArrayBuffer]] internal slot, then:
            else if (auto const* typed_array_base = as_if<JS::TypedArrayBase>(*object)) {
                TRY(serialize_viewed_array_buffer(m_vm, serialized, *typed_array_base, m_for_storage, m_memory));
            } else if (auto const* data_view = as_if<JS::DataView>(*object)) {
                TRY(serialize_viewed_array_buffer(m_vm, serialized, *data_view, m_for_storage, m_memory));
            }

            // 15. Otherwise, if value has a [[MapData]] internal slot, then:
            else if (is<JS::Map>(*object)) {
                // 1. Set serialized to { [[Type]]: "Map", [[MapData]]: a new empty List }.
                encode(ValueTag::MapObject);

                // 2. Set deep to true.
                deep = true;
            }

            // 16. Otherwise, if value has a [[SetData]] internal slot, then:
            else if (is<JS::Set>(*object)) {
                // 1. Set serialized to { [[Type]]: "Set", [[SetData]]: a new empty List }.
                encode(ValueTag::SetObject);

                // 2. Set deep to true.
                deep = true;
            }

            // 17. Otherwise, if value has an [[ErrorData]] internal slot and value is not a platform object, then:
            else if (is<JS::Error>(*object) && !is<Bindings::PlatformObject>(*object)) {
                // 1. Let name be ? Get(value, "name").
                auto name = TRY(object->get(m_vm.names.name));

                // 2. If name is not one of "Error", "EvalError", "RangeError", "ReferenceError", "SyntaxError", "TypeError", or "URIError", then set name to "Error".
                auto type = ErrorType::Error;
                if (name.is_string())
                    type = error_name_to_type(name.as_string().utf16_string_view());

                // 3. Let valueMessageDesc be ? value.[[GetOwnProperty]]("message").
                auto value_message_descriptor = TRY(object->internal_get_own_property(m_vm.names.message));

                // 4. Let message be undefined if IsDataDescriptor(valueMessageDesc) is false, and ? ToString(valueMessageDesc.[[Value]]) otherwise.
                Optional<Utf16String> message;
                if (value_message_descriptor.has_value() && value_message_descriptor->is_data_descriptor())
                    message = TRY(value_message_descriptor->value->to_utf16_string(m_vm));

                // FIXME: Spec bug - https://github.com/whatwg/html/issues/11321
                // MISSING STEP: Let valueCauseDesc be ? value.[[GetOwnProperty]]("cause").
                auto value_cause_descriptor = TRY(object->internal_get_own_property(m_vm.names.cause));

                // MISSING STEP: Let cause be undefined if IsDataDescriptor(valueCauseDesc) is false, and ? ToString(valueCauseDesc.[[Value]]) otherwise.
                Optional<Utf16String> cause;
                if (value_cause_descriptor.has_value() && value_cause_descriptor->is_data_descriptor())
                    cause = TRY(value_cause_descriptor->value->to_utf16_string(m_vm));

                // 5. Set serialized to { [[Type]]: "Error", [[Name]]: name, [[Message]]: message }.
                // FIXME: 6. User agents should attach a serialized representation of any interesting accompanying data which are not yet specified, notably the stack property, to serialized.
                encode(ValueTag::ErrorObject);
                encode(type);
                encode(message);
                encode(cause);
            }

            // 18. Otherwise, if value is an Array exotic object, then:
            else if (is<JS::Array>(*object)) {
                // 1. Let valueLenDescriptor be ? OrdinaryGetOwnProperty(value, "length").
                // 2. Let valueLen be valueLenDescriptor.[[Value]].
                // NON-STANDARD: Array objects in LibJS do not have a real length property, so it must be accessed the usual way
                u64 length = MUST(JS::length_of_array_like(m_vm, *object));

                // 3. Set serialized to { [[Type]]: "Array", [[Length]]: valueLen, [[Properties]]: a new empty List }.
                encode(ValueTag::ArrayObject);
                encode(length);

                // 4. Set deep to true.
                deep = true;
            }

            // 19. Otherwise, if value is a platform object that is a serializable object:
            else if (auto const* serializable = as_if<Bindings::Serializable>(*object)) {
                // FIXME: 1. If value has a [[Detached]] internal slot whose value is true, then throw a "DataCloneError" DOMException.

                // 2. Let typeString be the identifier of the primary interface of value.
                // 3. Set serialized to { [[Type]]: typeString }.
                encode(ValueTag::SerializableObject);
                encode(as<Bindings::PlatformObject>(serializable)->interface_name());

                if (serialized.type() == SerializationType::Storage)
                    serialized.encode(serializable->serialization_version());

                // 4. Set deep to true
                deep = true;
            }

            // 20. Otherwise, if value is a platform object, then throw a "DataCloneError" DOMException.
            else if (is<Bindings::PlatformObject>(*object)) {
                return throw_completion(WebIDL::DataCloneError::create(*m_vm.current_realm(), "Cannot serialize platform objects"_utf16));
            }

            // 21. Otherwise, if IsCallable(value) is true, then throw a "DataCloneError" DOMException.
            else if (value.is_function()) {
                return throw_completion(WebIDL::DataCloneError::create(*m_vm.current_realm(), "Cannot serialize functions"_utf16));
            }

            // FIXME: 22. Otherwise, if value has any internal slot other than [[Prototype]] or [[Extensible]], then throw a "DataCloneError" DOMException.

            // FIXME: 23. Otherwise, if value is an exotic object and value is not the %Object.prototype% intrinsic object associated with any realm, then throw a "DataCloneError" DOMException.

            // 24. Otherwise:
            else {
                // 1. Set serialized to { [[Type]]: "Object", [[Properties]]: a new empty List }.
                encode(ValueTag::Object);

                // 2. Set deep to true.
                deep = true;
            }
        }

        // 25. Set memory[value] to serialized.
        // Nested sub-serialization above may already have claimed earlier ids.
        m_memory.set(make_root(value), m_memory.size());

        // 26. If deep is true, then:
        if (deep) {
            auto& object = value.as_object();

            // 1. If value has a [[MapData]] internal slot, then:
            if (auto const* map = as_if<JS::Map>(object)) {
                // 1. Let copiedList be a new empty List.
                Vector<JS::Value> copied_list;
                copied_list.ensure_capacity(map->map_size() * 2);

                // 2. For each Record { [[Key]], [[Value]] } entry of value.[[MapData]]:
                for (auto entry : *map) {
                    // 1. Let copiedEntry be a new Record { [[Key]]: entry.[[Key]], [[Value]]: entry.[[Value]] }.
                    // 2. If copiedEntry.[[Key]] is not the special value empty, append copiedEntry to copiedList.
                    copied_list.append(entry.key);
                    copied_list.append(entry.value);
                }

                encode(static_cast<u64>(map->map_size()));

                // 3. For each Record { [[Key]], [[Value]] } entry of copiedList:
                for (auto copied_value : copied_list) {
                    // 1. Let serializedKey be ? StructuredSerializeInternal(entry.[[Key]], forStorage, memory).
                    // 2. Let serializedValue be ? StructuredSerializeInternal(entry.[[Value]], forStorage, memory).
                    TRY(structured_serialize_internal(m_vm, serialized, copied_value, m_for_storage, m_memory));

                    // 3. Append { [[Key]]: serializedKey, [[Value]]: serializedValue } to serialized.[[MapData]].
                }
            }

            // 2. Otherwise, if value has a [[SetData]] internal slot, then:
            else if (auto const* set = as_if<JS::Set>(object)) {
                // 1. Let copiedList be a new empty List.
                Vector<JS::Value> copied_list;
                copied_list.ensure_capacity(set->set_size());

                // 2. For each entry of value.[[SetData]]:
                for (auto entry : *set) {
                    // 1. If entry is not the special value empty, append entry to copiedList.
                    copied_list.append(entry);
                }

                encode(static_cast<u64>(set->set_size()));

                // 3. For each entry of copiedList:
                for (auto copied_value : copied_list) {
                    // 1. Let serializedEntry be ? StructuredSerializeInternal(entry, forStorage, memory).
                    TRY(structured_serialize_internal(m_vm, serialized, copied_value, m_for_storage, m_memory));

                    // 2. Append serializedEntry to serialized.[[SetData]].
                }
            }

            // 3. Otherwise, if value is a platform object that is a serializable object, then perform the serialization steps for value's primary interface, given value, serialized, and forStorage.
            else if (auto* serializable = as_if<Bindings::Serializable>(object)) {
                TRY(serializable->serialization_steps(serialized, m_for_storage, m_memory));
            }

            // 4. Otherwise, for each key in ! EnumerableOwnProperties(value, key):
            else {
                for (auto key : MUST(object.enumerable_own_property_names(JS::Object::PropertyKind::Key))) {
                    auto property_key = MUST(JS::PropertyKey::from_value(m_vm, key));

                    // 1. If ! HasOwnProperty(value, key) is true, then:
                    if (MUST(object.has_own_property(property_key))) {
                        // 1. Let inputValue be ? value.[[Get]](key, value).
                        auto input_value = TRY(object.internal_get(property_key, value));

                        // 2. Let outputValue be ? StructuredSerializeInternal(inputValue, forStorage, memory).
                        TRY(structured_serialize_internal(m_vm, serialized, input_value, m_for_storage, m_memory));

                        // 3. Append { [[Key]]: key, [[Value]]: outputValue } to serialized.[[Properties]].
                        encode(key.as_string().utf16_string());
                    }
                }

                encode(ValueTag::EndObject);
            }
        }

        // 27. Return serialized.
        return {};
    }

private:
    template<typename T>
    void encode(T const& value)
    {
        m_serialized.encode(value);
    }

    JS::VM& m_vm;
    StructuredSerializeWriter& m_serialized;
    SerializationMemory& m_memory; // JS value -> index
    bool m_for_storage { false };
};

class Deserializer {
public:
    Deserializer(JS::VM& vm, StructuredSerializeReader& serialized, JS::Realm& target_realm, DeserializationMemory& memory)
        : m_vm(vm)
        , m_serialized(serialized)
        , m_memory(memory)
    {
        VERIFY(vm.current_realm() == &target_realm);
    }

    // https://html.spec.whatwg.org/multipage/structured-data.html#structureddeserialize
    WebIDL::ExceptionOr<JS::Value> deserialize()
    {
        return deserialize_value(TRY(decode<u8>()));
    }

    // The Object/Array property loop is the only caller allowed to pre-read EndObject.
    WebIDL::ExceptionOr<JS::Value> deserialize_value(u8 raw_tag)
    {
        if (m_vm.did_reach_stack_space_limit())
            return m_vm.throw_completion<JS::InternalError>(JS::ErrorType::CallStackSizeExceeded);

        auto& realm = *m_vm.current_realm();

        auto tag = static_cast<ValueTag>(raw_tag);

        // EndObject is handled by the Object/Array property loop, not here.
        if (!value_tag_is_decodable(tag))
            return data_clone_error_from_serialization_error(realm, AK::Error::from_string_literal("Unexpected structured serialize value tag"));

        // 2. If memory[serialized] exists, then return memory[serialized].
        if (tag == ValueTag::ObjectReference) {
            auto index = TRY(decode<u32>());
            if (index == NumericLimits<u32>::max())
                return JS::Object::create(*m_vm.current_realm(), nullptr);
            if (index >= m_memory.size())
                return data_clone_error_from_serialization_error(realm, AK::Error::from_string_literal("Object reference index out of range"));
            return m_memory[index];
        }

        // 3. Let deep be false.
        auto deep = false;

        // 4. Let value be an uninitialized value.
        JS::Value value;

        auto is_primitive = false;

        auto decode_string = [&]() -> WebIDL::ExceptionOr<GC::Ref<JS::PrimitiveString>> {
            auto string = TRY(decode<Utf16String>());
            return JS::PrimitiveString::create(m_vm, string);
        };

        auto decode_big_int = [&]() -> WebIDL::ExceptionOr<GC::Ref<JS::BigInt>> {
            return JS::BigInt::create(m_vm, TRY(decode_signed_big_integer(m_serialized, realm)));
        };

        switch (tag) {
        // 5. If serialized.[[Type]] is "primitive", then set value to serialized.[[Value]].
        case ValueTag::UndefinedPrimitive:
            value = JS::js_undefined();
            is_primitive = true;
            break;
        case ValueTag::NullPrimitive:
            value = JS::js_null();
            is_primitive = true;
            break;
        case ValueTag::BooleanPrimitive:
            value = JS::Value { TRY(decode<bool>()) };
            is_primitive = true;
            break;
        case ValueTag::Int32Primitive:
            value = JS::Value { TRY(decode<i32>()) };
            is_primitive = true;
            break;
        case ValueTag::NumberPrimitive:
            value = JS::Value { TRY(decode<double>()) };
            is_primitive = true;
            break;
        case ValueTag::BigIntPrimitive:
            value = TRY(decode_big_int());
            is_primitive = true;
            break;
        case ValueTag::StringPrimitive:
            value = TRY(decode_string());
            is_primitive = true;
            break;

        // 6. Otherwise, if serialized.[[Type]] is "Boolean", then set value to a new Boolean object in targetRealm whose [[BooleanData]] internal slot value is serialized.[[BooleanData]].
        case ValueTag::BooleanObject:
            value = JS::BooleanObject::create(realm, TRY(decode<bool>()));
            break;

        // 7. Otherwise, if serialized.[[Type]] is "Number", then set value to a new Number object in targetRealm whose [[NumberData]] internal slot value is serialized.[[NumberData]].
        case ValueTag::NumberObject:
            value = JS::NumberObject::create(realm, TRY(decode<double>()));
            break;

        // 8. Otherwise, if serialized.[[Type]] is "BigInt", then set value to a new BigInt object in targetRealm whose [[BigIntData]] internal slot value is serialized.[[BigIntData]].
        case ValueTag::BigIntObject:
            value = JS::BigIntObject::create(realm, TRY(decode_big_int()));
            break;

        // 9. Otherwise, if serialized.[[Type]] is "String", then set value to a new String object in targetRealm whose [[StringData]] internal slot value is serialized.[[StringData]].
        case ValueTag::StringObject:
            value = JS::StringObject::create(realm, TRY(decode_string()), realm.intrinsics().string_prototype());
            break;

        // 10. Otherwise, if serialized.[[Type]] is "Date", then set value to a new Date object in targetRealm whose [[DateValue]] internal slot value is serialized.[[DateValue]].
        case ValueTag::DateObject:
            value = JS::Date::create(realm, TRY(decode<double>()));
            break;

        // 11. Otherwise, if serialized.[[Type]] is "RegExp", then set value to a new RegExp object in targetRealm whose [[RegExpMatcher]] internal slot value is serialized.[[RegExpMatcher]],
        //     whose [[OriginalSource]] internal slot value is serialized.[[OriginalSource]], and whose [[OriginalFlags]] internal slot value is serialized.[[OriginalFlags]].
        case ValueTag::RegExpObject: {
            auto pattern = TRY(decode_string());
            auto flags = TRY(decode_string());

            auto regexp = JS::regexp_create(m_vm, pattern, flags);
            if (regexp.is_error())
                return data_clone_error_from_serialization_error(realm, AK::Error::from_string_literal("Invalid RegExp pattern or flags"));
            value = regexp.release_value();
            break;
        }

        // 12. Otherwise, if serialized.[[Type]] is "SharedArrayBuffer", then:
        case ValueTag::SharedArrayBuffer: {
            // FIXME: 1. If targetRealm's corresponding agent cluster is not serialized.[[AgentCluster]], then throw a "DataCloneError" DOMException.

            // 2. Otherwise, set value to a new SharedArrayBuffer object in targetRealm whose [[ArrayBufferData]] internal slot value is serialized.[[ArrayBufferData]]
            //    and whose [[ArrayBufferByteLength]] internal slot value is serialized.[[ArrayBufferByteLength]].
            auto buffer = TRY(decode<ByteBuffer>());
            value = JS::ArrayBuffer::create(realm, move(buffer), JS::DataBlock::Shared::Yes);
            break;
        }

        // 13. Otherwise, if serialized.[[Type]] is "GrowableSharedArrayBuffer", then:
        case ValueTag::GrowableSharedArrayBuffer: {
            // FIXME: 1. If targetRealm's corresponding agent cluster is not serialized.[[AgentCluster]], then throw a "DataCloneError" DOMException.

            // 2. Otherwise, set value to a new SharedArrayBuffer object in targetRealm whose [[ArrayBufferData]] internal slot value is serialized.[[ArrayBufferData]],
            //    whose [[ArrayBufferByteLengthData]] internal slot value is serialized.[[ArrayBufferByteLengthData]],
            //    and whose [[ArrayBufferMaxByteLength]] internal slot value is serialized.[[ArrayBufferMaxByteLength]].
            auto buffer = TRY(decode<ByteBuffer>());
            auto max_byte_length = TRY(decode_max_byte_length());

            auto data = JS::ArrayBuffer::create(realm, move(buffer), JS::DataBlock::Shared::Yes);
            data->set_max_byte_length(max_byte_length);

            value = data;
            break;
        }

        // 14. Otherwise, if serialized.[[Type]] is "ArrayBuffer", then set value to a new ArrayBuffer object in targetRealm whose [[ArrayBufferData]] internal slot value is serialized.[[ArrayBufferData]], and whose [[ArrayBufferByteLength]] internal slot value is serialized.[[ArrayBufferByteLength]].
        case ValueTag::ArrayBuffer: {
            auto buffer = TRY(decode<ByteBuffer>());
            value = JS::ArrayBuffer::create(realm, move(buffer));
            break;
        }

        // 15. Otherwise, if serialized.[[Type]] is "ResizableArrayBuffer", then set value to a new ArrayBuffer object in targetRealm whose [[ArrayBufferData]] internal slot value is serialized.[[ArrayBufferData]], whose [[ArrayBufferByteLength]] internal slot value is serialized.[[ArrayBufferByteLength]], and whose [[ArrayBufferMaxByteLength]] internal slot value is a serialized.[[ArrayBufferMaxByteLength]].
        case ValueTag::ResizeableArrayBuffer: {
            auto buffer = TRY(decode<ByteBuffer>());
            auto max_byte_length = TRY(decode_max_byte_length());

            auto data = JS::ArrayBuffer::create(realm, move(buffer));
            data->set_max_byte_length(max_byte_length);

            value = data;
            break;
        }

        // 16. Otherwise, if serialized.[[Type]] is "ArrayBufferView", then:
        case ValueTag::ArrayBufferView: {
            auto array_buffer_value = TRY(deserialize());
            if (!array_buffer_value.is_object() || !is<JS::ArrayBuffer>(array_buffer_value.as_object()))
                return data_clone_error_from_serialization_error(realm, AK::Error::from_string_literal("ArrayBufferView is not backed by an ArrayBuffer"));
            auto& array_buffer = static_cast<JS::ArrayBuffer&>(array_buffer_value.as_object());

            auto deserialize_byte_length = [&]() -> WebIDL::ExceptionOr<JS::ByteLength> {
                auto is_auto = TRY(decode<bool>());
                if (is_auto)
                    return JS::ByteLength::auto_();

                auto length = TRY(decode<u32>());
                return length;
            };

            auto constructor_name = TRY(decode<Utf16String>());
            auto byte_length = TRY(deserialize_byte_length());
            auto byte_offset = TRY(decode<u32>());

            // Validate in 64 bits; the engine's element-access check works in u32.
            auto length_fits_buffer = [&](JS::ByteLength const& length, u64 element_size) {
                if (length.is_auto())
                    return !array_buffer.is_fixed_length() && byte_offset <= array_buffer.byte_length();
                Checked<u64> end = length.length();
                end *= element_size;
                end += byte_offset;
                return !end.has_overflow() && end.value() <= array_buffer.byte_length();
            };

            if (constructor_name == "DataView"sv) {
                if (!length_fits_buffer(byte_length, 1))
                    return data_clone_error_from_serialization_error(realm, AK::Error::from_string_literal("DataView does not fit its ArrayBuffer"));
                value = JS::DataView::create(realm, &array_buffer, byte_length, byte_offset);
            } else {
                auto array_length = TRY(deserialize_byte_length());

                GC::Ptr<JS::TypedArrayBase> typed_array;
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, Type) \
    if (constructor_name == #ClassName##sv)                                         \
        typed_array = JS::ClassName::create(realm, 0, array_buffer);
                JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE
#undef CREATE_TYPED_ARRAY

                if (!typed_array)
                    return data_clone_error_from_serialization_error(realm, AK::Error::from_string_literal("Unknown ArrayBufferView constructor"));

                auto element_size = typed_array->element_size();

                // Reject combinations a real typed array could not produce.
                auto consistent_slots = byte_length.is_auto() == array_length.is_auto() && byte_offset % element_size == 0;
                if (consistent_slots && !array_length.is_auto()) {
                    Checked<u64> expected_byte_length = array_length.length();
                    expected_byte_length *= element_size;
                    consistent_slots = !expected_byte_length.has_overflow() && byte_length.length() == expected_byte_length.value();
                }
                if (!consistent_slots || !length_fits_buffer(array_length, element_size))
                    return data_clone_error_from_serialization_error(realm, AK::Error::from_string_literal("ArrayBufferView does not fit its ArrayBuffer"));

                typed_array->set_array_length(array_length);
                typed_array->set_byte_length(byte_length);
                typed_array->set_byte_offset(byte_offset);
                value = typed_array;
            }
            break;
        }

        // 17. Otherwise, if serialized.[[Type]] is "Map", then:
        case ValueTag::MapObject: {
            // 1. Set value to a new Map object in targetRealm whose [[MapData]] internal slot value is a new empty List.
            value = JS::Map::create(realm);

            // 2. Set deep to true.
            deep = true;
            break;
        }

        // 18. Otherwise, if serialized.[[Type]] is "Set", then:
        case ValueTag::SetObject: {
            // 1. Set value to a new Set object in targetRealm whose [[SetData]] internal slot value is a new empty List.
            value = JS::Set::create(realm);

            // 2. Set deep to true.
            deep = true;
            break;
        }

        // 19. Otherwise, if serialized.[[Type]] is "Array", then:
        case ValueTag::ArrayObject: {
            // 1. Let outputProto be targetRealm.[[Intrinsics]].[[%Array.prototype%]].
            // 2. Set value to ! ArrayCreate(serialized.[[Length]], outputProto).
            auto length = TRY(decode<u64>());
            auto array = JS::Array::create(realm, length, realm.intrinsics().array_prototype());
            if (array.is_error())
                return data_clone_error_from_serialization_error(realm, AK::Error::from_string_literal("Array length out of range"));
            value = array.release_value();

            // 3. Set deep to true.
            deep = true;
            break;
        }

        // 20. Otherwise, if serialized.[[Type]] is "Object", then:
        case ValueTag::Object: {
            // 1. Set value to a new Object in targetRealm.
            value = JS::Object::create(realm, realm.intrinsics().object_prototype());

            // 2. Set deep to true.
            deep = true;
            break;
        }

        // 21. Otherwise, if serialized.[[Type]] is "Error", then:
        case ValueTag::ErrorObject: {
            auto type = TRY(decode<ErrorType>());
            auto message = TRY(decode<Optional<Utf16String>>());
            auto cause = TRY(decode<Optional<Utf16String>>());

            GC::Ptr<JS::Error> error;

            switch (type) {
            case ErrorType::Error:
                error = JS::Error::create(realm);
                break;
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    case ErrorType::ClassName:                                                           \
        error = JS::ClassName::create(realm);                                            \
        break;
                JS_ENUMERATE_NATIVE_ERRORS
#undef __JS_ENUMERATE
            }

            // decode<ErrorType> validated type, and every ErrorType has a case above, so error is set.
            VERIFY(error);

            if (message.has_value())
                error->set_message(message.release_value());

            if (cause.has_value())
                error->create_non_enumerable_data_property_or_throw(m_vm.names.cause, JS::PrimitiveString::create(m_vm, cause.release_value()));

            value = error;
            break;
        }

        // 22. Otherwise:
        default:
            if (tag != ValueTag::SerializableObject)
                return data_clone_error_from_serialization_error(realm, AK::Error::from_string_literal("Unexpected structured serialize value tag"));

            // 1. Let interfaceName be serialized.[[Type]].
            auto interface_name = TRY(decode<Bindings::InterfaceName>());

            Optional<u64> version;
            if (m_serialized.type() == SerializationType::Storage)
                version = TRY(decode<u64>());

            // 2. If the interface identified by interfaceName is not exposed in targetRealm, then throw a "DataCloneError" DOMException.
            if (!is_exposed(interface_name, realm))
                return WebIDL::DataCloneError::create(realm, "Unsupported type"_utf16);

            // 3. Set value to a new instance of the interface identified by interfaceName, created in targetRealm.
            value = create_serialized_type(interface_name, realm);

            if (version.has_value()) {
                auto supported_version = as<Bindings::Serializable>(value.as_object()).serialization_version();
                if (*version == 0 || *version != supported_version)
                    return data_clone_error_from_serialization_error(realm, AK::Error::from_string_literal("Unsupported serializable payload version"));
            }

            // 4. Set deep to true.
            deep = true;
        }

        // 23. Set memory[serialized] to value.
        // IMPLEMENTATION DEFINED: We don't add primitive values to the memory to match the serialization indices (which also doesn't add them)
        if (!is_primitive)
            m_memory.append(value);

        // 24. If deep is true, then:
        if (deep) {
            // 1. If serialized.[[Type]] is "Map", then:
            if (tag == ValueTag::MapObject) {
                auto& map = as<JS::Map>(value.as_object());
                auto length = TRY(decode<u64>());

                // 1. For each Record { [[Key]], [[Value]] } entry of serialized.[[MapData]]:
                for (u64 i = 0u; i < length; ++i) {
                    // 1. Let deserializedKey be ? StructuredDeserialize(entry.[[Key]], targetRealm, memory).
                    auto deserialized_key = TRY(deserialize());

                    // 2. Let deserializedValue be ? StructuredDeserialize(entry.[[Value]], targetRealm, memory).
                    auto deserialized_value = TRY(deserialize());

                    // 3. Append { [[Key]]: deserializedKey, [[Value]]: deserializedValue } to value.[[MapData]].
                    map.map_set(deserialized_key, deserialized_value);
                }
            }

            // 2. Otherwise, if serialized.[[Type]] is "Set", then:
            else if (tag == ValueTag::SetObject) {
                auto& set = as<JS::Set>(value.as_object());
                auto length = TRY(decode<u64>());

                // 1. For each entry of serialized.[[SetData]]:
                for (u64 i = 0u; i < length; ++i) {
                    // 1. Let deserializedEntry be ? StructuredDeserialize(entry, targetRealm, memory).
                    auto deserialized_entry = TRY(deserialize());

                    // 2. Append deserializedEntry to value.[[SetData]].
                    set.set_add(deserialized_entry);
                }
            }

            // 3. Otherwise, if serialized.[[Type]] is "Array" or "Object", then:
            else if (tag == ValueTag::ArrayObject || tag == ValueTag::Object) {
                auto& object = value.as_object();

                // 1. For each Record { [[Key]], [[Value]] } entry of serialized.[[Properties]]:
                // EndObject ends the list; any other tag begins a property whose value precedes its untagged key.
                while (true) {
                    auto raw_property_tag = TRY(decode<u8>());
                    if (raw_property_tag == to_underlying(ValueTag::EndObject))
                        break;
                    auto deserialized_value = TRY(deserialize_value(raw_property_tag));
                    auto key = TRY(decode<Utf16String>());

                    // 2. Let result be ! CreateDataProperty(value, entry.[[Key]], deserializedValue).
                    auto result = object.create_data_property(key, deserialized_value);
                    if (result.is_error() || !result.release_value())
                        return data_clone_error_from_serialization_error(realm, AK::Error::from_string_literal("Invalid serialized property"));
                }
            }

            // 4. Otherwise:
            else {
                // 1. Perform the appropriate deserialization steps for the interface identified by serialized.[[Type]], given serialized, value, and targetRealm.
                auto& serializable = as<Bindings::Serializable>(value.as_object());
                TRY(serializable.deserialization_steps(m_serialized, m_memory));
            }
        }

        // 25. Return value.
        return value;
    }

private:
    template<typename T>
    WebIDL::ExceptionOr<T> decode()
    {
        return decode_or_throw_data_clone_error<T>(*m_vm.current_realm(), m_serialized);
    }

    WebIDL::ExceptionOr<size_t> decode_max_byte_length()
    {
        auto value = TRY(decode<u64>());
        if (!AK::is_within_range<size_t>(value))
            return data_clone_error_from_serialization_error(*m_vm.current_realm(), AK::Error::from_string_literal("Max byte length does not fit on this platform"));
        return static_cast<size_t>(value);
    }

    static GC::Ref<Bindings::PlatformObject> create_serialized_type(Bindings::InterfaceName serialize_type, JS::Realm& realm)
    {
        for (auto const& entry : serializable_storage_registry()) {
            if (entry.interface_name == serialize_type)
                return entry.create(realm);
        }
        VERIFY_NOT_REACHED();
    }

    JS::VM& m_vm;
    StructuredSerializeReader& m_serialized;
    DeserializationMemory& m_memory;
};

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializewithtransfer
WebIDL::ExceptionOr<SerializedTransferRecord> structured_serialize_with_transfer(JS::VM& vm, JS::Value value, ReadonlySpan<GC::Ref<JS::Object>> transfer_list)
{
    // 1. Let memory be an empty map.
    SerializationMemory memory = {};

    // 2. For each transferable of transferList:
    for (auto const& transferable : transfer_list) {
        auto const* as_array_buffer = as_if<JS::ArrayBuffer>(*transferable);

        // 1. If transferable has neither an [[ArrayBufferData]] internal slot nor a [[Detached]] internal slot, then throw a "DataCloneError" DOMException.
        // FIXME: Handle transferring objects with [[Detached]] internal slot.
        if (!as_array_buffer && !is<Bindings::Transferable>(*transferable))
            return WebIDL::DataCloneError::create(*vm.current_realm(), "Cannot transfer type"_utf16);

        // 2. If transferable has an [[ArrayBufferData]] internal slot and IsSharedArrayBuffer(transferable) is true, then throw a "DataCloneError" DOMException.
        if (as_array_buffer && as_array_buffer->is_shared_array_buffer())
            return WebIDL::DataCloneError::create(*vm.current_realm(), "Cannot transfer shared array buffer"_utf16);

        JS::Value transferable_value { transferable };

        // 3. If memory[transferable] exists, then throw a "DataCloneError" DOMException.
        if (memory.contains(transferable_value))
            return WebIDL::DataCloneError::create(*vm.current_realm(), "Cannot transfer value twice"_utf16);

        // 4. Set memory[transferable] to { [[Type]]: an uninitialized value }.
        memory.set(GC::make_root(transferable_value), memory.size());
    }

    // 3. Let serialized be ? StructuredSerializeInternal(value, false, memory).
    auto serialized = StructuredSerializeWriter::create_ipc();
    TRY(structured_serialize_internal(vm, serialized, value, false, memory));
    auto serialized_record = serialized.take_ipc_record();

    // 4. Let transferDataHolders be a new empty List.
    Vector<TransferDataEncoder> transfer_data_holders;
    transfer_data_holders.ensure_capacity(transfer_list.size());

    // 5. For each transferable of transferList:
    for (auto const& transferable : transfer_list) {
        auto* array_buffer = as_if<JS::ArrayBuffer>(*transferable);
        auto is_detached = array_buffer && array_buffer->is_detached();

        // 1. If transferable has an [[ArrayBufferData]] internal slot and IsDetachedBuffer(transferable) is true, then throw a "DataCloneError" DOMException.
        if (is_detached)
            return WebIDL::DataCloneError::create(*vm.current_realm(), "Cannot transfer detached buffer"_utf16);

        // 2. If transferable has a [[Detached]] internal slot and transferable.[[Detached]] is true, then throw a "DataCloneError" DOMException.
        if (auto* transferable_object = as_if<Bindings::Transferable>(*transferable)) {
            if (transferable_object->is_detached())
                return WebIDL::DataCloneError::create(*vm.current_realm(), "Value already transferred"_utf16);
        }

        // 3. Let dataHolder be memory[transferable].
        // IMPLEMENTATION DEFINED: We just create a data holder here, our memory holds indices into the serialized record.
        TransferDataEncoder data_holder;

        // 4. If transferable has an [[ArrayBufferData]] internal slot, then:
        if (array_buffer) {
            // 1. If transferable has an [[ArrayBufferMaxByteLength]] internal slot, then:
            auto buffer_data = MUST(array_buffer->copy_to_byte_buffer());

            if (!array_buffer->is_fixed_length()) {
                // 1. Set dataHolder.[[Type]] to "ResizableArrayBuffer".
                MUST(data_holder.encode(TransferType::ResizableArrayBuffer));

                // 2. Set dataHolder.[[ArrayBufferData]] to transferable.[[ArrayBufferData]].
                // 3. Set dataHolder.[[ArrayBufferByteLength]] to transferable.[[ArrayBufferByteLength]].
                MUST(data_holder.encode(buffer_data));

                // 4. Set dataHolder.[[ArrayBufferMaxByteLength]] to transferable.[[ArrayBufferMaxByteLength]].
                MUST(data_holder.encode(array_buffer->max_byte_length()));
            }
            // 2. Otherwise:
            else {
                // 1. Set dataHolder.[[Type]] to "ArrayBuffer".
                MUST(data_holder.encode(TransferType::ArrayBuffer));

                // 2. Set dataHolder.[[ArrayBufferData]] to transferable.[[ArrayBufferData]].
                // 3. Set dataHolder.[[ArrayBufferByteLength]] to transferable.[[ArrayBufferByteLength]].
                MUST(data_holder.encode(buffer_data));
            }

            // 3. Perform ? DetachArrayBuffer(transferable).
            // NOTE: Specifications can use the [[ArrayBufferDetachKey]] internal slot to prevent ArrayBuffers from being detached. This is used in WebAssembly JavaScript Interface, for example. See: https://html.spec.whatwg.org/multipage/references.html#refsWASMJS
            TRY(JS::detach_array_buffer(vm, *array_buffer));
        }
        // 5. Otherwise:
        else {
            // 1. Assert: transferable is a platform object that is a transferable object.
            auto& transferable_object = as<Bindings::Transferable>(*transferable);
            VERIFY(is<Bindings::PlatformObject>(*transferable));

            // 2. Let interfaceName be the identifier of the primary interface of transferable.
            auto interface_name = transferable_object.primary_interface();

            // 3. Set dataHolder.[[Type]] to interfaceName.
            MUST(data_holder.encode(interface_name));

            // 4. Perform the appropriate transfer steps for the interface identified by interfaceName, given transferable and dataHolder.
            TRY(transferable_object.transfer_steps(data_holder));

            // 5. Set transferable.[[Detached]] to true.
            transferable_object.set_detached(true);
        }

        // 6. Append dataHolder to transferDataHolders.
        transfer_data_holders.append(move(data_holder));
    }

    // 6. Return { [[Serialized]]: serialized, [[TransferDataHolders]]: transferDataHolders }.
    return SerializedTransferRecord { .serialized = move(serialized_record), .transfer_data_holders = move(transfer_data_holders) };
}

static bool is_transferable_interface_exposed_on_target_realm(TransferType name, JS::Realm& realm)
{
    switch (name) {
    case TransferType::MessagePort:
        return is_exposed(Bindings::InterfaceName::MessagePort, realm);
    case TransferType::ReadableStream:
        return is_exposed(Bindings::InterfaceName::ReadableStream, realm);
    case TransferType::WritableStream:
        return is_exposed(Bindings::InterfaceName::WritableStream, realm);
    case TransferType::TransformStream:
        return is_exposed(Bindings::InterfaceName::TransformStream, realm);
    case TransferType::ImageBitmap:
        return is_exposed(Bindings::InterfaceName::ImageBitmap, realm);
    case TransferType::Unknown:
        dbgln("Unknown interface type for transfer: {}", to_underlying(name));
        break;
    default:
        VERIFY_NOT_REACHED();
    }
    return false;
}

static WebIDL::ExceptionOr<GC::Ref<Bindings::PlatformObject>> create_transferred_value(TransferType name, JS::Realm& target_realm, TransferDataDecoder& decoder)
{
    switch (name) {
    case TransferType::MessagePort: {
        auto message_port = HTML::MessagePort::create(target_realm);
        TRY(message_port->transfer_receiving_steps(decoder));
        return message_port;
    }
    case TransferType::ReadableStream: {
        auto readable_stream = target_realm.create<Streams::ReadableStream>(target_realm);
        TRY(readable_stream->transfer_receiving_steps(decoder));
        return readable_stream;
    }
    case TransferType::WritableStream: {
        auto writable_stream = target_realm.create<Streams::WritableStream>(target_realm);
        TRY(writable_stream->transfer_receiving_steps(decoder));
        return writable_stream;
    }
    case TransferType::TransformStream: {
        auto transform_stream = target_realm.create<Streams::TransformStream>(target_realm);
        TRY(transform_stream->transfer_receiving_steps(decoder));
        return transform_stream;
    }
    case TransferType::ImageBitmap: {
        auto image_bitmap = target_realm.create<ImageBitmap>(target_realm);
        TRY(image_bitmap->transfer_receiving_steps(decoder));
        return image_bitmap;
    }
    case TransferType::ArrayBuffer:
    case TransferType::ResizableArrayBuffer:
    case TransferType::Unknown:
        break;
    }
    VERIFY_NOT_REACHED();
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structureddeserializewithtransfer
WebIDL::ExceptionOr<DeserializedTransferRecord> structured_deserialize_with_transfer(SerializedTransferRecord& serialize_with_transfer_result, JS::Realm& target_realm)
{
    auto& vm = target_realm.vm();

    // 1. Let memory be an empty map.
    DeserializationMemory memory {};

    // 2. Let transferredValues be a new empty List.
    Vector<GC::Root<JS::Object>> transferred_values;

    // 3. For each transferDataHolder of serializeWithTransferResult.[[TransferDataHolders]]:
    for (auto& transfer_data_holder : serialize_with_transfer_result.transfer_data_holders) {
        if (transfer_data_holder.buffer().data().is_empty())
            continue;

        TransferDataDecoder decoder { move(transfer_data_holder) };

        // 1. Let value be an uninitialized value.
        auto value = TRY(structured_deserialize_with_transfer_internal(decoder, target_realm));

        // 5. Set memory[transferDataHolder] to value.
        memory.append(value);

        // 6. Append value to transferredValues.
        transferred_values.append(GC::make_root(value.as_object()));
    }

    // 4. Let deserialized be ? StructuredDeserialize(serializeWithTransferResult.[[Serialized]], targetRealm, memory).
    auto deserialized = TRY(structured_deserialize(vm, serialize_with_transfer_result.serialized, target_realm, memory));

    // 5. Return { [[Deserialized]]: deserialized, [[TransferredValues]]: transferredValues }.
    return DeserializedTransferRecord { .deserialized = deserialized, .transferred_values = move(transferred_values) };
}

// AD-HOC: This non-standard overload is meant to extract just one transferrable value from a serialized transfer record.
//         It's primarily useful for an object's transfer receiving steps to deserialize a nested value.
WebIDL::ExceptionOr<JS::Value> structured_deserialize_with_transfer_internal(TransferDataDecoder& decoder, JS::Realm& target_realm)
{
    auto type = TRY(decode_or_throw_data_clone_error<TransferType>(target_realm, decoder));

    // 1. Let value be an uninitialized value.
    JS::Value value;

    // 2. If transferDataHolder.[[Type]] is "ArrayBuffer", then set value to a new ArrayBuffer object in targetRealm
    //    whose [[ArrayBufferData]] internal slot value is transferDataHolder.[[ArrayBufferData]], and
    //    whose [[ArrayBufferByteLength]] internal slot value is transferDataHolder.[[ArrayBufferByteLength]].
    // NOTE: In cases where the original memory occupied by [[ArrayBufferData]] is accessible during the deserialization,
    //       this step is unlikely to throw an exception, as no new memory needs to be allocated: the memory occupied by
    //       [[ArrayBufferData]] is instead just getting transferred into the new ArrayBuffer. This could be true, for example,
    //       when both the source and target realms are in the same process.
    if (type == TransferType::ArrayBuffer) {
        auto buffer = TRY(decode_or_throw_data_clone_error<ByteBuffer>(target_realm, decoder));
        value = JS::ArrayBuffer::create(target_realm, move(buffer));
    }

    // 3. Otherwise, if transferDataHolder.[[Type]] is "ResizableArrayBuffer", then set value to a new ArrayBuffer object
    //     in targetRealm whose [[ArrayBufferData]] internal slot value is transferDataHolder.[[ArrayBufferData]], whose
    //     [[ArrayBufferByteLength]] internal slot value is transferDataHolder.[[ArrayBufferByteLength]], and whose
    //     [[ArrayBufferMaxByteLength]] internal slot value is transferDataHolder.[[ArrayBufferMaxByteLength]].
    // NOTE: For the same reason as the previous step, this step is also unlikely to throw an exception.
    else if (type == TransferType::ResizableArrayBuffer) {
        auto buffer = TRY(decode_or_throw_data_clone_error<ByteBuffer>(target_realm, decoder));
        auto max_byte_length = TRY(decode_or_throw_data_clone_error<size_t>(target_realm, decoder));

        auto data = JS::ArrayBuffer::create(target_realm, move(buffer));
        data->set_max_byte_length(max_byte_length);

        value = data;
    }

    // 4. Otherwise:
    else {
        // 1. Let interfaceName be transferDataHolder.[[Type]].
        // 2. If the interface identified by interfaceName is not exposed in targetRealm, then throw a "DataCloneError" DOMException.
        if (!is_transferable_interface_exposed_on_target_realm(type, target_realm))
            return WebIDL::DataCloneError::create(target_realm, "Unknown type transferred"_utf16);

        // 3. Set value to a new instance of the interface identified by interfaceName, created in targetRealm.
        // 4. Perform the appropriate transfer-receiving steps for the interface identified by interfaceName given transferDataHolder and value.
        value = TRY(create_transferred_value(type, target_realm, decoder));
    }

    return value;
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserialize
WebIDL::ExceptionOr<IPCSerializationRecord> structured_serialize(JS::VM& vm, JS::Value value)
{
    // 1. Return ? StructuredSerializeInternal(value, false).
    SerializationMemory memory = {};
    auto serialized = StructuredSerializeWriter::create_ipc();
    TRY(structured_serialize_internal(vm, serialized, value, false, memory));
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

// https://html.spec.whatwg.org/multipage/structured-data.html#structuredserializeinternal
WebIDL::ExceptionOr<void> structured_serialize_internal(JS::VM& vm, StructuredSerializeWriter& serialized, JS::Value value, bool for_storage, SerializationMemory& memory)
{
    // 1. If memory was not supplied, let memory be an empty map.
    // IMPLEMENTATION DEFINED: We move this requirement up to the callers to make recursion easier

    Serializer serializer(vm, serialized, memory, for_storage);
    return serializer.serialize(value);
}

// https://html.spec.whatwg.org/multipage/structured-data.html#structureddeserialize
WebIDL::ExceptionOr<JS::Value> structured_deserialize(JS::VM& vm, IPCSerializationRecord const& serialized, JS::Realm& target_realm, Optional<DeserializationMemory> memory)
{
    TemporaryExecutionContext execution_context { target_realm };

    if (!memory.has_value())
        memory = DeserializationMemory {};

    StructuredSerializeReader decoder { serialized };
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
    Deserializer deserializer(vm, serialized, target_realm, memory);
    auto value = TRY(deserializer.deserialize());

    // Nested sub-value decodes legitimately stop mid-record.
    if (check_fully_consumed == CheckFullyConsumed::Yes && serialized.type() == SerializationType::Storage && !serialized.is_at_end())
        return data_clone_error_from_serialization_error(target_realm, AK::Error::from_string_literal("Trailing data after structured serialize storage record"));

    return value;
}

TransferDataEncoder::TransferDataEncoder()
    : m_encoder(m_buffer)
{
}

TransferDataEncoder::TransferDataEncoder(IPC::MessageBuffer&& buffer)
    : m_buffer(move(buffer))
    , m_encoder(m_buffer)
{
}

IPC::MessageBuffer const& TransferDataEncoder::buffer() const
{
    return m_buffer;
}

IPC::MessageBuffer TransferDataEncoder::take_buffer() const
{
    VERIFY(!m_buffer_has_been_taken);
    m_buffer_has_been_taken = true;
    return move(m_buffer);
}

void TransferDataEncoder::append(IPCSerializationRecord&& record)
{
    VERIFY(!m_buffer_has_been_taken);
    MUST(m_buffer.append_data(record.data.data(), record.data.size()));
}

void TransferDataEncoder::extend(Vector<TransferDataEncoder> data_holders)
{
    for (auto& data_holder : data_holders)
        MUST(m_buffer.extend(data_holder.take_buffer()));
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

template WEB_API void StructuredSerializeWriter::encode<bool>(bool const&);
template WEB_API void StructuredSerializeWriter::encode<int>(int const&);
template WEB_API void StructuredSerializeWriter::encode<u32>(u32 const&);
template WEB_API void StructuredSerializeWriter::encode<u64>(u64 const&);
template WEB_API void StructuredSerializeWriter::encode<i64>(i64 const&);
template WEB_API void StructuredSerializeWriter::encode<double>(double const&);
template WEB_API void StructuredSerializeWriter::encode<Utf16String>(Utf16String const&);
template WEB_API void StructuredSerializeWriter::encode<ByteBuffer>(ByteBuffer const&);
template WEB_API void StructuredSerializeWriter::encode<ReadonlyBytes>(ReadonlyBytes const&);
template WEB_API void StructuredSerializeWriter::encode<Bindings::InterfaceName>(Bindings::InterfaceName const&);
template WEB_API void StructuredSerializeWriter::encode<Bindings::KeyType>(Bindings::KeyType const&);
template WEB_API void StructuredSerializeWriter::encode<Bindings::KeyUsage>(Bindings::KeyUsage const&);
template WEB_API void StructuredSerializeWriter::encode<Bindings::PredefinedColorSpace>(Bindings::PredefinedColorSpace const&);
template WEB_API void StructuredSerializeWriter::encode<Gfx::BitmapFormat>(Gfx::BitmapFormat const&);
template WEB_API void StructuredSerializeWriter::encode<Gfx::AlphaType>(Gfx::AlphaType const&);
template WEB_API void StructuredSerializeWriter::encode<u8>(u8 const&);
template WEB_API void StructuredSerializeWriter::encode<u16>(u16 const&);
template WEB_API void StructuredSerializeWriter::encode<Optional<double>>(Optional<double> const&);
template WEB_API void StructuredSerializeWriter::encode<Optional<Utf16String>>(Optional<Utf16String> const&);
template WEB_API void StructuredSerializeWriter::encode<Optional<bool>>(Optional<bool> const&);
template WEB_API void StructuredSerializeWriter::encode<Vector<Bindings::KeyUsage>>(Vector<Bindings::KeyUsage> const&);
template WEB_API void StructuredSerializeWriter::encode<Optional<Vector<int>>>(Optional<Vector<int>> const&);

template WEB_API ErrorOr<bool> StructuredSerializeReader::decode<bool>();
template WEB_API ErrorOr<int> StructuredSerializeReader::decode<int>();
template WEB_API ErrorOr<u32> StructuredSerializeReader::decode<u32>();
template WEB_API ErrorOr<u64> StructuredSerializeReader::decode<u64>();
template WEB_API ErrorOr<i64> StructuredSerializeReader::decode<i64>();
template WEB_API ErrorOr<double> StructuredSerializeReader::decode<double>();
template WEB_API ErrorOr<Utf16String> StructuredSerializeReader::decode<Utf16String>();
template WEB_API ErrorOr<ByteBuffer> StructuredSerializeReader::decode<ByteBuffer>();
template WEB_API ErrorOr<Bindings::InterfaceName> StructuredSerializeReader::decode<Bindings::InterfaceName>();
template WEB_API ErrorOr<Bindings::KeyType> StructuredSerializeReader::decode<Bindings::KeyType>();
template WEB_API ErrorOr<Bindings::KeyUsage> StructuredSerializeReader::decode<Bindings::KeyUsage>();
template WEB_API ErrorOr<Bindings::PredefinedColorSpace> StructuredSerializeReader::decode<Bindings::PredefinedColorSpace>();
template WEB_API ErrorOr<Gfx::BitmapFormat> StructuredSerializeReader::decode<Gfx::BitmapFormat>();
template WEB_API ErrorOr<Gfx::AlphaType> StructuredSerializeReader::decode<Gfx::AlphaType>();
template WEB_API ErrorOr<u8> StructuredSerializeReader::decode<u8>();
template WEB_API ErrorOr<u16> StructuredSerializeReader::decode<u16>();
template WEB_API ErrorOr<Optional<double>> StructuredSerializeReader::decode<Optional<double>>();
template WEB_API ErrorOr<Optional<Utf16String>> StructuredSerializeReader::decode<Optional<Utf16String>>();
template WEB_API ErrorOr<Optional<bool>> StructuredSerializeReader::decode<Optional<bool>>();
template WEB_API ErrorOr<Vector<Bindings::KeyUsage>> StructuredSerializeReader::decode<Vector<Bindings::KeyUsage>>();
template WEB_API ErrorOr<Optional<Vector<int>>> StructuredSerializeReader::decode<Optional<Vector<int>>>();

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

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::TransferDataEncoder const& data_holder)
{
    auto buffer = data_holder.take_buffer();
    auto data = buffer.take_data();
    auto attachments = buffer.take_attachments();

    TRY(encoder.encode(data));
    TRY(encoder.encode(static_cast<u32>(attachments.size())));
    for (auto& attachment : attachments)
        TRY(encoder.append_attachment(move(attachment)));

    return {};
}

template<>
ErrorOr<Web::HTML::TransferDataEncoder> decode(Decoder& decoder)
{
    auto data = TRY(decoder.decode<MessageDataType>());
    auto attachment_count = TRY(decoder.decode<u32>());

    Vector<Attachment> attachments;
    TRY(attachments.try_ensure_capacity(attachment_count));
    for (u32 i = 0; i < attachment_count; ++i)
        attachments.unchecked_append(TRY(decoder.attachments().try_dequeue()));

    IPC::MessageBuffer buffer { move(data), move(attachments) };
    return Web::HTML::TransferDataEncoder { move(buffer) };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::IPCSerializationRecord const& record)
{
    TRY(encoder.encode(record.data));
    return {};
}

template<>
ErrorOr<Web::HTML::IPCSerializationRecord> decode(Decoder& decoder)
{
    auto data = TRY(decoder.decode<MessageDataType>());
    return Web::HTML::IPCSerializationRecord { move(data) };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::StorageSerializationRecord const& record)
{
    TRY(encoder.encode(record.data));
    return {};
}

template<>
ErrorOr<Web::HTML::StorageSerializationRecord> decode(Decoder& decoder)
{
    auto data = TRY(decoder.decode<ByteBuffer>());
    return Web::HTML::StorageSerializationRecord { move(data) };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::HTML::SerializedTransferRecord const& record)
{
    TRY(encoder.encode(record.serialized));
    TRY(encoder.encode(record.transfer_data_holders));
    return {};
}

template<>
ErrorOr<Web::HTML::SerializedTransferRecord> decode(Decoder& decoder)
{
    auto serialized = TRY(decoder.decode<Web::HTML::IPCSerializationRecord>());
    auto transfer_data_holders = TRY(decoder.decode<Vector<Web::HTML::TransferDataEncoder>>());

    return Web::HTML::SerializedTransferRecord { move(serialized), move(transfer_data_holders) };
}

}
