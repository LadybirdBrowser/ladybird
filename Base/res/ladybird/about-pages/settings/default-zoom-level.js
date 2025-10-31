const defaultZoomLevelDropdown = document.querySelector("#default-zoom-level");

const zoomLevelFactors = [1 / 3.0, 0.5, 2 / 3.0, 0.75, 0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0, 4.0, 5.0];

const zoomLevelFactorMap = zoomLevelFactors.map(factor => ({
    factor,
    label: `${Math.round(factor * 100)}%`,
}));

function snapToClosestFactor(value, factorMap) {
    const tolerance = 0.001;

    let closest = factorMap[0].factor;
    let minDiff = Math.abs(value - closest);

    for (const item of factorMap) {
        const diff = Math.abs(value - item.factor);
        if (diff < minDiff) {
            minDiff = diff;
            closest = item.factor;
        }
    }

    return minDiff <= tolerance ? closest : null;
}

const loadSettings = settings => {
    defaultZoomLevelDropdown.innerHTML = "";

    zoomLevelFactorMap.forEach(item => {
        const zoomLevelOption = document.createElement("option");
        zoomLevelOption.textContent = item.label;
        zoomLevelOption.value = item.factor.toString();
        defaultZoomLevelDropdown.appendChild(zoomLevelOption);
    });

    const snapped = snapToClosestFactor(settings.defaultZoomLevelFactor, zoomLevelFactorMap);
    if (snapped !== null) {
        defaultZoomLevelDropdown.value = snapped.toString();
    } else {
        console.warn("No close match found for zoom factor: ", settings.defaultZoomLevelFactor);
    }
};

defaultZoomLevelDropdown.addEventListener("change", () => {
    ladybird.sendMessage("setDefaultZoomLevelFactor", parseFloat(defaultZoomLevelDropdown.value));
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    }
});
