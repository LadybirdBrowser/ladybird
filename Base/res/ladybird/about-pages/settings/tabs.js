const tabLayoutGroup = document.querySelector("#tab-layout-group");
const tabLayoutInputs = tabLayoutGroup.querySelectorAll('input[name="tab-layout"]');
const verticalTabsExpandOnHover = document.querySelector("#vertical-tabs-expand-on-hover");
const verticalTabsPosition = document.querySelector("#vertical-tabs-position");
const showBookmarksBar = document.querySelector("#show-bookmarks-bar");
const discardHiddenTabsAfter = document.querySelector("#discard-hidden-tabs-after");
const tabProcesses = document.querySelector("#tab-processes");

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
    // With everything in the sidebar, the sidebar is always on the left.
    verticalTabsPosition.parentElement.classList.toggle(
        "hidden",
        !verticalTabsEnabled || !!TAB_SETTINGS.everythingInSidebar
    );
    showBookmarksBar.parentElement.classList.toggle(
        "hidden",
        verticalTabsEnabled && !!TAB_SETTINGS.everythingInSidebar
    );
};

const tabLayout = () => {
    if (!TAB_SETTINGS.verticalTabsEnabled) return "horizontal";
    return TAB_SETTINGS.everythingInSidebar ? "sidebar" : "vertical";
};

const loadSettings = settings => {
    if (!Object.hasOwn(settings, "tabs")) {
        return;
    }

    TAB_SETTINGS = settings.tabs || {};

    const layout = tabLayout();
    for (const input of tabLayoutInputs) input.checked = input.value === layout;
    verticalTabsExpandOnHover.checked = !!TAB_SETTINGS.verticalTabsExpandOnHover;
    verticalTabsPosition.value = TAB_SETTINGS.verticalTabsPosition || "left";
    const discardMinutes = String(TAB_SETTINGS.discardHiddenTabsAfterMinutes ?? 120);
    // A value set outside this page, such as in the settings file, gets an option of its own, so the menu shows it.
    if (![...discardHiddenTabsAfter.options].some(option => option.value === discardMinutes)) {
        discardHiddenTabsAfter.add(new Option(`After ${discardMinutes} minutes`, discardMinutes));
    }
    discardHiddenTabsAfter.value = discardMinutes;
    tabProcesses.value = TAB_SETTINGS.tabProcesses || "perOrigin";
    updateVerticalTabsDependentSettings();
};

function saveTabSettings() {
    ladybird.sendMessage("setTabSettings", TAB_SETTINGS);
}

for (const input of tabLayoutInputs) {
    input.addEventListener("change", () => {
        if (!input.checked) return;
        TAB_SETTINGS.verticalTabsEnabled = input.value !== "horizontal";
        TAB_SETTINGS.everythingInSidebar = input.value === "sidebar";
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

discardHiddenTabsAfter.addEventListener("change", () => {
    TAB_SETTINGS.discardHiddenTabsAfterMinutes = Number(discardHiddenTabsAfter.value);
    saveTabSettings();
});

tabProcesses.addEventListener("change", () => {
    TAB_SETTINGS.tabProcesses = tabProcesses.value;
    saveTabSettings();
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadFeatures") {
        loadFeatures(event.detail.data);
    } else if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    }
});
