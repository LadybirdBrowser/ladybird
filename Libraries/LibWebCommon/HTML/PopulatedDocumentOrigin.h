/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::HTML {

// Whose origin a document a process populates has, when it is not its navigation params'.
enum class PopulatedDocumentOrigin : u8 {
    InlineContent,
    PdfViewer,
};

}
