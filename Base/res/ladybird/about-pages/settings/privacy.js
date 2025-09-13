const globalPrivacyControlToggle = document.querySelector("#global-privacy-control-toggle");

function loadSettings(settings) {
    globalPrivacyControlToggle.checked = settings.globalPrivacyControl;
}

globalPrivacyControlToggle.addEventListener("change", () => {
    ladybird.sendMessage("setGlobalPrivacyControl", globalPrivacyControlToggle.checked);
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    }
});
