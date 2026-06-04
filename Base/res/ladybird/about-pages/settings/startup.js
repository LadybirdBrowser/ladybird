const restoreSessionOnStartup = document.querySelector("#restore-session-on-startup");

let STARTUP_BEHAVIOR = {};

const loadSettings = settings => {
    STARTUP_BEHAVIOR.restoreSessionOnStartup = settings.restoreSessionOnStartup ?? true;
    restoreSessionOnStartup.checked = STARTUP_BEHAVIOR.restoreSessionOnStartup;
};

restoreSessionOnStartup.addEventListener("change", () => {
    STARTUP_BEHAVIOR.restoreSessionOnStartup = restoreSessionOnStartup.checked;
    ladybird.sendMessage("setStartupBehavior", STARTUP_BEHAVIOR);
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    }
});
