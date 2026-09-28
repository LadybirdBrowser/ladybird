/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebDriver/UserPrompt.h>

namespace Web::WebDriver {

// https://w3c.github.io/webdriver/#dfn-handle-any-user-prompts
void handle_any_user_prompts(Page& page, GC::Ref<GC::Function<void(Optional<Error>)>> on_complete)
{
    // 1. If the current browsing context is not blocked by a dialog return success.
    if (!page.has_pending_dialog()) {
        on_complete->function()({});
        return;
    }

    // 2. Let type be "default".
    auto type = PromptType::Default;

    // 3. If the current user prompt is an alert dialog, set type to "alert". Otherwise, if the current user prompt is a
    //    beforeunload dialog, set type to "beforeUnload". Otherwise, if the current user prompt is a confirm dialog,
    //    set type to "confirm". Otherwise, if the current user prompt is a prompt dialog, set type to "prompt".
    // FIXME: Handle beforeunload dialogs when they are implemented.
    switch (page.pending_dialog()) {
    case Page::PendingDialog::Alert:
        type = PromptType::Alert;
        break;
    case Page::PendingDialog::Confirm:
        type = PromptType::Confirm;
        break;
    case Page::PendingDialog::Prompt:
        type = PromptType::Prompt;
        break;
    case Page::PendingDialog::None:
        VERIFY_NOT_REACHED();
    }

    // 4. Let handler be get the prompt handler with type.
    auto handler = get_the_prompt_handler(type);

    auto on_dialog_closed = GC::create_function(page.heap(), [notify = handler.notify, pending_dialog_text = page.pending_dialog_text(), on_complete]() {
        // 5. If handler's notify is true, return annotated unexpected alert open error.
        if (notify == PromptHandlerConfiguration::Notify::Yes) {
            on_complete->function()(create_annotated_unexpected_alert_open_error(pending_dialog_text));
            return;
        }

        // 6. Return success.
        on_complete->function()({});
    });

    // 4. Perform the following substeps based on handler's handler:
    switch (handler.handler) {
    // -> "accept"
    case PromptHandler::Accept:
        // Accept the current user prompt.
        page.accept_dialog(on_dialog_closed);
        break;
    // -> "dismiss"
    case PromptHandler::Dismiss:
        // Dismiss the current user prompt.
        page.dismiss_dialog(on_dialog_closed);
        break;
    // -> "ignore"
    case PromptHandler::Ignore:
        // Do nothing.
        on_dialog_closed->function()();
        break;
    }
}

}
