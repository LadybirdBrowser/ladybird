/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/TrustedTypes/TrustedHTMLParserOptions.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>

namespace Web::Bindings {

inline WebIDL::ExceptionOr<GC::Ref<TrustedTypes::TrustedHTMLParserOptions>> create_parser_options(JS::Realm& realm, TrustedTypes::TrustedTypePolicy& policy, SetHTMLUnsafeOptions const& options, GC::RootVector<JS::Value> const& arguments)
{
    return policy.create_parser_options(realm.global_object(), options, arguments);
}

}
