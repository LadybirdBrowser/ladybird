/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Memory.h>
#include <LibJS/Runtime/Array.h>
#include <LibWeb/Bindings/CryptoKeyPrototype.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Crypto/CryptoKey.h>

namespace Web::Crypto {

JS_DEFINE_ALLOCATOR(CryptoKey);
JS_DEFINE_ALLOCATOR(CryptoKeyPair);

JS::NonnullGCPtr<CryptoKey> CryptoKey::create(JS::Realm& realm, InternalKeyData key_data)
{
    return realm.heap().allocate<CryptoKey>(realm, realm, move(key_data));
}

JS::NonnullGCPtr<CryptoKey> CryptoKey::create(JS::Realm& realm)
{
    return realm.heap().allocate<CryptoKey>(realm, realm);
}

CryptoKey::CryptoKey(JS::Realm& realm, InternalKeyData key_data)
    : PlatformObject(realm)
    , m_algorithm(Object::create(realm, nullptr))
    , m_usages(Object::create(realm, nullptr))
    , m_key_data(move(key_data))
{
}

CryptoKey::CryptoKey(JS::Realm& realm)
    : PlatformObject(realm)
    , m_algorithm(Object::create(realm, nullptr))
    , m_usages(Object::create(realm, nullptr))
    , m_key_data(MUST(ByteBuffer::create_uninitialized(0)))
{
}

CryptoKey::~CryptoKey()
{
    m_key_data.visit(
        [](ByteBuffer& data) { secure_zero(data.data(), data.size()); },
        [](auto& data) { secure_zero(reinterpret_cast<u8*>(&data), sizeof(data)); });
}

void CryptoKey::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CryptoKey);
}

void CryptoKey::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_algorithm);
    visitor.visit(m_usages);
}

void CryptoKey::set_usages(Vector<Bindings::KeyUsage> usages)
{
    m_key_usages = move(usages);
    auto& realm = this->realm();
    m_usages = JS::Array::create_from<Bindings::KeyUsage>(realm, m_key_usages.span(), [&](auto& key_usage) -> JS::Value {
        return JS::PrimitiveString::create(realm.vm(), Bindings::idl_enum_to_string(key_usage));
    });
}

String CryptoKey::algorithm_name() const
{
    if (m_algorithm_name.is_empty()) {
        auto name = MUST(m_algorithm->get("name"));
        m_algorithm_name = MUST(name.to_string(vm()));
    }
    return m_algorithm_name;
}

JS::NonnullGCPtr<CryptoKeyPair> CryptoKeyPair::create(JS::Realm& realm, JS::NonnullGCPtr<CryptoKey> public_key, JS::NonnullGCPtr<CryptoKey> private_key)
{
    return realm.heap().allocate<CryptoKeyPair>(realm, realm, public_key, private_key);
}

CryptoKeyPair::CryptoKeyPair(JS::Realm& realm, JS::NonnullGCPtr<CryptoKey> public_key, JS::NonnullGCPtr<CryptoKey> private_key)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().object_prototype())
    , m_public_key(public_key)
    , m_private_key(private_key)
{
}

void CryptoKeyPair::initialize(JS::Realm& realm)
{
    define_native_accessor(realm, "publicKey", public_key_getter, {}, JS::Attribute::Enumerable | JS::Attribute::Configurable);
    define_native_accessor(realm, "privateKey", private_key_getter, {}, JS::Attribute::Enumerable | JS::Attribute::Configurable);

    Base::initialize(realm);
}

void CryptoKeyPair::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_public_key);
    visitor.visit(m_private_key);
}

static JS::ThrowCompletionOr<CryptoKeyPair*> impl_from(JS::VM& vm)
{
    auto this_value = vm.this_value();
    JS::Object* this_object = nullptr;
    if (this_value.is_nullish())
        this_object = &vm.current_realm()->global_object();
    else
        this_object = TRY(this_value.to_object(vm));

    if (!is<CryptoKeyPair>(this_object))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "CryptoKeyPair");
    return static_cast<CryptoKeyPair*>(this_object);
}

JS_DEFINE_NATIVE_FUNCTION(CryptoKeyPair::public_key_getter)
{
    auto* impl = TRY(impl_from(vm));
    return TRY(Bindings::throw_dom_exception_if_needed(vm, [&] { return impl->public_key(); }));
}

JS_DEFINE_NATIVE_FUNCTION(CryptoKeyPair::private_key_getter)
{
    auto* impl = TRY(impl_from(vm));
    return TRY(Bindings::throw_dom_exception_if_needed(vm, [&] { return impl->private_key(); }));
}

WebIDL::ExceptionOr<void> CryptoKey::serialization_steps(HTML::SerializationRecord& serialized, bool for_storage, HTML::SerializationMemory& memory)
{
    auto& vm = this->vm();

    // 1. Set serialized.[[Type]] to the [[type]] internal slot of value.
    HTML::serialize_primitive_type(serialized, static_cast<u32>(m_type));

    // 2. Set serialized.[[Extractable]] to the [[extractable]] internal slot of value.
    HTML::serialize_primitive_type(serialized, m_extractable);

    // 3. Set serialized.[[Algorithm]] to the sub-serialization of the [[algorithm]] internal slot of value.
    auto serialized_algorithm = TRY(HTML::structured_serialize_internal(vm, m_algorithm, for_storage, memory));
    serialized.extend(move(serialized_algorithm));

    // 4. Set serialized.[[Usages]] to the sub-serialization of the [[usages]] internal slot of value.
    auto serialized_usages = TRY(HTML::structured_serialize_internal(vm, m_usages, for_storage, memory));
    serialized.extend(move(serialized_usages));

    // FIXME: 5. Set serialized.[[Handle]] to the [[handle]] internal slot of value.

    return {};
}

WebIDL::ExceptionOr<void> CryptoKey::deserialization_steps(ReadonlySpan<u32> const& serialized, size_t& position, HTML::DeserializationMemory& memory)
{
    auto& vm = this->vm();
    auto& realm = this->realm();

    // 1. Initialize the [[type]] internal slot of value to serialized.[[Type]].
    m_type = static_cast<Bindings::KeyType>(HTML::deserialize_primitive_type<u32>(serialized, position));

    // 2. Initialize the [[extractable]] internal slot of value to serialized.[[Extractable]].
    m_extractable = HTML::deserialize_primitive_type<bool>(serialized, position);

    // 3. Initialize the [[algorithm]] internal slot of value to the sub-deserialization of serialized.[[Algorithm]].
    auto deserialized_record = TRY(HTML::structured_deserialize_internal(vm, serialized, realm, memory, position));
    if (deserialized_record.value.has_value())
        m_algorithm = deserialized_record.value.release_value().as_object();
    position = deserialized_record.position;

    // 4. Initialize the [[usages]] internal slot of value to the sub-deserialization of serialized.[[Usages]].
    deserialized_record = TRY(HTML::structured_deserialize_internal(vm, serialized, realm, memory, position));
    if (deserialized_record.value.has_value())
        m_usages = deserialized_record.value.release_value().as_object();
    position = deserialized_record.position;

    // FIXME: 5. Initialize the [[handle]] internal slot of value to serialized.[[Handle]].

    return {};
}

}
