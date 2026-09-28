/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWebCommon/HTML/SameDocumentNavigationEntry.h>

namespace Web::HTML {

WEB_API SameDocumentNavigationEntry create_same_document_navigation_entry(SessionHistoryEntry const&);

}
