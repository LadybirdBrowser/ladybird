/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ObservableArray.h>

namespace Web::DOM {

JS::NonnullGCPtr<WebIDL::ObservableArray> create_adopted_style_sheets_list(Document& document);

}
