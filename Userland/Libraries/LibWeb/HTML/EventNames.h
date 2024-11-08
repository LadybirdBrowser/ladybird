/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/FlyString.h>

namespace Web::HTML::EventNames {

// FIXME: Add app cache events https://html.spec.whatwg.org/multipage/offline.html#appcacheevents
// FIXME: Add drag and drop events https://html.spec.whatwg.org/multipage/dnd.html#dndevents

#define ENUMERATE_HTML_EVENTS                        \
    __ENUMERATE_HTML_EVENT(abort)                    \
    __ENUMERATE_HTML_EVENT(addtrack)                 \
    __ENUMERATE_HTML_EVENT(animationcancel)          \
    __ENUMERATE_HTML_EVENT(animationend)             \
    __ENUMERATE_HTML_EVENT(animationiteration)       \
    __ENUMERATE_HTML_EVENT(animationstart)           \
    __ENUMERATE_HTML_EVENT(afterprint)               \
    __ENUMERATE_HTML_EVENT(beforeprint)              \
    __ENUMERATE_HTML_EVENT(beforetoggle)             \
    __ENUMERATE_HTML_EVENT(beforeunload)             \
    __ENUMERATE_HTML_EVENT(blocked)                  \
    __ENUMERATE_HTML_EVENT(blur)                     \
    __ENUMERATE_HTML_EVENT(cancel)                   \
    __ENUMERATE_HTML_EVENT(canplay)                  \
    __ENUMERATE_HTML_EVENT(canplaythrough)           \
    __ENUMERATE_HTML_EVENT(change)                   \
    __ENUMERATE_HTML_EVENT(click)                    \
    __ENUMERATE_HTML_EVENT(close)                    \
    __ENUMERATE_HTML_EVENT(complete)                 \
    __ENUMERATE_HTML_EVENT(connect)                  \
    __ENUMERATE_HTML_EVENT(controllerchange)         \
    __ENUMERATE_HTML_EVENT(contextmenu)              \
    __ENUMERATE_HTML_EVENT(copy)                     \
    __ENUMERATE_HTML_EVENT(cuechange)                \
    __ENUMERATE_HTML_EVENT(currententrychange)       \
    __ENUMERATE_HTML_EVENT(dispose)                  \
    __ENUMERATE_HTML_EVENT(cut)                      \
    __ENUMERATE_HTML_EVENT(DOMContentLoaded)         \
    __ENUMERATE_HTML_EVENT(drag)                     \
    __ENUMERATE_HTML_EVENT(dragend)                  \
    __ENUMERATE_HTML_EVENT(dragenter)                \
    __ENUMERATE_HTML_EVENT(dragleave)                \
    __ENUMERATE_HTML_EVENT(dragover)                 \
    __ENUMERATE_HTML_EVENT(dragstart)                \
    __ENUMERATE_HTML_EVENT(drop)                     \
    __ENUMERATE_HTML_EVENT(durationchange)           \
    __ENUMERATE_HTML_EVENT(emptied)                  \
    __ENUMERATE_HTML_EVENT(ended)                    \
    __ENUMERATE_HTML_EVENT(enter)                    \
    __ENUMERATE_HTML_EVENT(error)                    \
    __ENUMERATE_HTML_EVENT(exit)                     \
    __ENUMERATE_HTML_EVENT(finish)                   \
    __ENUMERATE_HTML_EVENT(focus)                    \
    __ENUMERATE_HTML_EVENT(focusin)                  \
    __ENUMERATE_HTML_EVENT(focusout)                 \
    __ENUMERATE_HTML_EVENT(formdata)                 \
    __ENUMERATE_HTML_EVENT(hashchange)               \
    __ENUMERATE_HTML_EVENT(input)                    \
    __ENUMERATE_HTML_EVENT(invalid)                  \
    __ENUMERATE_HTML_EVENT(languagechange)           \
    __ENUMERATE_HTML_EVENT(load)                     \
    __ENUMERATE_HTML_EVENT(loaded)                   \
    __ENUMERATE_HTML_EVENT(loadend)                  \
    __ENUMERATE_HTML_EVENT(loadeddata)               \
    __ENUMERATE_HTML_EVENT(loadedmetadata)           \
    __ENUMERATE_HTML_EVENT(loading)                  \
    __ENUMERATE_HTML_EVENT(loadingdone)              \
    __ENUMERATE_HTML_EVENT(loadingerror)             \
    __ENUMERATE_HTML_EVENT(loadstart)                \
    __ENUMERATE_HTML_EVENT(message)                  \
    __ENUMERATE_HTML_EVENT(messageerror)             \
    __ENUMERATE_HTML_EVENT(navigate)                 \
    __ENUMERATE_HTML_EVENT(navigatesuccess)          \
    __ENUMERATE_HTML_EVENT(navigateerror)            \
    __ENUMERATE_HTML_EVENT(offline)                  \
    __ENUMERATE_HTML_EVENT(online)                   \
    __ENUMERATE_HTML_EVENT(open)                     \
    __ENUMERATE_HTML_EVENT(pagehide)                 \
    __ENUMERATE_HTML_EVENT(pageshow)                 \
    __ENUMERATE_HTML_EVENT(paste)                    \
    __ENUMERATE_HTML_EVENT(pause)                    \
    __ENUMERATE_HTML_EVENT(play)                     \
    __ENUMERATE_HTML_EVENT(playing)                  \
    __ENUMERATE_HTML_EVENT(popstate)                 \
    __ENUMERATE_HTML_EVENT(progress)                 \
    __ENUMERATE_HTML_EVENT(ratechange)               \
    __ENUMERATE_HTML_EVENT(readystatechange)         \
    __ENUMERATE_HTML_EVENT(rejectionhandled)         \
    __ENUMERATE_HTML_EVENT(remove)                   \
    __ENUMERATE_HTML_EVENT(removetrack)              \
    __ENUMERATE_HTML_EVENT(reset)                    \
    __ENUMERATE_HTML_EVENT(resize)                   \
    __ENUMERATE_HTML_EVENT(scroll)                   \
    __ENUMERATE_HTML_EVENT(scrollend)                \
    __ENUMERATE_HTML_EVENT(securitypolicyviolation)  \
    __ENUMERATE_HTML_EVENT(selectionchange)          \
    __ENUMERATE_HTML_EVENT(seeked)                   \
    __ENUMERATE_HTML_EVENT(seeking)                  \
    __ENUMERATE_HTML_EVENT(select)                   \
    __ENUMERATE_HTML_EVENT(slotchange)               \
    __ENUMERATE_HTML_EVENT(stalled)                  \
    __ENUMERATE_HTML_EVENT(statechange)              \
    __ENUMERATE_HTML_EVENT(storage)                  \
    __ENUMERATE_HTML_EVENT(submit)                   \
    __ENUMERATE_HTML_EVENT(success)                  \
    __ENUMERATE_HTML_EVENT(suspend)                  \
    __ENUMERATE_HTML_EVENT(timeupdate)               \
    __ENUMERATE_HTML_EVENT(toggle)                   \
    __ENUMERATE_HTML_EVENT(transitionend)            \
    __ENUMERATE_HTML_EVENT(unhandledrejection)       \
    __ENUMERATE_HTML_EVENT(unload)                   \
    __ENUMERATE_HTML_EVENT(upgradeneeded)            \
    __ENUMERATE_HTML_EVENT(visibilitychange)         \
    __ENUMERATE_HTML_EVENT(volumechange)             \
    __ENUMERATE_HTML_EVENT(waiting)                  \
    __ENUMERATE_HTML_EVENT(webkitAnimationEnd)       \
    __ENUMERATE_HTML_EVENT(webkitAnimationIteration) \
    __ENUMERATE_HTML_EVENT(webkitAnimationStart)     \
    __ENUMERATE_HTML_EVENT(webkitTransitionEnd)

#define __ENUMERATE_HTML_EVENT(name) extern FlyString name;
ENUMERATE_HTML_EVENTS
#undef __ENUMERATE_HTML_EVENT

void initialize_strings();

}
