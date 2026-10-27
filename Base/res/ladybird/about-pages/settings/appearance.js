const showMenuBar = document.querySelector("#show-menu-bar");
const showMenuBarGroup = document.querySelector("#show-menu-bar-group");
const showBookmarksBar = document.querySelector("#show-bookmarks-bar");
const interfaceTheme = document.querySelector("#interface-theme");
const websiteColorScheme = document.querySelector("#website-color-scheme");

function segmentedValue(control) {
    return control.querySelector("input:checked")?.value;
}

function setSegmentedValue(control, value) {
    for (const input of control.querySelectorAll("input")) input.checked = input.value === value;
}

let APPEARANCE = {};

const loadFeatures = features => {
    showMenuBarGroup.classList.toggle("hidden", !features?.menuBar);
};

const loadSettings = settings => {
    if (!Object.hasOwn(settings, "appearance")) {
        return;
    }

    APPEARANCE = settings.appearance || {};

    showMenuBar.checked = !!APPEARANCE.showMenuBar;
    showBookmarksBar.value = APPEARANCE.showBookmarksBar || "always";
    setSegmentedValue(interfaceTheme, APPEARANCE.interfaceTheme || "system");
    setSegmentedValue(websiteColorScheme, APPEARANCE.websiteColorScheme || "system");
};

function saveAppearance() {
    ladybird.sendMessage("setAppearance", APPEARANCE);
}

showMenuBar.addEventListener("change", () => {
    APPEARANCE.showMenuBar = showMenuBar.checked;
    saveAppearance();
});

showBookmarksBar.addEventListener("change", () => {
    APPEARANCE.showBookmarksBar = showBookmarksBar.value;
    saveAppearance();
});

interfaceTheme.addEventListener("change", () => {
    APPEARANCE.interfaceTheme = segmentedValue(interfaceTheme);
    saveAppearance();
});

websiteColorScheme.addEventListener("change", () => {
    APPEARANCE.websiteColorScheme = segmentedValue(websiteColorScheme);
    saveAppearance();
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadFeatures") {
        loadFeatures(event.detail.data);
    } else if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    }
});
