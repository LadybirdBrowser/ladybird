const enableGeolocation = document.querySelector("#enable-geolocation");

enableGeolocation.addEventListener("change", () => {
    ladybird.sendMessage("setLocationPolicy", enableGeolocation.checked ? "ask" : "block");
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadFeatures") {
        enableGeolocation.disabled = event.detail.data.geolocation !== true;
        return;
    }

    if (event.detail.name !== "loadSettings") {
        return;
    }
    if (!Object.hasOwn(event.detail.data, "locationPolicy")) {
        return;
    }

    enableGeolocation.checked = event.detail.data.locationPolicy === "ask";
});
