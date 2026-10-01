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

function populateSearchEngineSelect(select, engines, settings = {}) {
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

function updateSearchSuggestionsControl(select, checkbox, description) {
    const supportsSuggestions = select.selectedOptions[0]?.dataset.supportsSuggestions === "true";
    checkbox.disabled = !supportsSuggestions;
    if (!select.value) description.textContent = "Choose a search engine to use search suggestions.";
    else if (!supportsSuggestions) description.textContent = `Search suggestions aren't available for ${select.value}.`;
    else description.textContent = `Sends what you type to ${select.value}.`;
}

function renderEngineSettings() {
    populateSearchEngineSelect(searchEngine, ENGINES, SEARCH_ENGINE);
    searchSuggestionsEnabled.checked = SEARCH_ENGINE.suggestions === true;
    updateSearchSuggestionsControl(searchEngine, searchSuggestionsEnabled, searchSuggestionsDescription);
    if (searchDialog.open) showSearchEngineSettings();
}

function saveSearchEngineSettings() {
    ladybird.sendMessage("setSearchEngineSettings", {
        engine: searchEngine.value || null,
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
        url.textContent = ` — ${custom.url}`;

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

    for (let i = 0; i < searchEngine.length; ++i) {
        if (searchCustomName.value === searchEngine.item(i).value) {
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
        SEARCH_ENGINE = event.detail.data.searchEngine || {};
        renderEngineSettings();
    } else if (event.detail.name === "loadEngines") {
        ENGINES = event.detail.data.search;
        renderEngineSettings();
    }
});
