/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ObservableArray.h>

namespace Web::DOM {

class Document;
class ShadowRoot;

struct AdoptedStyleSheetsAccess {
    static GC::Ref<WebIDL::ObservableArray> adopted_style_sheets(Document&);
    static GC::Ref<WebIDL::ObservableArray> adopted_style_sheets(ShadowRoot&);
};

WEB_API GC::Ref<WebIDL::ObservableArray> create_adopted_style_sheets_list(Node& document_or_shadow_root);
WEB_API void for_each_adopted_style_sheet(WebIDL::ObservableArray&, Function<void(CSS::CSSStyleSheet&)> const&);

}
