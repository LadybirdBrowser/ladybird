const enableVerticalTabs = document.querySelector("#enable-vertical-tabs");

let TAB_SETTINGS = {};

const loadFeatures = features => {
    // When more tab settings are added, we will need to be more selective here.
    const tabSettingsCard = enableVerticalTabs.closest(".card");
    tabSettingsCard.previousElementSibling.classList.toggle("hidden", !features?.verticalTabs);
    tabSettingsCard.classList.toggle("hidden", !features?.verticalTabs);
};

const loadSettings = settings => {
    TAB_SETTINGS = settings.tabs || {};

    enableVerticalTabs.checked = !!TAB_SETTINGS.verticalTabsEnabled;
};

function addChangeHandler(input, name) {
    input.addEventListener("change", () => {
        TAB_SETTINGS[name] = input.checked;
        ladybird.sendMessage("setTabSettings", TAB_SETTINGS);
    });
}

addChangeHandler(enableVerticalTabs, "verticalTabsEnabled");

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadFeatures") {
        loadFeatures(event.detail.data);
    } else if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    }
});
