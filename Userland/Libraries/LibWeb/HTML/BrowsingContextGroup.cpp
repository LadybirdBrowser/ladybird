/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/BrowsingContextGroup.h>
#include <LibWeb/Page/Page.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(BrowsingContextGroup);

// https://html.spec.whatwg.org/multipage/browsers.html#browsing-context-group-set
static HashTable<JS::NonnullGCPtr<BrowsingContextGroup>>& user_agent_browsing_context_group_set()
{
    static HashTable<JS::NonnullGCPtr<BrowsingContextGroup>> set;
    return set;
}

BrowsingContextGroup::BrowsingContextGroup(JS::NonnullGCPtr<Web::Page> page)
    : m_page(page)
{
    user_agent_browsing_context_group_set().set(*this);
}

BrowsingContextGroup::~BrowsingContextGroup()
{
    user_agent_browsing_context_group_set().remove(*this);
}

void BrowsingContextGroup::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_page);
    visitor.visit(m_browsing_context_set);
}

// https://html.spec.whatwg.org/multipage/document-sequences.html#creating-a-new-browsing-context-group-and-document
auto BrowsingContextGroup::create_a_new_browsing_context_group_and_document(JS::NonnullGCPtr<Page> page) -> WebIDL::ExceptionOr<BrowsingContextGroupAndDocument>
{
    // 1. Let group be a new browsing context group.
    // 2. Append group to the user agent's browsing context group set.
    auto group = Bindings::main_thread_vm().heap().allocate_without_realm<BrowsingContextGroup>(page);

    // 3. Let browsingContext and document be the result of creating a new browsing context and document with null, null, and group.
    auto [browsing_context, document] = TRY(BrowsingContext::create_a_new_browsing_context_and_document(page, nullptr, nullptr, group));

    // 4. Append browsingContext to group.
    group->append(browsing_context);

    // 5. Return group and document.
    return BrowsingContextGroupAndDocument { group, document };
}

// https://html.spec.whatwg.org/multipage/browsers.html#bcg-append
void BrowsingContextGroup::append(BrowsingContext& browsing_context)
{
    VERIFY(browsing_context.is_top_level());

    // 1. Append browsingContext to group's browsing context set.
    m_browsing_context_set.set(browsing_context);

    // 2. Set browsingContext's group to group.
    browsing_context.set_group(this);
}

}
