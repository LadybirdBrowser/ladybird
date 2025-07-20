/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ObservableArray.h>

namespace Web::DOM {

WEB_API GC::Ref<WebIDL::ObservableArray> create_adopted_style_sheets_list(Node& document_or_shadow_root);

}
