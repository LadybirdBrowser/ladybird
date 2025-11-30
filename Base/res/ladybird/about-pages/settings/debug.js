const debugDumpPath = document.querySelector("#debug-dump-path");

let DEBUG_SETTINGS = {};

function loadSettings(settings) {
    DEBUG_SETTINGS = settings;
    loadDebugSettings();
}

function loadDebugSettings() {
    debugDumpPath.value = DEBUG_SETTINGS.debugDumpPath || "";
}

function loadDefaultDebugDumpPath() {
    ladybird.sendMessage("getDefaultDebugDumpPath");
}

debugDumpPath.addEventListener("change", () => {
    debugDumpPath.classList.remove("success");
    debugDumpPath.classList.remove("error");

    const path = debugDumpPath.value.trim();

    if (path.length === 0) {
        debugDumpPath.classList.add("error");
        return;
    }

    ladybird.sendMessage("setDebugDumpPath", path);
    debugDumpPath.classList.add("success");

    setTimeout(() => {
        debugDumpPath.classList.remove("success");
    }, 1000);
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    } else if (event.detail.name === "defaultDebugDumpPath") {
        debugDumpPath.value = event.detail.data;

        ladybird.sendMessage("setDebugDumpPath", event.detail.data);

        debugDumpPath.classList.add("success");

        setTimeout(() => {
            debugDumpPath.classList.remove("success");
        }, 1000);
    }
});
