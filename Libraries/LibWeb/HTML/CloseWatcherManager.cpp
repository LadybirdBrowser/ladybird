/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/EventDispatcher.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/HTML/CloseWatcher.h>
#include <LibWeb/HTML/CloseWatcherManager.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(CloseWatcherManager);

JS::NonnullGCPtr<CloseWatcherManager> CloseWatcherManager::create(JS::Realm& realm)
{
    return realm.heap().allocate<CloseWatcherManager>(realm, realm);
}

CloseWatcherManager::CloseWatcherManager(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void CloseWatcherManager::add(JS::NonnullGCPtr<CloseWatcher> close_watcher)
{
    // If manager's groups's size is less than manager's allowed number of groups
    if (m_groups.size() < m_allowed_number_of_groups) {
        // then append « closeWatcher » to manager's groups.
        JS::MarkedVector<JS::NonnullGCPtr<CloseWatcher>> new_group(realm().heap());
        new_group.append(close_watcher);
        m_groups.append(move(new_group));
    } else {
        // Assert: manager's groups's size is at least 1 in this branch, since manager's allowed number of groups is always at least 1.
        VERIFY(!m_groups.is_empty());
        // Append closeWatcher to manager's groups's last item.
        m_groups.last().append(close_watcher);
    }

    // Set manager's next user interaction allows a new group to true.
    m_next_user_interaction_allows_a_new_group = true;
}

void CloseWatcherManager::remove(CloseWatcher const& close_watcher)
{
    // 2. For each group of manager's groups: remove closeWatcher from group
    for (auto& group : m_groups) {
        group.remove_first_matching([&close_watcher](JS::NonnullGCPtr<CloseWatcher>& entry) {
            return entry.ptr() == &close_watcher;
        });
    }
    // 3. Remove any item from manager's group that is empty
    m_groups.remove_all_matching([](auto& group) { return group.is_empty(); });
}

// https://html.spec.whatwg.org/multipage/interaction.html#process-close-watchers
bool CloseWatcherManager::process_close_watchers()
{
    // 1. Let processedACloseWatcher be false.
    bool processed_a_close_watcher = false;
    // 2. If window's close watcher manager's groups is not empty:
    if (!m_groups.is_empty()) {
        // 2.1 Let group be the last item in window's close watcher manager's groups.
        auto& group = m_groups.last();
        // Ambiguous spec wording. We copy the groups to avoid modifying the original while iterating.
        // See https://github.com/whatwg/html/issues/10240
        JS::MarkedVector<JS::NonnullGCPtr<CloseWatcher>> group_copy(realm().heap());
        group_copy.ensure_capacity(group.size());
        for (auto& close_watcher : group) {
            group_copy.append(close_watcher);
        }
        // 2.2 For each closeWatcher of group, in reverse order:
        for (auto it = group_copy.rbegin(); it != group_copy.rend(); ++it) {
            // 2.1.1 Set processedACloseWatcher to true.
            processed_a_close_watcher = true;
            // 2.1.2 Let shouldProceed be the result of requesting to close closeWatcher.
            bool should_proceed = (*it)->request_close();
            // 2.1.3 If shouldProceed is false, then break;
            if (!should_proceed)
                break;
        }
    }
    // 3. If window's close watcher manager's allowed number of groups is greater than 1, decrement it by 1.
    if (m_allowed_number_of_groups > 1)
        m_allowed_number_of_groups--;

    // 4. Return processedACloseWatcher.
    return processed_a_close_watcher;
}

// https://html.spec.whatwg.org/multipage/interaction.html#notify-the-close-watcher-manager-about-user-activation
void CloseWatcherManager::notify_about_user_activation()
{
    // 1. Let manager be window's close watcher manager.
    // 2. If manager's next user interaction allows a new group is true, then increment manager's allowed number of groups.
    if (m_next_user_interaction_allows_a_new_group)
        m_allowed_number_of_groups++;
    // 3. Set manager's next user interaction allows a new group to false.
    m_next_user_interaction_allows_a_new_group = false;
}

// https://html.spec.whatwg.org/multipage/interaction.html#close-watcher-request-close
bool CloseWatcherManager::can_prevent_close()
{
    // 5. Let canPreventClose be true if window's close watcher manager's groups's size is less than window's close watcher manager's allowed number of groups...
    return m_groups.size() < m_allowed_number_of_groups;
}

void CloseWatcherManager::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_groups);
}

}
