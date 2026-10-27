const tabLayoutGroup = document.querySelector("#tab-layout-group");
const tabLayoutInputs = tabLayoutGroup.querySelectorAll('input[name="tab-layout"]');
const verticalTabsExpandOnHover = document.querySelector("#vertical-tabs-expand-on-hover");
const verticalTabsPosition = document.querySelector("#vertical-tabs-position");

let TAB_SETTINGS = {};

const loadFeatures = features => {
    // Without vertical tabs, there is no choice of tab layout to make.
    tabLayoutGroup.classList.toggle("hidden", !features?.verticalTabs);
    if (!features?.verticalTabs) {
        verticalTabsExpandOnHover.parentElement.classList.add("hidden");
        verticalTabsPosition.parentElement.classList.add("hidden");
    }
};

const updateVerticalTabsDependentSettings = () => {
    const verticalTabsEnabled = !!TAB_SETTINGS.verticalTabsEnabled && !tabLayoutGroup.classList.contains("hidden");
    verticalTabsExpandOnHover.parentElement.classList.toggle("hidden", !verticalTabsEnabled);
    verticalTabsPosition.parentElement.classList.toggle("hidden", !verticalTabsEnabled);
};

const loadSettings = settings => {
    if (!Object.hasOwn(settings, "tabs")) {
        return;
    }

    TAB_SETTINGS = settings.tabs || {};

    const layout = TAB_SETTINGS.verticalTabsEnabled ? "vertical" : "horizontal";
    for (const input of tabLayoutInputs) input.checked = input.value === layout;
    verticalTabsExpandOnHover.checked = !!TAB_SETTINGS.verticalTabsExpandOnHover;
    verticalTabsPosition.value = TAB_SETTINGS.verticalTabsPosition || "left";
    updateVerticalTabsDependentSettings();
};

function saveTabSettings() {
    ladybird.sendMessage("setTabSettings", TAB_SETTINGS);
}

for (const input of tabLayoutInputs) {
    input.addEventListener("change", () => {
        if (!input.checked) return;
        TAB_SETTINGS.verticalTabsEnabled = input.value === "vertical";
        updateVerticalTabsDependentSettings();
        saveTabSettings();
    });
}

verticalTabsExpandOnHover.addEventListener("change", () => {
    TAB_SETTINGS.verticalTabsExpandOnHover = verticalTabsExpandOnHover.checked;
    saveTabSettings();
});

verticalTabsPosition.addEventListener("change", () => {
    TAB_SETTINGS.verticalTabsPosition = verticalTabsPosition.value;
    saveTabSettings();
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadFeatures") {
        loadFeatures(event.detail.data);
    } else if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    }
});
