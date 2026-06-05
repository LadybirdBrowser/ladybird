/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibSyntax/Document.h>

namespace Syntax {

class HighlighterClient {
public:
    virtual ~HighlighterClient() = default;

    virtual StringView highlighter_did_request_text() const = 0;
    virtual void highlighter_did_set_spans(Vector<TextDocumentSpan>) = 0;

    void do_set_spans(Vector<TextDocumentSpan> spans) { highlighter_did_set_spans(move(spans)); }

    StringView get_text() const { return highlighter_did_request_text(); }
};

}
