const enableAutoscroll = document.querySelector("#enable-autoscroll");

let BROWSING_BEHAVIOR = {};

const loadSettings = settings => {
    BROWSING_BEHAVIOR = settings.browsingBehavior || {};

    enableAutoscroll.checked = !!BROWSING_BEHAVIOR.enableAutoscroll;
};

enableAutoscroll.addEventListener("change", () => {
    BROWSING_BEHAVIOR.enableAutoscroll = enableAutoscroll.checked;
    ladybird.sendMessage("setBrowsingBehavior", BROWSING_BEHAVIOR);
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    }
});
