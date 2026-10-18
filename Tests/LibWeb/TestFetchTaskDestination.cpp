/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "FetchTestDocument.h"
#include <LibCore/EventLoop.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Fetch/Infrastructure/Task.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>

using Web::Fetch::Infrastructure::TaskDestination;

TEST_CASE(a_document_override_takes_precedence_over_a_shared_global)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    auto document = create_test_document();
    auto& window = *document->window();
    auto other_document = Web::DOM::Document::create(document->page(), window);
    auto& global = Web::HTML::relevant_global_object(*document);

    TaskDestination for_the_global { GC::Ref { global } };
    EXPECT(for_the_global.is_for_document(*document));
    EXPECT(for_the_global.is_for_document(*other_document));

    TaskDestination for_the_other_document { GC::Ref { global } };
    for_the_other_document.document_override = other_document;
    EXPECT(for_the_other_document.is_for_document(*other_document));
    EXPECT(!for_the_other_document.is_for_document(*document));

    TaskDestination for_the_document { GC::Ref { global } };
    for_the_document.document_override = document;
    EXPECT(for_the_document.is_for_document(*document));
    EXPECT(!for_the_document.is_for_document(*other_document));
}

TEST_CASE(a_parallel_queue_is_for_no_document)
{
    Core::EventLoop event_loop;
    install_font_plugin();
    auto document = create_test_document();

    TaskDestination parallel_queue { Web::HTML::ParallelQueue::create() };
    EXPECT(!parallel_queue.is_for_document(*document));
}
