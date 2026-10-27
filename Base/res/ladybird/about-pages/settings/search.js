import { registerDialogDeepLink } from "./dialog-deep-link.js";

const searchClose = document.querySelector("#search-close");
const searchCustomAdd = document.querySelector("#search-custom-add");
const searchCustomName = document.querySelector("#search-custom-name");
const searchCustomURL = document.querySelector("#search-custom-url");
const searchCustomSuggestionsURL = document.querySelector("#search-custom-suggestions-url");
const searchDialog = document.querySelector("#search-dialog");
const searchEngine = document.querySelector("#search-engine");
const searchList = document.querySelector("#search-list");
const searchSettings = document.querySelector("#search-settings");

const searchSuggestionsEnabled = document.querySelector("#search-suggestions-enabled");
const searchSuggestionsDescription = document.querySelector("#search-suggestions-description");

let SEARCH_ENGINE = {};
let ENGINES = [];

function selectedEngine() {
    return searchEngine.querySelector("input:checked");
}

// Each engine is a row of the list, with its initial on a tile, and a check mark when it's the one chosen.
function createEngineChoice(name, supportsSuggestions) {
    const choice = document.createElement("label");
    choice.className = "engine-choice";

    const radio = document.createElement("input");
    radio.type = "radio";
    radio.name = "search-engine";
    radio.value = name;
    radio.dataset.supportsSuggestions = supportsSuggestions;

    const tile = document.createElement("span");
    tile.setAttribute("aria-hidden", "true");
    if (name) {
        tile.className = "letter-tile";
        tile.textContent = name.charAt(0).toUpperCase();
    } else {
        tile.className = "empty-tile";
    }

    const label = document.createElement("span");
    label.className = "engine-name";
    label.textContent = name || "None";

    const check = document.createElementNS("http://www.w3.org/2000/svg", "svg");
    check.setAttribute("class", "engine-check");
    check.setAttribute("viewBox", "0 0 16 16");
    check.setAttribute("aria-hidden", "true");
    const path = document.createElementNS("http://www.w3.org/2000/svg", "path");
    path.setAttribute("d", "M3.5 8.5l3 3 6-7");
    check.append(path);

    choice.append(radio, tile, label, check);
    return choice;
}

function updateSearchSuggestionsControl() {
    const engine = selectedEngine();
    const supportsSuggestions = engine?.dataset.supportsSuggestions === "true";
    searchSuggestionsEnabled.disabled = !supportsSuggestions;
    if (!engine?.value) searchSuggestionsDescription.textContent = "Choose a search engine to use search suggestions.";
    else if (!supportsSuggestions)
        searchSuggestionsDescription.textContent = `Search suggestions aren't available for ${engine.value}.`;
    else searchSuggestionsDescription.textContent = `Sends what you type to ${engine.value}.`;
}

function renderEngineSettings() {
    const engines = [...ENGINES, ...(SEARCH_ENGINE.custom || [])];
    searchEngine.replaceChildren(
        createEngineChoice("", false),
        ...engines.map(engine =>
            createEngineChoice(engine.name, Boolean(engine.supportsSuggestions || engine.suggestionsUrl))
        )
    );
    const chosen = [...searchEngine.querySelectorAll("input")].find(
        radio => radio.value === (SEARCH_ENGINE.engine || "")
    );
    if (chosen) chosen.checked = true;

    searchSuggestionsEnabled.checked = SEARCH_ENGINE.suggestions === true;
    updateSearchSuggestionsControl();
    if (searchDialog.open) showSearchEngineSettings();
}

function saveSearchEngineSettings() {
    ladybird.sendMessage("setSearchEngineSettings", {
        engine: selectedEngine()?.value || null,
        suggestions: searchSuggestionsEnabled.checked,
    });
}

searchEngine.addEventListener("change", saveSearchEngineSettings);
searchSuggestionsEnabled.addEventListener("change", saveSearchEngineSettings);

function showSearchEngineSettings() {
    searchCustomName.classList.remove("error");
    searchCustomURL.classList.remove("error");
    searchCustomSuggestionsURL.classList.remove("error");
    searchList.innerHTML = "";

    const custom = SEARCH_ENGINE.custom || [];

    if (custom.length === 0) {
        const placeholder = document.createElement("div");
        placeholder.className = "dialog-list-item-placeholder";
        placeholder.textContent = "No custom search engines added";

        searchList.appendChild(placeholder);
    }

    custom.forEach(custom => {
        const name = document.createElement("span");
        name.textContent = custom.name;

        const url = document.createElement("span");
        url.className = "dialog-list-item-placeholder";
        url.style = "padding-left: 0";
        url.textContent = ` · ${custom.url}`;

        const engine = document.createElement("span");
        engine.className = "dialog-list-item-label";
        engine.appendChild(name);
        engine.appendChild(url);

        const remove = document.createElement("button");
        remove.className = "dialog-button";
        remove.innerHTML = "&times;";
        remove.title = `Remove ${custom.name}`;

        remove.addEventListener("click", () => {
            ladybird.sendMessage("removeCustomSearchEngine", custom);
        });

        const item = document.createElement("div");
        item.className = "dialog-list-item";
        item.appendChild(engine);
        item.appendChild(remove);

        searchList.appendChild(item);
    });

    if (!searchDialog.open) {
        setTimeout(() => searchCustomName.focus());
        searchDialog.showModal();
    }
}

function addCustomSearchEngine() {
    searchCustomName.classList.remove("error");
    searchCustomURL.classList.remove("error");
    searchCustomSuggestionsURL.classList.remove("error");

    if (!searchCustomName.value.trim()) {
        searchCustomName.classList.add("error");
        return;
    }

    for (const radio of searchEngine.querySelectorAll("input")) {
        if (searchCustomName.value === radio.value) {
            searchCustomName.classList.add("error");
            return;
        }
    }

    if (!containsValidURL(searchCustomURL)) {
        searchCustomURL.classList.add("error");
        return;
    }

    if (
        searchCustomSuggestionsURL.value &&
        (!containsValidURL(searchCustomSuggestionsURL) ||
            !searchCustomSuggestionsURL.value.includes("%s") ||
            !["http:", "https:"].includes(new URL(searchCustomSuggestionsURL.value).protocol))
    ) {
        searchCustomSuggestionsURL.classList.add("error");
        return;
    }

    ladybird.sendMessage("addCustomSearchEngine", {
        name: searchCustomName.value,
        url: searchCustomURL.value,
        suggestionsUrl: searchCustomSuggestionsURL.value,
    });

    searchCustomName.value = "";
    searchCustomURL.value = "";
    searchCustomSuggestionsURL.value = "";

    setTimeout(() => searchCustomName.focus());
}

searchCustomAdd.addEventListener("click", addCustomSearchEngine);

for (const input of [searchCustomName, searchCustomURL, searchCustomSuggestionsURL]) {
    input.addEventListener("keydown", event => {
        if (event.key === "Enter") addCustomSearchEngine();
    });
}

searchClose.addEventListener("click", () => {
    searchDialog.close();
});

registerDialogDeepLink({
    hash: "searchEngines",
    tab: "search",
    dialog: searchDialog,
    onOpen: showSearchEngineSettings,
});

searchSettings.addEventListener("click", () => {
    location.hash = "searchEngines";
});

document.addEventListener("WebUILoaded", () => {
    ladybird.sendMessage("loadAvailableEngines");
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        if (!Object.hasOwn(event.detail.data, "searchEngine")) {
            return;
        }

        SEARCH_ENGINE = event.detail.data.searchEngine || {};
        renderEngineSettings();
    } else if (event.detail.name === "loadEngines") {
        ENGINES = event.detail.data.search;
        renderEngineSettings();
    }
});
