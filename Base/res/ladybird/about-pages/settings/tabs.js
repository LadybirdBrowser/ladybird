const enableVerticalTabs = document.querySelector("#enable-vertical-tabs");
const verticalTabsExpandOnHover = document.querySelector("#vertical-tabs-expand-on-hover");
const verticalTabsPosition = document.querySelector("#vertical-tabs-position");

let TAB_SETTINGS = {};

const loadFeatures = features => {
    // When more tab settings are added, we will need to be more selective here.
    const tabSettingsCard = enableVerticalTabs.closest(".card");
    tabSettingsCard.previousElementSibling.classList.toggle("hidden", !features?.verticalTabs);
    tabSettingsCard.classList.toggle("hidden", !features?.verticalTabs);
};

const updateVerticalTabsDependentSettings = () => {
    verticalTabsExpandOnHover.parentElement.classList.toggle("hidden", !enableVerticalTabs.checked);
    verticalTabsPosition.parentElement.classList.toggle("hidden", !enableVerticalTabs.checked);
};

const loadSettings = settings => {
    TAB_SETTINGS = settings.tabs || {};

    enableVerticalTabs.checked = !!TAB_SETTINGS.verticalTabsEnabled;
    verticalTabsExpandOnHover.checked = !!TAB_SETTINGS.verticalTabsExpandOnHover;
    verticalTabsPosition.value = TAB_SETTINGS.verticalTabsPosition || "left";
    updateVerticalTabsDependentSettings();
};

function saveTabSettings() {
    ladybird.sendMessage("setTabSettings", TAB_SETTINGS);
}

function addCheckboxChangeHandler(input, name) {
    input.addEventListener("change", () => {
        TAB_SETTINGS[name] = input.checked;
        updateVerticalTabsDependentSettings();
        saveTabSettings();
    });
}

function addSelectChangeHandler(input, name) {
    input.addEventListener("change", () => {
        TAB_SETTINGS[name] = input.value;
        saveTabSettings();
    });
}

addCheckboxChangeHandler(enableVerticalTabs, "verticalTabsEnabled");
addCheckboxChangeHandler(verticalTabsExpandOnHover, "verticalTabsExpandOnHover");
addSelectChangeHandler(verticalTabsPosition, "verticalTabsPosition");

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadFeatures") {
        loadFeatures(event.detail.data);
    } else if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    }
});
