const adultContentToggle = document.querySelector("#adult-content-toggle");

function loadSettings(settings) {
    adultContentToggle.checked = settings.blockAdultContent;
}

adultContentToggle.addEventListener("change", () => {
    ladybird.sendMessage("setBlockAdultContent", adultContentToggle.checked);
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    }
});
