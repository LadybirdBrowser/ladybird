/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>

namespace Web::Editing {

class InsertedContent;

struct ParagraphBoundaryState {
    bool selection_start_was_start_of_paragraph { false };
    bool selection_end_was_end_of_paragraph { false };
};

struct ParagraphIntegrationDecision {
    bool merge_start { false };
    bool merge_end { false };
};

// Decide how an inserted paragraph range joins the surrounding document after insertion. This deliberately consumes
// rendered positions derived from the mutation-tracked range: wrapper cleanup and list integration may have changed
// the DOM representation, while the logical first and last inserted paragraphs remain stable.
class ParagraphIntegration {
public:
    ParagraphIntegration(DOM::Document&, InsertedContent const&, ParagraphBoundaryState);

    ParagraphIntegrationDecision decide() const;

private:
    bool should_merge_start() const;
    bool should_merge_end() const;

    GC::Ref<DOM::Document> m_document;
    InsertedContent const& m_inserted_content;
    ParagraphBoundaryState m_boundary_state;
};

}
