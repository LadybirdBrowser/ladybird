const enableAutoscroll = document.querySelector("#enable-autoscroll");
const enablePrimaryPaste = document.querySelector("#enable-primary-paste");
const enablePrimaryPasteGroup = document.querySelector("#enable-primary-paste-group");
const restorePreviousSession = document.querySelector("#restore-previous-session");

let BROWSING_BEHAVIOR = {};

const loadFeatures = features => {
    enablePrimaryPasteGroup.classList.toggle("hidden", !features?.primaryPaste);
};

const loadSettings = settings => {
    BROWSING_BEHAVIOR = settings.browsingBehavior || {};

    enableAutoscroll.checked = !!BROWSING_BEHAVIOR.enableAutoscroll;
    enablePrimaryPaste.checked = !!BROWSING_BEHAVIOR.enablePrimaryPaste;
    restorePreviousSession.checked = !!BROWSING_BEHAVIOR.restorePreviousSession;
};

function addChangeHandler(input, name) {
    input.addEventListener("change", () => {
        BROWSING_BEHAVIOR[name] = input.checked;
        ladybird.sendMessage("setBrowsingBehavior", BROWSING_BEHAVIOR);
    });
}

addChangeHandler(enableAutoscroll, "enableAutoscroll");
addChangeHandler(enablePrimaryPaste, "enablePrimaryPaste");
addChangeHandler(restorePreviousSession, "restorePreviousSession");

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadFeatures") {
        loadFeatures(event.detail.data);
    } else if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    }
});
