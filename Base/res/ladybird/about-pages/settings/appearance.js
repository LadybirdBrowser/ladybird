const showMenuBar = document.querySelector("#show-menu-bar");
const alwaysShowScrollbars = document.querySelector("#always-show-scrollbars");
const showMenuBarGroup = document.querySelector("#show-menu-bar-group");
const windowDecorations = document.querySelector("#use-client-side-window-decorations");
const windowDecorationsGroup = document.querySelector("#window-decorations-group");
const showBookmarksBar = document.querySelector("#show-bookmarks-bar");
const interfaceTheme = document.querySelector("#interface-theme");
const websiteColorScheme = document.querySelector("#website-color-scheme");
const colorTheme = document.querySelector("#color-theme");

const WINDOW_DECORATIONS_VARIABLE = "ui.window.use_client_side_decorations";

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
    const windowDecorationsVariable = settings.configVariableDefinitions?.find(
        variable => variable.name === WINDOW_DECORATIONS_VARIABLE
    );
    if (windowDecorationsVariable) {
        windowDecorations.checked = windowDecorationsVariable.value;
        windowDecorationsGroup.classList.remove("hidden");
    }

    if (!Object.hasOwn(settings, "appearance")) {
        return;
    }

    APPEARANCE = settings.appearance || {};

    showMenuBar.checked = !!APPEARANCE.showMenuBar;
    alwaysShowScrollbars.checked = !!APPEARANCE.alwaysShowScrollbars;
    showBookmarksBar.value = APPEARANCE.showBookmarksBar || "never";
    setSegmentedValue(interfaceTheme, APPEARANCE.interfaceTheme || "system");
    setSegmentedValue(websiteColorScheme, APPEARANCE.websiteColorScheme || "system");
    setSegmentedValue(colorTheme, APPEARANCE.colorTheme || "neutral");
};

function saveAppearance() {
    ladybird.sendMessage("setAppearance", APPEARANCE);
}

showMenuBar.addEventListener("change", () => {
    APPEARANCE.showMenuBar = showMenuBar.checked;
    saveAppearance();
});

windowDecorations.addEventListener("change", () => {
    ladybird.sendMessage("setConfigVariable", {
        name: WINDOW_DECORATIONS_VARIABLE,
        value: windowDecorations.checked,
    });
});

alwaysShowScrollbars.addEventListener("change", () => {
    APPEARANCE.alwaysShowScrollbars = alwaysShowScrollbars.checked;
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

colorTheme.addEventListener("change", () => {
    APPEARANCE.colorTheme = segmentedValue(colorTheme);
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
