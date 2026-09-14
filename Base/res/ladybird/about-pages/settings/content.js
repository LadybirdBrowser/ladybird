const defaultZoomLevel = document.querySelector("#default-zoom-level");
const enableForceDark = document.querySelector("#enable-force-dark");

let CONTENT = {};

const loadSettings = settings => {
    CONTENT = settings.content || {};

    const snapped = snapToClosestFactor(CONTENT.defaultZoomLevelFactor);
    if (snapped !== null) {
        defaultZoomLevel.value = snapped.toString();
    } else {
        console.warn("No close match found for zoom factor: ", CONTENT.defaultZoomLevelFactor);
    }

    enableForceDark.checked = !!CONTENT.enableForceDark;
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
