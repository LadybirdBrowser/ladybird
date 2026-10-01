/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

export function populateSearchEngineSelect(select, engines, settings = {}) {
    const disabledOption = select.options[0];
    select.replaceChildren(disabledOption, document.createElement("hr"));

    function addEngine(engine) {
        const option = document.createElement("option");
        option.value = option.textContent = engine.name;
        option.dataset.supportsSuggestions = Boolean(engine.supportsSuggestions || engine.suggestionsUrl);
        select.append(option);
    }

    engines.forEach(addEngine);
    if (settings.custom?.length) {
        select.append(document.createElement("hr"));
        settings.custom.forEach(addEngine);
    }
    select.value = settings.engine || "";
}

export function updateSearchSuggestionsControl(select, checkbox, description) {
    const supportsSuggestions = select.selectedOptions[0]?.dataset.supportsSuggestions === "true";
    checkbox.disabled = !supportsSuggestions;
    if (!description) return;
    if (!select.value) description.textContent = "Choose a search engine to use search suggestions.";
    else if (!supportsSuggestions) description.textContent = `Search suggestions aren't available for ${select.value}.`;
    else description.textContent = `Sends what you type to ${select.value}.`;
}
