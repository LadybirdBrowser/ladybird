/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

const form = document.querySelector("#welcome-form");
const setupOptions = document.querySelector("#setup-options");
const startBrowsing = document.querySelector("#start-browsing");
const searchEngines = document.querySelector("#search-engines");
const searchSuggestions = document.querySelector("#search-suggestions");
const searchSuggestionsEnabled = document.querySelector("#search-suggestions-enabled");
const searchSuggestionsDescription = document.querySelector("#search-suggestions-description");
const tabLayoutInputs = document.querySelectorAll('input[name="tab-layout"]');
const colorThemeInputs = document.querySelectorAll('input[name="color-theme"]');
const enableContentBlocking = document.querySelector("#enable-content-blocking");
const enableUpdateChecks = document.querySelector("#enable-update-checks");
const defaultBrowser = document.querySelector("#default-browser");
const defaultBrowserLabel = document.querySelector("#default-browser-label");
const defaultBrowserDescription = document.querySelector("#default-browser-description");
const makeDefaultBrowser = document.querySelector("#make-default-browser");
const saveStatus = document.querySelector("#save-status");

let settings;
let engines;
let features;
let initialized = false;
// Why the last attempt to make Ladybird the default browser failed, which is shown in place of the row's description.
let defaultBrowserFailure = null;

function setSearchEngineSettings(name, suggestions) {
    // NB: Keep our copy current, as the settings update for this change can arrive after the next one is made.
    settings.searchEngine = { ...settings.searchEngine, engine: name, suggestions };
    ladybird.sendMessage("setSearchEngineSettings", { engine: name, suggestions });
    updateSearchSuggestions();
}

function setSearchEngine(name) {
    setSearchEngineSettings(name, name !== null || settings.searchEngine.suggestions);
}

// The suggestions switch is only shown for a chosen engine that offers suggestions, and says where typed text goes.
function updateSearchSuggestions() {
    const name = settings.searchEngine.engine;
    const engine = engines.search.find(engine => engine.name === name);
    searchSuggestions.classList.toggle("hidden", !engine?.supportsSuggestions);
    searchSuggestionsEnabled.checked = settings.searchEngine.suggestions;
    if (engine)
        searchSuggestionsDescription.textContent = `Your typing in the address bar is sent to ${name} before you press Enter.`;
}

function renderSearchEngines() {
    const names = engines.search.map(engine => engine.name).sort((a, b) => a.localeCompare(b));

    searchEngines.replaceChildren();
    for (const name of names) {
        const tile = document.createElement("label");
        tile.className = "search-engine choice-card";

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

        // NB: Engines are shown by name only. Logos would mean shipping other companies' trademarks, or contacting
        //     their servers before anyone has chosen.
        const label = document.createElement("span");
        label.textContent = name;

        tile.append(radio, label);
        searchEngines.append(tile);
    }
}

function tabLayout() {
    if (!settings.tabs.verticalTabsEnabled) return "horizontal";
    return settings.tabs.everythingInSidebar ? "sidebar" : "vertical";
}

function updateTabLayout() {
    const layout = tabLayout();
    for (const input of tabLayoutInputs) input.checked = input.value === layout;
}

function updateColorTheme() {
    const colorTheme = settings.appearance.colorTheme || "neutral";
    for (const input of colorThemeInputs) input.checked = input.value === colorTheme;
}

// The row is only shown where the system can tell which browser is the default, and lets Ladybird become it.
function updateDefaultBrowser() {
    const isDefault = settings.defaultBrowser === "default";
    const canMakeDefault = settings.defaultBrowser === "not-default";

    defaultBrowser.classList.toggle("hidden", !isDefault && !canMakeDefault);
    defaultBrowserDescription.textContent =
        defaultBrowserFailure ??
        (isDefault ? "Links from other apps open in Ladybird." : "Open links from other apps in Ladybird.");
    makeDefaultBrowser.classList.toggle("hidden", !canMakeDefault);

    // A control row is filled under the pointer, and a click anywhere on it reaches the control its label names. This
    // row is one only while there is something for the button to do.
    defaultBrowser.classList.toggle("control-row", canMakeDefault);
    if (canMakeDefault) defaultBrowserLabel.htmlFor = makeDefaultBrowser.id;
    else defaultBrowserLabel.removeAttribute("for");
}

function initialize() {
    if (initialized || !settings || !engines || !features) return;
    initialized = true;

    document.querySelector("#tab-layout-section").classList.toggle("hidden", !features.verticalTabs);
    updateTabLayout();
    updateColorTheme();
    updateDefaultBrowser();

    renderSearchEngines();
    updateSearchSuggestions();

    enableContentBlocking.checked = settings.contentBlockers.enabled;
    enableUpdateChecks.checked = settings.backgroundNetworking.features.updateChecks;
    setupOptions.disabled = false;
    startBrowsing.disabled = false;
}

for (const input of tabLayoutInputs) {
    input.addEventListener("change", () => {
        if (!initialized || !input.checked || !features.verticalTabs) return;
        // NB: Keep our copy current, as the settings update for this change can arrive after the next one is made.
        settings.tabs = {
            ...settings.tabs,
            verticalTabsEnabled: input.value !== "horizontal",
            everythingInSidebar: input.value === "sidebar",
        };
        ladybird.sendMessage("setTabSettings", settings.tabs);
    });
}

// Picking a color recolors the window around the page at once.
for (const input of colorThemeInputs) {
    input.addEventListener("change", () => {
        if (!initialized || !input.checked) return;
        // NB: Keep our copy current, as the settings update for this change can arrive after the next one is made.
        settings.appearance = { ...settings.appearance, colorTheme: input.value };
        ladybird.sendMessage("setAppearance", settings.appearance);
    });
}

searchSuggestionsEnabled.addEventListener("change", () => {
    if (!initialized) return;
    setSearchEngineSettings(settings.searchEngine.engine, searchSuggestionsEnabled.checked);
});

enableContentBlocking.addEventListener("change", () => {
    if (!initialized) return;

    // Enable the lists before the content blocker, which then downloads every enabled list.
    if (enableContentBlocking.checked) {
        for (const identifier of ["easyList", "easyPrivacy"])
            ladybird.sendMessage("setContentBlockerListEnabled", { identifier, enabled: true });
    }

    ladybird.sendMessage("setContentBlockerEnabled", enableContentBlocking.checked);
    ladybird.sendMessage("setFilterListUpdatesEnabled", enableContentBlocking.checked);
});

enableUpdateChecks.addEventListener("change", () => {
    if (!initialized) return;
    ladybird.sendMessage("setUpdateChecksEnabled", enableUpdateChecks.checked);
});

// The system may ask before it changes the default browser, so the button waits for its answer.
makeDefaultBrowser.addEventListener("click", () => {
    if (!initialized) return;

    makeDefaultBrowser.disabled = true;
    defaultBrowserFailure = null;
    updateDefaultBrowser();

    ladybird.sendMessage("makeDefaultBrowser");
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
            if (Object.hasOwn(data, "tabs")) updateTabLayout();
            if (Object.hasOwn(data, "appearance")) updateColorTheme();
            if (Object.hasOwn(data, "searchEngine")) {
                for (const radio of searchEngines.querySelectorAll("input"))
                    radio.checked = radio.value === settings.searchEngine.engine;
                updateSearchSuggestions();
            }
            if (Object.hasOwn(data, "contentBlockers"))
                enableContentBlocking.checked = settings.contentBlockers.enabled;
            if (Object.hasOwn(data, "backgroundNetworking"))
                enableUpdateChecks.checked = settings.backgroundNetworking.features.updateChecks;
            if (Object.hasOwn(data, "defaultBrowser")) {
                defaultBrowserFailure = null;
                updateDefaultBrowser();
            }
        }
    } else if (name === "loadEngines") {
        engines = data;
    } else if (name === "loadFeatures") {
        features = data;
    } else if (name === "defaultBrowserResult") {
        makeDefaultBrowser.disabled = false;
        defaultBrowserFailure = data;
        updateDefaultBrowser();
    } else if (name === "firstRunCompleted") {
        location.replace(data);
    } else if (name === "firstRunError") {
        saveStatus.textContent = data;
        setupOptions.disabled = false;
        startBrowsing.disabled = false;
    }

    initialize();
});
