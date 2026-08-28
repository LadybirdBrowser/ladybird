const enableForceDark = document.querySelector("#enable-force-dark");

enableForceDark.addEventListener("change", () => {
    ladybird.sendMessage("setForceDarkEnabled", enableForceDark.checked);
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name !== "loadSettings") return;

    enableForceDark.checked = event.detail.data.forceDarkEnabled === true;
});
