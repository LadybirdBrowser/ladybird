const enableGeolocation = document.querySelector("#enable-geolocation");

enableGeolocation.addEventListener("change", () => {
    ladybird.sendMessage("setGeolocationEnabled", enableGeolocation.checked);
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadFeatures") {
        enableGeolocation.disabled = event.detail.data.geolocation !== true;
        return;
    }

    if (event.detail.name !== "loadSettings") return;

    enableGeolocation.checked = event.detail.data.geolocationEnabled === true;
});
