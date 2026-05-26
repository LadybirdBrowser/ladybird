const geolocationMode = document.querySelector("#geolocation-mode");
const geolocationActualMode = document.querySelector("#geolocation-mode option[value='actual']");
const geolocationEmulatedSettings = document.querySelector("#geolocation-emulated-settings");
const geolocationLatitude = document.querySelector("#geolocation-latitude");
const geolocationLongitude = document.querySelector("#geolocation-longitude");
const geolocationAltitude = document.querySelector("#geolocation-altitude");
const geolocationAltitudeAccuracy = document.querySelector("#geolocation-altitude-accuracy");
const geolocationSpeed = document.querySelector("#geolocation-speed");
const geolocationHeading = document.querySelector("#geolocation-heading");

let supportsActualGeolocation = false;

function updateUIForMode(mode) {
    geolocationEmulatedSettings.style.display = mode === "emulated" ? "block" : "none";
}

function updateActualGeolocationAvailability() {
    geolocationActualMode.hidden = !supportsActualGeolocation;

    if (!supportsActualGeolocation && geolocationMode.value === "actual") geolocationMode.value = "disabled";
}

geolocationMode.addEventListener("change", () => {
    updateUIForMode(geolocationMode.value);
    ladybird.sendMessage("setGeolocationMode", geolocationMode.value);
});

function sendCoordinates() {
    const lat = parseFloat(geolocationLatitude.value);
    const lng = parseFloat(geolocationLongitude.value);
    if (isNaN(lat) || isNaN(lng)) return;
    if (lat < -90 || lat > 90 || lng < -180 || lng > 180) return;

    const coords = { latitude: lat, longitude: lng };

    const alt = parseFloat(geolocationAltitude.value);
    if (!isNaN(alt)) coords.altitude = alt;

    const altAcc = parseFloat(geolocationAltitudeAccuracy.value);
    if (!isNaN(altAcc)) coords.altitudeAccuracy = altAcc;

    const speed = parseFloat(geolocationSpeed.value);
    if (!isNaN(speed)) coords.speed = speed;

    const heading = parseFloat(geolocationHeading.value);
    if (!isNaN(heading)) coords.heading = heading;

    ladybird.sendMessage("setGeolocationCoordinates", coords);
}

geolocationLatitude.addEventListener("change", sendCoordinates);
geolocationLongitude.addEventListener("change", sendCoordinates);
geolocationAltitude.addEventListener("change", sendCoordinates);
geolocationAltitudeAccuracy.addEventListener("change", sendCoordinates);
geolocationSpeed.addEventListener("change", sendCoordinates);
geolocationHeading.addEventListener("change", sendCoordinates);

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadFeatures") {
        supportsActualGeolocation = event.detail.data.actualGeolocation === true;
        updateActualGeolocationAvailability();
        return;
    }

    if (event.detail.name !== "loadSettings") return;

    const geo = event.detail.data.geolocation;
    if (!geo) return;

    let mode = geo.mode || "disabled";
    if (!supportsActualGeolocation && mode === "actual") mode = "disabled";

    geolocationMode.value = mode;
    geolocationLatitude.value = geo.latitude ?? 37.7647658;
    geolocationLongitude.value = geo.longitude ?? -122.4345892;
    geolocationAltitude.value = geo.altitude ?? 0;
    geolocationAltitudeAccuracy.value = geo.altitudeAccuracy ?? 0;
    geolocationSpeed.value = geo.speed ?? 0;
    geolocationHeading.value = geo.heading ?? 0;

    updateActualGeolocationAvailability();
    updateUIForMode(geolocationMode.value);
});
