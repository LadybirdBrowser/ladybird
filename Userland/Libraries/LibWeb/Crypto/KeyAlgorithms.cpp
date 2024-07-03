/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Crypto/KeyAlgorithms.h>

namespace Web::Crypto {

JS_DEFINE_ALLOCATOR(KeyAlgorithm);
JS_DEFINE_ALLOCATOR(RsaKeyAlgorithm);
JS_DEFINE_ALLOCATOR(RsaHashedKeyAlgorithm);
JS_DEFINE_ALLOCATOR(EcKeyAlgorithm);

template<typename T>
static JS::ThrowCompletionOr<T*> impl_from(JS::VM& vm, StringView Name)
{
    auto this_value = vm.this_value();
    JS::Object* this_object = nullptr;
    if (this_value.is_nullish())
        this_object = &vm.current_realm()->global_object();
    else
        this_object = TRY(this_value.to_object(vm));

    if (!is<T>(this_object))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, Name);
    return static_cast<T*>(this_object);
}

JS::NonnullGCPtr<KeyAlgorithm> KeyAlgorithm::create(JS::Realm& realm)
{
    return realm.heap().allocate<KeyAlgorithm>(realm, realm);
}

KeyAlgorithm::KeyAlgorithm(JS::Realm& realm)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().object_prototype())
    , m_realm(realm)
{
}

void KeyAlgorithm::initialize(JS::Realm& realm)
{
    define_native_accessor(realm, "name", name_getter, {}, JS::Attribute::Enumerable | JS::Attribute::Configurable);
    Base::initialize(realm);
}

JS_DEFINE_NATIVE_FUNCTION(KeyAlgorithm::name_getter)
{
    auto* impl = TRY(impl_from<KeyAlgorithm>(vm, "KeyAlgorithm"sv));
    auto name = TRY(Bindings::throw_dom_exception_if_needed(vm, [&] { return impl->name(); }));
    return JS::PrimitiveString::create(vm, name);
}

void KeyAlgorithm::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_realm);
}

JS::NonnullGCPtr<RsaKeyAlgorithm> RsaKeyAlgorithm::create(JS::Realm& realm)
{
    return realm.heap().allocate<RsaKeyAlgorithm>(realm, realm);
}

RsaKeyAlgorithm::RsaKeyAlgorithm(JS::Realm& realm)
    : KeyAlgorithm(realm)
    , m_public_exponent(MUST(JS::Uint8Array::create(realm, 0)))
{
}

void RsaKeyAlgorithm::initialize(JS::Realm& realm)
{
    Base::initialize(realm);

    define_native_accessor(realm, "modulusLength", modulus_length_getter, {}, JS::Attribute::Enumerable | JS::Attribute::Configurable);
    define_native_accessor(realm, "publicExponent", public_exponent_getter, {}, JS::Attribute::Enumerable | JS::Attribute::Configurable);
}

void RsaKeyAlgorithm::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_public_exponent);
}

WebIDL::ExceptionOr<void> RsaKeyAlgorithm::set_public_exponent(::Crypto::UnsignedBigInteger exponent)
{
    auto& realm = this->realm();
    auto& vm = this->vm();

    auto bytes = TRY_OR_THROW_OOM(vm, ByteBuffer::create_uninitialized(exponent.trimmed_byte_length()));

    bool const remove_leading_zeroes = true;
    auto data_size = exponent.export_data(bytes.span(), remove_leading_zeroes);
    auto data_slice_be = bytes.bytes().slice(bytes.size() - data_size, data_size);

    // The BigInteger typedef from the WebCrypto spec requires the bytes in the Uint8Array be ordered in Big Endian

    if constexpr (AK::HostIsLittleEndian) {
        Vector<u8, 32> data_slice_le;
        data_slice_le.ensure_capacity(data_size);
        for (size_t i = 0; i < data_size; ++i) {
            data_slice_le.append(data_slice_be[data_size - i - 1]);
        }
        m_public_exponent = TRY(JS::Uint8Array::create(realm, data_slice_le.size()));
        m_public_exponent->viewed_array_buffer()->buffer().overwrite(0, data_slice_le.data(), data_slice_le.size());
    } else {
        m_public_exponent = TRY(JS::Uint8Array::create(realm, data_slice_be.size()));
        m_public_exponent->viewed_array_buffer()->buffer().overwrite(0, data_slice_be.data(), data_slice_be.size());
    }

    return {};
}

JS_DEFINE_NATIVE_FUNCTION(RsaKeyAlgorithm::modulus_length_getter)
{
    auto* impl = TRY(impl_from<RsaKeyAlgorithm>(vm, "RsaKeyAlgorithm"sv));
    return JS::Value(impl->modulus_length());
}

JS_DEFINE_NATIVE_FUNCTION(RsaKeyAlgorithm::public_exponent_getter)
{
    auto* impl = TRY(impl_from<RsaKeyAlgorithm>(vm, "RsaKeyAlgorithm"sv));
    return impl->public_exponent();
}

JS::NonnullGCPtr<EcKeyAlgorithm> EcKeyAlgorithm::create(JS::Realm& realm)
{
    return realm.heap().allocate<EcKeyAlgorithm>(realm, realm);
}

EcKeyAlgorithm::EcKeyAlgorithm(JS::Realm& realm)
    : KeyAlgorithm(realm)
{
}

void EcKeyAlgorithm::initialize(JS::Realm& realm)
{
    Base::initialize(realm);

    define_native_accessor(realm, "namedCurve", named_curve_getter, {}, JS::Attribute::Enumerable | JS::Attribute::Configurable);
}

JS_DEFINE_NATIVE_FUNCTION(EcKeyAlgorithm::named_curve_getter)
{
    auto* impl = TRY(impl_from<EcKeyAlgorithm>(vm, "EcKeyAlgorithm"sv));
    return JS::PrimitiveString::create(vm, impl->named_curve());
}

JS::NonnullGCPtr<RsaHashedKeyAlgorithm> RsaHashedKeyAlgorithm::create(JS::Realm& realm)
{
    return realm.heap().allocate<RsaHashedKeyAlgorithm>(realm, realm);
}

RsaHashedKeyAlgorithm::RsaHashedKeyAlgorithm(JS::Realm& realm)
    : RsaKeyAlgorithm(realm)
    , m_hash(String {})
{
}

void RsaHashedKeyAlgorithm::initialize(JS::Realm& realm)
{
    Base::initialize(realm);

    define_native_accessor(realm, "hash", hash_getter, {}, JS::Attribute::Enumerable | JS::Attribute::Configurable);
}

JS_DEFINE_NATIVE_FUNCTION(RsaHashedKeyAlgorithm::hash_getter)
{
    auto* impl = TRY(impl_from<RsaHashedKeyAlgorithm>(vm, "RsaHashedKeyAlgorithm"sv));
    auto hash = TRY(Bindings::throw_dom_exception_if_needed(vm, [&] { return impl->hash(); }));
    return hash.visit(
        [&](String const& hash_string) -> JS::Value {
            return JS::PrimitiveString::create(vm, hash_string);
        },
        [&](JS::Handle<JS::Object> const& hash) -> JS::Value {
            return hash;
        });
}

}
