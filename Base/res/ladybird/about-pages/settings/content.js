const defaultZoomLevel = document.querySelector("#default-zoom-level");
const enableForceDark = document.querySelector("#enable-force-dark");
const forceDarkSites = document.querySelector("#force-dark-sites");

let CONTENT = {};
let forceDarkSitesReadOnly = false;

const loadSettings = settings => {
    if (Object.hasOwn(settings, "forceDarkSitesReadOnly")) {
        forceDarkSitesReadOnly = settings.forceDarkSitesReadOnly;
    }
    if (!Object.hasOwn(settings, "content")) {
        return;
    }

    CONTENT = settings.content || {};

    const snapped = snapToClosestFactor(CONTENT.defaultZoomLevelFactor);
    if (snapped !== null) {
        defaultZoomLevel.value = snapped.toString();
    } else {
        console.warn("No close match found for zoom factor: ", CONTENT.defaultZoomLevelFactor);
    }

    enableForceDark.checked = !!CONTENT.enableForceDark;
    forceDarkSites.replaceChildren();
    const choices = Object.entries(CONTENT.forceDarkPerHost || {}).sort(([a], [b]) => a.localeCompare(b));
    forceDarkSites.hidden = choices.length === 0;
    for (const [host, enabled] of choices) {
        const row = document.createElement("div");
        row.className = "dialog-list-item";
        row.dataset.host = host;
        const label = document.createElement("span");
        label.className = "dialog-list-item-label";
        label.textContent = `${host}: ${enabled ? "On" : "Off"}`;
        const reset = document.createElement("button");
        reset.textContent = "Use default";
        reset.disabled = forceDarkSitesReadOnly;
        reset.setAttribute("aria-label", `Use default force-dark setting for ${host}`);
        reset.addEventListener("click", () => ladybird.sendMessage("resetForceDarkForHost", host));
        row.append(label, reset);
        forceDarkSites.appendChild(row);
    }
};

const ZOOM_LEVEL_FACTORS = [1 / 3.0, 0.5, 2 / 3.0, 0.75, 0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0, 4.0, 5.0];
const ZOOM_LEVEL_FACTOR_MAP = ZOOM_LEVEL_FACTORS.map(factor => ({
    factor,
    label: `${Math.round(factor * 100)}%`,
}));

ZOOM_LEVEL_FACTOR_MAP.forEach(item => {
    const zoomLevelOption = document.createElement("option");
    zoomLevelOption.textContent = item.label;
    zoomLevelOption.value = item.factor.toString();
    defaultZoomLevel.appendChild(zoomLevelOption);
});

function snapToClosestFactor(value) {
    const tolerance = 0.001;

    let closest = ZOOM_LEVEL_FACTOR_MAP[0].factor;
    let minDiff = Math.abs(value - closest);

    for (const item of ZOOM_LEVEL_FACTOR_MAP) {
        const diff = Math.abs(value - item.factor);
        if (diff < minDiff) {
            minDiff = diff;
            closest = item.factor;
        }
    }

    return minDiff <= tolerance ? closest : null;
}

defaultZoomLevel.addEventListener("change", () => {
    CONTENT.defaultZoomLevelFactor = parseFloat(defaultZoomLevel.value);
    ladybird.sendMessage("setContentSettings", CONTENT);
});

enableForceDark.addEventListener("change", () => {
    CONTENT.enableForceDark = enableForceDark.checked;
    ladybird.sendMessage("setContentSettings", CONTENT);
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    }
});
