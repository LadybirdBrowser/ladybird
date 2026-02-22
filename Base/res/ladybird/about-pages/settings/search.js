const searchClose = document.querySelector("#search-close");
const searchCustomAdd = document.querySelector("#search-custom-add");
const searchCustomName = document.querySelector("#search-custom-name");
const searchCustomURL = document.querySelector("#search-custom-url");
const searchDialog = document.querySelector("#search-dialog");
const searchEngine = document.querySelector("#search-engine");
const searchList = document.querySelector("#search-list");
const searchSettings = document.querySelector("#search-settings");

const autocompleteEngine = document.querySelector("#autocomplete-engine");
const autocompleteRemoteEnabled = document.querySelector("#autocomplete-remote-enabled");
const autocompleteLocalIndexMaxEntries = document.querySelector("#autocomplete-local-index-max-entries");
const autocompleteLocalIndexAdvanced = document.querySelector("#autocomplete-local-index-advanced");
const autocompleteLocalIndexDialog = document.querySelector("#autocomplete-local-index-dialog");
const autocompleteLocalIndexClose = document.querySelector("#autocomplete-local-index-close");
const autocompleteSearchTitleData = document.querySelector("#autocomplete-search-title-data");
const autocompleteLocalIndexRebuild = document.querySelector("#autocomplete-local-index-rebuild");
const autocompleteLocalIndexStatus = document.querySelector("#autocomplete-local-index-status");
const autocompleteLocalIndexTotalEntries = document.querySelector("#autocomplete-local-index-total-entries");
const autocompleteLocalIndexNavigationalEntries = document.querySelector(
    "#autocomplete-local-index-navigational-entries"
);
const autocompleteLocalIndexQueryEntries = document.querySelector("#autocomplete-local-index-query-entries");
const autocompleteLocalIndexBookmarkEntries = document.querySelector("#autocomplete-local-index-bookmark-entries");
const autocompleteLocalIndexHistoryEntries = document.querySelector("#autocomplete-local-index-history-entries");
const autocompleteLocalIndexUniqueTokens = document.querySelector("#autocomplete-local-index-unique-tokens");
const autocompleteLocalIndexPhrasePrefixes = document.querySelector("#autocomplete-local-index-phrase-prefixes");
const autocompleteLocalIndexTokenPrefixes = document.querySelector("#autocomplete-local-index-token-prefixes");
const autocompleteLocalIndexTransitionContexts = document.querySelector(
    "#autocomplete-local-index-transition-contexts"
);
const autocompleteLocalIndexTransitionEdges = document.querySelector("#autocomplete-local-index-transition-edges");

let SEARCH_ENGINE = {};
let AUTOCOMPLETE_ENGINE = {};

let NATIVE_SEARCH_ENGINE_COUNT = 0;
const numberFormatter = new Intl.NumberFormat();

const Engine = Object.freeze({
    search: 1,
    autocomplete: 2,
});

function loadEngineSettings(settings) {
    SEARCH_ENGINE = settings.searchEngine || {};
    AUTOCOMPLETE_ENGINE = settings.autocompleteEngine || {};

    autocompleteEngine.disabled = !SEARCH_ENGINE.name;
    autocompleteRemoteEnabled.checked = settings.autocompleteRemoteEnabled ?? true;
    autocompleteRemoteEnabled.disabled = !SEARCH_ENGINE.name;
    autocompleteLocalIndexMaxEntries.value = settings.autocompleteLocalIndexMaxEntries ?? 25000;
    autocompleteSearchTitleData.checked = settings.autocompleteSearchTitleData ?? false;

    loadCustomSearchEngines();
    renderEngine(Engine.search, SEARCH_ENGINE);
    renderEngine(Engine.autocomplete, AUTOCOMPLETE_ENGINE);

    if (searchDialog.open) {
        showSearchEngineSettings();
    }

    if (autocompleteLocalIndexDialog.open) {
        loadAutocompleteLocalIndexStats();
    }
}

function setAutocompleteLocalIndexStat(element, value) {
    element.textContent = numberFormatter.format(value);
}

function updateAutocompleteLocalIndexStats(stats) {
    setAutocompleteLocalIndexStat(autocompleteLocalIndexTotalEntries, stats.totalEntries);
    setAutocompleteLocalIndexStat(autocompleteLocalIndexNavigationalEntries, stats.navigationalEntries);
    setAutocompleteLocalIndexStat(autocompleteLocalIndexQueryEntries, stats.queryCompletionEntries);
    setAutocompleteLocalIndexStat(autocompleteLocalIndexBookmarkEntries, stats.bookmarkEntries);
    setAutocompleteLocalIndexStat(autocompleteLocalIndexHistoryEntries, stats.historyEntries);
    setAutocompleteLocalIndexStat(autocompleteLocalIndexUniqueTokens, stats.uniqueTokens);
    setAutocompleteLocalIndexStat(autocompleteLocalIndexPhrasePrefixes, stats.phrasePrefixes);
    setAutocompleteLocalIndexStat(autocompleteLocalIndexTokenPrefixes, stats.tokenPrefixes);
    setAutocompleteLocalIndexStat(autocompleteLocalIndexTransitionContexts, stats.termTransitionContexts);
    setAutocompleteLocalIndexStat(autocompleteLocalIndexTransitionEdges, stats.termTransitionEdges);

    let status = "Index is ready.";
    if (stats.isLoading) {
        status = "Loading local index from disk...";
    } else if (stats.rebuildInProgress) {
        status = "Rebuilding local suggestion index...";
    } else if (stats.rebuildPending) {
        status = "Index cleared. Rebuild starts on next OmniBox interaction.";
    } else if (stats.totalEntries === 0) {
        status = "Index is empty. Visit pages or add bookmarks, then press Rebuild Index.";
    }

    autocompleteLocalIndexStatus.textContent = status;
    autocompleteLocalIndexRebuild.disabled = stats.isLoading || stats.rebuildInProgress;
}

function loadAutocompleteLocalIndexStats() {
    ladybird.sendMessage("loadAutocompleteLocalIndexStats");
}

function showAutocompleteLocalIndexDialog() {
    if (!autocompleteLocalIndexDialog.open) {
        autocompleteLocalIndexDialog.showModal();
    }

    loadAutocompleteLocalIndexStats();
}

function engineForType(engine) {
    if (engine === Engine.search) {
        return ["Search", searchEngine];
    }
    if (engine === Engine.autocomplete) {
        return ["Autocomplete", autocompleteEngine];
    }
    throw Error(`Unrecognized engine type ${engine}`);
}

function loadEngines(type, engines) {
    const [name, engine] = engineForType(type);

    for (const engineName of engines) {
        const option = document.createElement("option");
        option.text = engineName;
        option.value = engineName;

        engine.add(option);
    }

    if (type === Engine.search) {
        NATIVE_SEARCH_ENGINE_COUNT = engine.length;
        engine.appendChild(document.createElement("hr"));
    }
}

function renderEngine(type, setting) {
    const [name, engine] = engineForType(type);

    if (setting.name) {
        engine.value = setting.name;
    } else {
        engine.selectedIndex = 0;
    }
}

function saveEngine(type) {
    const [name, engine] = engineForType(type);

    if (engine.selectedIndex !== 0) {
        ladybird.sendMessage(`set${name}Engine`, engine.value);
    } else {
        ladybird.sendMessage(`set${name}Engine`, null);
    }
}

function setSaveEngineListeners(type) {
    const [name, engine] = engineForType(type);

    engine.addEventListener("change", () => {
        saveEngine(type);
    });
}

setSaveEngineListeners(Engine.search);
setSaveEngineListeners(Engine.autocomplete);

autocompleteRemoteEnabled.addEventListener("change", () => {
    ladybird.sendMessage("setAutocompleteRemoteEnabled", autocompleteRemoteEnabled.checked);
});

autocompleteLocalIndexMaxEntries.addEventListener("change", () => {
    autocompleteLocalIndexMaxEntries.classList.remove("success");
    autocompleteLocalIndexMaxEntries.classList.remove("error");

    if (autocompleteLocalIndexMaxEntries.value.length === 0 || !autocompleteLocalIndexMaxEntries.checkValidity()) {
        autocompleteLocalIndexMaxEntries.classList.add("error");
        return;
    }

    ladybird.sendMessage("setAutocompleteLocalIndexMaxEntries", autocompleteLocalIndexMaxEntries.value | 0);
    autocompleteLocalIndexMaxEntries.classList.add("success");

    setTimeout(() => {
        autocompleteLocalIndexMaxEntries.classList.remove("success");
    }, 1000);
});

function loadCustomSearchEngines() {
    while (searchEngine.length > NATIVE_SEARCH_ENGINE_COUNT) {
        searchEngine.remove(NATIVE_SEARCH_ENGINE_COUNT);
    }

    const custom = SEARCH_ENGINE.custom || [];

    custom.forEach(custom => {
        const option = document.createElement("option");
        option.text = custom.name;
        option.value = custom.name;

        searchEngine.add(option);
    });
}

function showSearchEngineSettings() {
    searchCustomName.classList.remove("error");
    searchCustomURL.classList.remove("error");
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

    ladybird.sendMessage("addCustomSearchEngine", {
        name: searchCustomName.value,
        url: searchCustomURL.value,
    });

    searchCustomName.value = "";
    searchCustomURL.value = "";

    setTimeout(() => searchCustomName.focus());
}

searchCustomAdd.addEventListener("click", addCustomSearchEngine);

searchCustomName.addEventListener("keydown", event => {
    if (event.key === "Enter") {
        addCustomSearchEngine();
    }
});

searchCustomURL.addEventListener("keydown", event => {
    if (event.key === "Enter") {
        addCustomSearchEngine();
    }
});

searchClose.addEventListener("click", () => {
    searchDialog.close();
});

searchSettings.addEventListener("click", event => {
    showSearchEngineSettings();
    event.stopPropagation();
});

autocompleteLocalIndexAdvanced.addEventListener("click", event => {
    showAutocompleteLocalIndexDialog();
    event.stopPropagation();
});

autocompleteLocalIndexClose.addEventListener("click", () => {
    autocompleteLocalIndexDialog.close();
});

autocompleteLocalIndexRebuild.addEventListener("click", () => {
    ladybird.sendMessage("rebuildAutocompleteLocalIndex");
    loadAutocompleteLocalIndexStats();
});

autocompleteSearchTitleData.addEventListener("change", () => {
    ladybird.sendMessage("setAutocompleteSearchTitleData", autocompleteSearchTitleData.checked);
});

document.addEventListener("WebUILoaded", () => {
    ladybird.sendMessage("loadAvailableEngines");
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadEngineSettings(event.detail.data);
    } else if (event.detail.name === "loadEngines") {
        loadEngines(Engine.search, event.detail.data.search);
        loadEngines(Engine.autocomplete, event.detail.data.autocomplete);
    } else if (event.detail.name === "autocompleteLocalIndexStats") {
        updateAutocompleteLocalIndexStats(event.detail.data);
    }
});
