/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

const form = document.querySelector("#welcome-form");
const setupOptions = document.querySelector("#setup-options");
const startBrowsing = document.querySelector("#start-browsing");
const searchEngines = document.querySelector("#search-engines");
const enableContentBlocking = document.querySelector("#enable-content-blocking");
const saveStatus = document.querySelector("#save-status");

let settings;
let engines;
let features;
let initialized = false;

function setSearchEngine(name) {
    ladybird.sendMessage("setSearchEngineSettings", {
        engine: name,
        suggestions: settings.searchEngine.suggestions,
    });
}

// The search engines are shown in a new random order each time, so that none is favored by its place in the list.
function shuffled(items) {
    const result = [...items];
    for (let i = result.length - 1; i > 0; --i) {
        const j = Math.floor(Math.random() * (i + 1));
        [result[i], result[j]] = [result[j], result[i]];
    }
    return result;
}

function renderSearchEngines() {
    const names = shuffled(engines.search.map(engine => engine.name));

    searchEngines.replaceChildren();
    for (const name of names) {
        const tile = document.createElement("label");
        tile.className = "search-engine";

        const radio = document.createElement("input");
        radio.type = "radio";
        radio.name = "search-engine";
        radio.value = name;
        radio.checked = settings.searchEngine.engine === name;
        radio.addEventListener("change", () => {
            if (!initialized) return;
            setSearchEngine(name);
        });

        // A search engine is optional, so choosing the chosen one again goes without one.
        let wasChecked = false;
        tile.addEventListener("pointerdown", () => {
            wasChecked = radio.checked;
        });
        tile.addEventListener("click", event => {
            if (!initialized || !wasChecked || event.target !== radio) return;
            wasChecked = false;
            radio.checked = false;
            setSearchEngine(null);
        });

        const letter = document.createElement("span");
        letter.className = "letter-tile";
        letter.setAttribute("aria-hidden", "true");
        letter.textContent = name.charAt(0).toUpperCase();

        const label = document.createElement("span");
        label.textContent = name;

        tile.append(radio, letter, label);
        searchEngines.append(tile);
    }
}

function initialize() {
    if (initialized || !settings || !engines || !features) return;
    initialized = true;

    document.querySelector("#tab-mode-section").classList.toggle("hidden", !features.verticalTabs);
    const tabMode = settings.tabs.verticalTabsEnabled ? "vertical" : "horizontal";
    document.querySelector(`input[name="tab-mode"][value="${tabMode}"]`).checked = true;

    renderSearchEngines();

    enableContentBlocking.checked = settings.contentBlockers.enabled;
    setupOptions.disabled = false;
    startBrowsing.disabled = false;
}

for (const input of document.querySelectorAll('input[name="tab-mode"]')) {
    input.addEventListener("change", () => {
        if (!initialized || !input.checked || !features.verticalTabs) return;
        ladybird.sendMessage("setTabSettings", {
            ...settings.tabs,
            verticalTabsEnabled: input.value === "vertical",
        });
    });
}

enableContentBlocking.addEventListener("change", () => {
    if (!initialized) return;

    ladybird.sendMessage("setContentBlockerEnabled", enableContentBlocking.checked);
    ladybird.sendMessage("setFilterListUpdatesEnabled", enableContentBlocking.checked);
});

form.addEventListener("submit", event => {
    event.preventDefault();
    if (!initialized || startBrowsing.disabled) return;

    setupOptions.disabled = true;
    startBrowsing.disabled = true;
    saveStatus.textContent = "";
    ladybird.sendMessage("completeFirstRun");
});

document.addEventListener("WebUILoaded", () => {
    ladybird.sendMessage("loadFeatures");
    ladybird.sendMessage("loadAvailableEngines");
    ladybird.sendMessage("loadCurrentSettings");
});

document.addEventListener("WebUIMessage", event => {
    const { name, data } = event.detail;

    if (name === "loadSettings") {
        settings = { ...settings, ...data };

        if (initialized) {
            if (Object.hasOwn(data, "tabs")) {
                const mode = settings.tabs.verticalTabsEnabled ? "vertical" : "horizontal";
                document.querySelector(`input[name="tab-mode"][value="${mode}"]`).checked = true;
            }
            if (Object.hasOwn(data, "searchEngine")) {
                for (const radio of searchEngines.querySelectorAll("input"))
                    radio.checked = radio.value === settings.searchEngine.engine;
            }
            if (Object.hasOwn(data, "contentBlockers"))
                enableContentBlocking.checked = settings.contentBlockers.enabled;
        }
    } else if (name === "loadEngines") {
        engines = data;
    } else if (name === "loadFeatures") {
        features = data;
    } else if (name === "firstRunCompleted") {
        location.replace(data);
    } else if (name === "firstRunError") {
        saveStatus.textContent = data;
        setupOptions.disabled = false;
        startBrowsing.disabled = false;
    }

    initialize();
});
