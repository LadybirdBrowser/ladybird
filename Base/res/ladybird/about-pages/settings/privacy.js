const doNotTrackToggle = document.querySelector("#do-not-track-toggle");

function loadSettings(settings) {
    doNotTrackToggle.checked = settings.doNotTrack;
}

doNotTrackToggle.addEventListener("change", () => {
    ladybird.sendMessage("setDoNotTrack", doNotTrackToggle.checked);
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    }
});
