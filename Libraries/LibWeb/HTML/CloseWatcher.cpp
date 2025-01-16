/*
 * Copyright (c) 2024, the Ladybird developers.
 * Copyright (c) 2024, Felipe Muñoz Mazur <felipe.munoz.mazur@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibWeb/Bindings/CloseWatcherPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/EventDispatcher.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/HTML/CloseWatcher.h>
#include <LibWeb/HTML/CloseWatcherManager.h>
#include <LibWeb/HTML/EventHandler.h>
#include <LibWeb/HTML/HTMLIFrameElement.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(CloseWatcher);

// https://html.spec.whatwg.org/multipage/interaction.html#establish-a-close-watcher
GC::Ref<CloseWatcher> CloseWatcher::establish(HTML::Window& window)
{
    // 1. Assert: window's associated Document is fully active.
    VERIFY(window.associated_document().is_fully_active());

    // 2. Let closeWatcher be a new close watcher
    auto close_watcher = window.realm().create<CloseWatcher>(window.realm());

    // 3. Let manager be window's associated close watcher manager
    auto manager = window.close_watcher_manager();

    // 4 - 6. Moved to CloseWatcherManager::add
    manager->add(close_watcher);

    // 7. Return close_watcher.
    return close_watcher;
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-closewatcher
WebIDL::ExceptionOr<GC::Ref<CloseWatcher>> CloseWatcher::construct_impl(JS::Realm& realm, CloseWatcherOptions const& options)
{
    auto& window = as<HTML::Window>(realm.global_object());

    // NOTE: Not in spec explicitly, but this should account for detached iframes too. See /close-watcher/frame-removal.html WPT.
    auto navigable = window.navigable();
    if (navigable && navigable->has_been_destroyed())
        return WebIDL::InvalidStateError::create(realm, "The iframe has been detached"_string);

    // 1. If this's relevant global object's associated Document is not fully active, then return an "InvalidStateError" DOMException.
    if (!window.associated_document().is_fully_active())
        return WebIDL::InvalidStateError::create(realm, "The document is not fully active."_string);

    // 2. Let close_watcher be the result of establishing a close watcher
    auto close_watcher = establish(window);

    // 3. If options["signal"] exists, then:
    if (auto signal = options.signal) {
        // 3.1 If options["signal"]'s aborted, then destroy closeWatcher.
        if (signal->aborted()) {
            close_watcher->destroy();
        }

        // 3.2 Add the following steps to options["signal"]:
        signal->add_abort_algorithm([close_watcher] {
            // 3.2.1 Destroy closeWatcher.
            close_watcher->destroy();
        });
    }

    return close_watcher;
}

CloseWatcher::CloseWatcher(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-closewatcher-requestclose
void CloseWatcher::request_close_for_bindings()
{
    // The requestClose() method steps are to request to close this's internal close watcher with false.
    request_close(false);
}

// https://html.spec.whatwg.org/multipage/interaction.html#close-watcher-request-close
bool CloseWatcher::request_close(bool require_history_action_activation)
{
    // 1. If closeWatcher is not active, then return true.
    if (!m_is_active)
        return true;

    // 2. If the result of running closeWatcher's get enabled state is false, then return true.
    if (!get_enabled_state())
        return true;

    // 3. If closeWatcher's is running cancel action is true, then return true.
    if (m_is_running_cancel_action)
        return true;

    // 4. Let window be closeWatcher's window.
    auto& window = as<HTML::Window>(realm().global_object());

    // 5. If window's associated Document is not fully active, then return true.
    if (!window.associated_document().is_fully_active())
        return true;

    // 6. Let canPreventClose be true if requireHistoryActionActivation is false, or if window's close watcher manager's groups's size is less than window's close watcher manager's allowed number of groups,
    // and window has history-action activation; otherwise false.
    auto manager = window.close_watcher_manager();
    bool can_prevent_close = !require_history_action_activation || (manager->can_prevent_close() && window.has_history_action_activation());
    // 7. Set closeWatcher's is running cancel action to true.
    m_is_running_cancel_action = true;
    // 8. Let shouldContinue be the result of running closeWatcher's cancel action given canPreventClose.
    bool should_continue = dispatch_event(DOM::Event::create(realm(), HTML::EventNames::cancel, { .cancelable = can_prevent_close }));
    // 9. Set closeWatcher's is running cancel action to false.
    m_is_running_cancel_action = false;
    // 10. If shouldContinue is false, then:
    if (!should_continue) {
        // 10.1 Assert: canPreventClose is true.
        VERIFY(can_prevent_close);
        // 10.2 Consume history-action user activation given window.
        window.consume_history_action_user_activation();
        // 10.3 Return false.
        return false;
    }

    // 11. Close closeWatcher.
    close();

    // 12. Return true.
    return true;
}

// https://html.spec.whatwg.org/multipage/interaction.html#close-watcher-close
void CloseWatcher::close()
{
    // 1. If closeWatcher is not active, then return.
    if (!m_is_active)
        return;

    // 2. If the result of running closeWatcher's get enabled state is false, then return.
    if (!get_enabled_state())
        return;

    // 3. If closeWatcher's window's associated Document is not fully active, then return.
    if (!as<HTML::Window>(realm().global_object()).associated_document().is_fully_active())
        return;

    // 4. Destroy closeWatcher.
    destroy();

    // 5. Run closeWatcher's close action.
    dispatch_event(DOM::Event::create(realm(), HTML::EventNames::close));
}

// https://html.spec.whatwg.org/multipage/interaction.html#close-watcher-destroy
void CloseWatcher::destroy()
{
    // 1. Let manager be closeWatcher's window's close watcher manager.
    auto manager = as<HTML::Window>(realm().global_object()).close_watcher_manager();

    // 2-3. Moved to CloseWatcherManager::remove
    manager->remove(*this);

    m_is_active = false;
}

void CloseWatcher::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CloseWatcher);
}

void CloseWatcher::set_oncancel(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::cancel, event_handler);
}

WebIDL::CallbackType* CloseWatcher::oncancel()
{
    return event_handler_attribute(HTML::EventNames::cancel);
}

void CloseWatcher::set_onclose(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::close, event_handler);
}

WebIDL::CallbackType* CloseWatcher::onclose()
{
    return event_handler_attribute(HTML::EventNames::close);
}

}
