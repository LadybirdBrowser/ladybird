import { registerDialogDeepLink } from "./dialog-deep-link.js";

const defaultZoomLevel = document.querySelector("#default-zoom-level");
const enableForceDark = document.querySelector("#enable-force-dark");
const hostSettings = document.querySelector("#host-settings");
const hostSettingsTitle = document.querySelector("#host-settings-title");
const hostSettingsList = document.querySelector("#host-settings-list");
const hostSettingsResetAll = document.querySelector("#host-settings-reset-all");

const HOST_SETTINGS = {
    "force-dark": {
        title: "Force dark mode overrides",
        key: "forceDarkPerHost",
        format: enabled => (enabled ? "On" : "Off"),
    },
    zoom: {
        title: "Page zoom overrides",
        key: "zoomPerHost",
        format: factor => `${Math.round(factor * 100)}%`,
    },
};

let CONTENT = {};
let CURRENT_HOST_SETTING = null;

function renderHostSettings() {
    const { key, format } = HOST_SETTINGS[CURRENT_HOST_SETTING];
    const overrides = Object.entries(CONTENT[key] ?? {}).sort(([a], [b]) => a.localeCompare(b));
    hostSettingsList.replaceChildren();
    hostSettingsResetAll.disabled = overrides.length === 0;

    if (overrides.length === 0) {
        const placeholder = document.createElement("div");
        placeholder.className = "dialog-list-item-placeholder";
        placeholder.textContent = "No site overrides";
        hostSettingsList.appendChild(placeholder);
    }

    for (const [host, value] of overrides) {
        const row = document.createElement("div");
        row.className = "dialog-list-item";
        row.dataset.host = host;

        const label = document.createElement("span");
        label.className = "dialog-list-item-label";
        label.textContent = host;

        const savedValue = document.createElement("span");
        savedValue.className = "dialog-list-item-value";
        savedValue.textContent = format(value);

        const reset = document.createElement("button");
        reset.className = "secondary-button";
        reset.textContent = "Use default";
        reset.setAttribute("aria-label", `Use default for ${host}`);
        reset.addEventListener("click", () => {
            ladybird.sendMessage("resetHostSetting", { setting: CURRENT_HOST_SETTING, host });
        });
        row.append(label, savedValue, reset);
        hostSettingsList.appendChild(row);
    }
}

for (const [setting, { title }] of Object.entries(HOST_SETTINGS)) {
    registerDialogDeepLink({
        hash: setting,
        tab: "appearance",
        dialog: hostSettings,
        onOpen: () => {
            CURRENT_HOST_SETTING = setting;
            hostSettingsTitle.textContent = title;
            renderHostSettings();
        },
    });

    document.querySelector(`#${setting}-settings`).addEventListener("click", () => {
        location.hash = setting;
    });
}

document.querySelector("#host-settings-close").addEventListener("click", () => {
    hostSettings.close();
});

hostSettingsResetAll.addEventListener("click", () => {
    ladybird.sendMessage("resetHostSetting", { setting: CURRENT_HOST_SETTING });
});

const loadSettings = settings => {
    if (!Object.hasOwn(settings, "content")) {
        return;
    }

    CONTENT = settings.content || {};

    for (const [setting, { key }] of Object.entries(HOST_SETTINGS)) {
        document.querySelector(`#${setting}-settings`).hidden = Object.keys(CONTENT[key] ?? {}).length === 0;
    }

    const snapped = snapToClosestFactor(CONTENT.defaultZoomLevelFactor);
    if (snapped !== null) {
        defaultZoomLevel.value = snapped.toString();
    } else {
        console.warn("No close match found for zoom factor: ", CONTENT.defaultZoomLevelFactor);
    }

    enableForceDark.checked = !!CONTENT.enableForceDark;
    if (hostSettings.open) {
        renderHostSettings();
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
