/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Function.h>
#include <LibJS/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/NavigationParams.h>
#include <LibWebCommon/HTML/NavigationParamsDescriptor.h>

namespace Web::HTML {

using NavigationParamsDescriptorCompletion = GC::Function<void(NavigationParamsVariantDescriptor)>;

WEB_API void create_navigation_params_descriptor(JS::Realm&, NavigationParamsVariant, GC::Ref<NavigationParamsDescriptorCompletion>);
WEB_API ErrorOr<NavigationParamsVariant> create_navigation_params_from_descriptor(JS::Realm&, LocalNavigable&, NavigationParamsVariantDescriptor);

}
