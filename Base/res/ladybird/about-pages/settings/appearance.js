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
    showBookmarksBar.checked = !!APPEARANCE.showBookmarksBar;
};

function addChangeHandler(input, name) {
    input.addEventListener("change", () => {
        APPEARANCE[name] = input.checked;
        ladybird.sendMessage("setAppearance", APPEARANCE);
    });
}

addChangeHandler(showMenuBar, "showMenuBar");
addChangeHandler(showBookmarksBar, "showBookmarksBar");

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadFeatures") {
        loadFeatures(event.detail.data);
    } else if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    }
});
