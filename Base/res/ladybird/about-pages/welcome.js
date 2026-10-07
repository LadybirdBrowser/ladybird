/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { populateSearchEngineSelect, updateSearchSuggestionsControl } from "./search-engine-controls.js";

const form = document.querySelector("#welcome-form");
const setupOptions = document.querySelector("#setup-options");
const startBrowsing = document.querySelector("#start-browsing");
const searchEngine = document.querySelector("#search-engine");
const searchSuggestions = document.querySelector("#search-suggestions-enabled");
const searchSuggestionsDescription = document.querySelector("#search-suggestions-description");
const enableContentBlocking = document.querySelector("#enable-content-blocking");
const blockingOptions = document.querySelector("#blocking-options");
const contentBlockerLists = document.querySelector("#content-blocker-lists");
const saveStatus = document.querySelector("#save-status");

let settings;
let engines;
let features;
let initialized = false;

const filterListDescriptions = {
    easyList: "Blocks most ads.",
    easyPrivacy: "Blocks trackers.",
    fanboyAnnoyances: "Blocks pop-ups and other annoyances.",
};

function updateBlockingOptions() {
    blockingOptions.classList.toggle("hidden", !enableContentBlocking.checked);
}

function initialize() {
    if (initialized || !settings || !engines || !features) return;
    initialized = true;

    document.querySelector("#tab-mode-section").classList.toggle("hidden", !features.verticalTabs);
    const tabMode = settings.tabs.verticalTabsEnabled ? "vertical" : "horizontal";
    document.querySelector(`input[name="tab-mode"][value="${tabMode}"]`).checked = true;

    populateSearchEngineSelect(searchEngine, engines.search, settings.searchEngine);
    searchSuggestions.checked = settings.searchEngine.suggestions;
    updateSearchSuggestionsControl(searchEngine, searchSuggestions, searchSuggestionsDescription);

    enableContentBlocking.checked = settings.contentBlockers.enabled;
    for (const list of settings.contentBlockerLists) {
        if (!Object.hasOwn(filterListDescriptions, list.identifier)) continue;

        const row = document.createElement("label");
        row.className = "filter-list";
        const checkbox = document.createElement("input");
        checkbox.type = "checkbox";
        checkbox.dataset.identifier = list.identifier;
        checkbox.checked = list.enabled;
        const name = document.createElement("span");
        name.textContent = list.name;
        const description = document.createElement("p");
        description.id = `description-${list.identifier}`;
        description.textContent = filterListDescriptions[list.identifier];
        checkbox.setAttribute("aria-describedby", description.id);
        row.append(checkbox, name, description);
        contentBlockerLists.append(row);
    }
    updateBlockingOptions();
    setupOptions.disabled = false;
    startBrowsing.disabled = false;
}

enableContentBlocking.addEventListener("change", updateBlockingOptions);
searchEngine.addEventListener("change", () => {
    updateSearchSuggestionsControl(searchEngine, searchSuggestions, searchSuggestionsDescription);
});

for (const input of document.querySelectorAll('input[name="tab-mode"]')) {
    input.addEventListener("change", () => {
        if (!initialized || !input.checked || !features.verticalTabs) return;
        ladybird.sendMessage("setTabSettings", {
            ...settings.tabs,
            verticalTabsEnabled: input.value === "vertical",
        });
    });
}

form.addEventListener("submit", event => {
    event.preventDefault();
    if (!initialized || startBrowsing.disabled) return;

    setupOptions.disabled = true;
    startBrowsing.disabled = true;
    saveStatus.textContent = "";
    ladybird.sendMessage("completeFirstRun", {
        verticalTabsEnabled: document.querySelector('input[name="tab-mode"]:checked').value === "vertical",
        searchEngine: { engine: searchEngine.value || null, suggestions: searchSuggestions.checked },
        contentBlockerEnabled: enableContentBlocking.checked,
        contentBlockerLists: [...contentBlockerLists.querySelectorAll("input")].map(checkbox => ({
            identifier: checkbox.dataset.identifier,
            enabled: checkbox.checked,
        })),
    });
});

document.addEventListener("WebUILoaded", () => {
    ladybird.sendMessage("loadFeatures");
    ladybird.sendMessage("loadAvailableEngines");
    ladybird.sendMessage("loadCurrentSettings");
});

document.addEventListener("WebUIMessage", event => {
    const { name, data } = event.detail;
    if (name === "loadSettings") settings = data;
    else if (name === "loadEngines") engines = data;
    else if (name === "loadFeatures") features = data;
    else if (name === "firstRunCompleted") location.replace(data);
    else if (name === "firstRunError") {
        saveStatus.textContent = data;
        setupOptions.disabled = false;
        startBrowsing.disabled = false;
    }
    initialize();
});
