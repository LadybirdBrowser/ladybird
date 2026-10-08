const showMenuBar = document.querySelector("#show-menu-bar");
const showMenuBarGroup = document.querySelector("#show-menu-bar-group");
const showBookmarksBar = document.querySelector("#show-bookmarks-bar");

let APPEARANCE = {};

const loadFeatures = features => {
    showMenuBarGroup.classList.toggle("hidden", !features?.menuBar);
};

const loadSettings = settings => {
    APPEARANCE = settings.appearance || {};

    showMenuBar.checked = !!APPEARANCE.showMenuBar;
    showBookmarksBar.value = APPEARANCE.showBookmarksBar || "always";
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

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadFeatures") {
        loadFeatures(event.detail.data);
    } else if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    }
});
