/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/BigIntObject.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

GC::Ref<BigIntObject> BigIntObject::create(Realm& realm, BigInt& bigint)
{
    return static_cast<BigIntObject&>(object_from_abi(js_primitive_wrapper_create_bigint(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), bigint_to_abi(bigint))));
}

BigInt const& BigIntObject::bigint() const
{
    return const_cast<BigIntObject&>(*this).bigint();
}

BigInt& BigIntObject::bigint()
{
    auto* bigint = js_primitive_wrapper_bigint(object_to_abi(*this));
    VERIFY(bigint);
    return cell_ref_from_abi<BigInt>(bigint);
}

}
