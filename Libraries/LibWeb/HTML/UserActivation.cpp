/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/UserActivation.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(UserActivation);

WebIDL::ExceptionOr<GC::Ref<UserActivation>> UserActivation::construct_impl(JS::Realm& realm)
{
    return realm.create<UserActivation>(realm);
}

UserActivation::UserActivation(JS::Realm& realm)
    : Wrappable(realm)
{
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-useractivation-hasbeenactive
bool UserActivation::has_been_active() const
{
    // The hasBeenActive getter steps are to return true if this's relevant global object has sticky activation, and false otherwise.
    return relevant_window(*this).has_sticky_activation();
}

// https://html.spec.whatwg.org/multipage/interaction.html#dom-useractivation-isactive
bool UserActivation::is_active() const
{
    // The isActive getter steps are to return true if this's relevant global object has transient activation, and false otherwise.
    return relevant_window(*this).has_transient_activation();
}

}
