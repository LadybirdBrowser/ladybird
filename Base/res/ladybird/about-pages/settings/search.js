const searchClose = document.querySelector("#search-close");
const searchCustomAdd = document.querySelector("#search-custom-add");
const searchCustomName = document.querySelector("#search-custom-name");
const searchCustomURL = document.querySelector("#search-custom-url");
const searchDialog = document.querySelector("#search-dialog");
const searchEngine = document.querySelector("#search-engine");
const searchList = document.querySelector("#search-list");
const searchSettings = document.querySelector("#search-settings");
const searchToggle = document.querySelector("#search-toggle");

const autocompleteEngine = document.querySelector("#autocomplete-engine");
const autocompleteToggle = document.querySelector("#autocomplete-toggle");

let SEARCH_ENGINE = {};
let AUTOCOMPLETE_ENGINE = {};

let NATIVE_SEARCH_ENGINE_COUNT = 0;

const Engine = Object.freeze({
    search: 1,
    autocomplete: 2,
});

function loadEngineSettings(settings) {
    SEARCH_ENGINE = settings.searchEngine || {};
    AUTOCOMPLETE_ENGINE = settings.autocompleteEngine || {};

    const renderEngineSettings = (type, setting) => {
        const [name, toggle, engine] = engineForType(type);

        if (setting.name) {
            toggle.checked = true;
            engine.value = setting.name;
        } else {
            toggle.checked = false;
        }

        renderEngine(type);
    };

    loadCustomSearchEngines();
    renderEngineSettings(Engine.search, SEARCH_ENGINE);
    renderEngineSettings(Engine.autocomplete, AUTOCOMPLETE_ENGINE);

    if (searchDialog.open) {
        showSearchEngineSettings();
    }
}

function engineForType(engine) {
    if (engine === Engine.search) {
        return ["Search", searchToggle, searchEngine];
    }
    if (engine === Engine.autocomplete) {
        return ["Autocomplete", autocompleteToggle, autocompleteEngine];
    }
    throw Error(`Unrecognized engine type ${engine}`);
}

function loadEngines(type, engines) {
    const [name, toggle, engine] = engineForType(type);

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

function renderEngine(type) {
    const [name, toggle, engine] = engineForType(type);

    if (toggle.checked) {
        engine.closest(".card-group").classList.remove("hidden");
    } else {
        engine.closest(".card-group").classList.add("hidden");
    }

    if (toggle.checked && engine.selectedIndex !== 0) {
        engine.item(0).disabled = true;
    } else if (!toggle.checked) {
        engine.item(0).disabled = false;
        engine.selectedIndex = 0;
    }
}

function saveEngine(type) {
    const [name, toggle, engine] = engineForType(type);

    if (toggle.checked && engine.selectedIndex !== 0) {
        ladybird.sendMessage(`set${name}Engine`, engine.value);
    } else if (!toggle.checked) {
        ladybird.sendMessage(`set${name}Engine`, null);
    }

    renderEngine(type);
}

function setSaveEngineListeners(type) {
    const [name, toggle, engine] = engineForType(type);

    toggle.addEventListener("change", () => {
        saveEngine(type);
    });
    engine.addEventListener("change", () => {
        saveEngine(type);
    });
}

setSaveEngineListeners(Engine.search);
setSaveEngineListeners(Engine.autocomplete);

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

    for (const i = 0; i < searchEngine.length; ++i) {
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

document.addEventListener("WebUILoaded", () => {
    ladybird.sendMessage("loadAvailableEngines");
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadEngineSettings(event.detail.data);
    } else if (event.detail.name === "loadEngines") {
        loadEngines(Engine.search, event.detail.data.search);
        loadEngines(Engine.autocomplete, event.detail.data.autocomplete);
    }
});
