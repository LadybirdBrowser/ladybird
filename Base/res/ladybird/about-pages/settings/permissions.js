import { registerDialogDeepLink } from "./dialog-deep-link.js";

const siteSettings = document.querySelector("#site-settings");
const siteSettingsAdd = document.querySelector("#site-settings-add");
const siteSettingsClose = document.querySelector("#site-settings-close");
const siteSettingsList = document.querySelector("#site-settings-list");
const siteSettingsInput = document.querySelector("#site-settings-input");
const siteSettingsRemoveAll = document.querySelector("#site-settings-remove-all");
const siteSettingsTitle = document.querySelector("#site-settings-title");

const autoplayPolicy = document.querySelector("#autoplay-policy");
const autoplaySettings = document.querySelector("#autoplay-settings");

let AUTOPLAY_SETTINGS = {};

function loadSiteSettings(settings) {
    if (!Object.hasOwn(settings, "autoplay")) {
        return;
    }

    AUTOPLAY_SETTINGS = settings.autoplay;
    autoplayPolicy.value = AUTOPLAY_SETTINGS.policy;

    const siteSetting = currentSiteSetting();

    if (siteSetting === "autoplay") {
        showSiteSettings("Autoplay", AUTOPLAY_SETTINGS);
    }
}

function forciblyEnableSiteSettings(settings) {
    settings.forEach(setting => {
        const label = document.querySelector(`#${setting}-forcibly-enabled`);
        label.classList.remove("hidden");

        const controls = document.querySelector(`#${setting}-controls`);
        controls.classList.add("hidden");
    });
}

function currentSiteSetting() {
    if (!siteSettings.open) {
        return null;
    }

    return siteSettingsTitle.innerText.toLowerCase();
}

function showSiteSettings(title, settings) {
    siteSettingsTitle.innerText = title;
    siteSettingsList.innerHTML = "";

    // A deep link can open this dialog before the settings have loaded, so tolerate a not-yet-populated object.
    const siteFilters = settings.siteFilters ?? [];

    if (siteFilters.length === 0) {
        const placeholder = document.createElement("div");
        placeholder.className = "dialog-list-item-placeholder";
        placeholder.textContent = "No sites added";

        siteSettingsList.appendChild(placeholder);
    }

    siteFilters.forEach(site => {
        const filter = document.createElement("span");
        filter.className = "dialog-list-item-label";
        filter.textContent = site;

        const remove = document.createElement("button");
        remove.className = "dialog-button";
        remove.innerHTML = "&times;";
        remove.title = `Remove ${site}`;

        remove.addEventListener("click", () => {
            ladybird.sendMessage("removeSiteSettingFilter", {
                setting: currentSiteSetting(),
                filter: site,
            });
        });

        const item = document.createElement("div");
        item.className = "dialog-list-item";
        item.appendChild(filter);
        item.appendChild(remove);

        siteSettingsList.appendChild(item);
    });

    if (!siteSettings.open) {
        setTimeout(() => siteSettingsInput.focus());
        siteSettings.showModal();
    }
}

function addSiteSettingFilter() {
    ladybird.sendMessage("addSiteSettingFilter", {
        setting: currentSiteSetting(),
        filter: siteSettingsInput.value,
    });

    siteSettingsInput.classList.add("success");
    siteSettingsInput.value = "";

    setTimeout(() => {
        siteSettingsInput.classList.remove("success");
    }, 1000);

    setTimeout(() => siteSettingsInput.focus());
}

siteSettingsAdd.addEventListener("click", addSiteSettingFilter);

siteSettingsInput.addEventListener("keydown", event => {
    if (event.key === "Enter") {
        addSiteSettingFilter();
    }
});

siteSettingsClose.addEventListener("click", () => {
    siteSettings.close();
});

siteSettingsRemoveAll.addEventListener("click", () => {
    ladybird.sendMessage("removeAllSiteSettingFilters", {
        setting: currentSiteSetting(),
    });
});

registerDialogDeepLink({
    hash: "autoplay",
    tab: "permissions",
    dialog: siteSettings,
    onOpen: () => showSiteSettings("Autoplay", AUTOPLAY_SETTINGS),
});

autoplayPolicy.addEventListener("change", () => {
    ladybird.sendMessage("setSiteSettingPolicy", {
        setting: "autoplay",
        policy: autoplayPolicy.value,
    });
});

autoplaySettings.addEventListener("click", () => {
    location.hash = "autoplay";
});

document.addEventListener("WebUILoaded", () => {
    ladybird.sendMessage("loadForciblyEnabledSiteSettings");
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadSiteSettings(event.detail.data);
    } else if (event.detail.name === "forciblyEnableSiteSettings") {
        forciblyEnableSiteSettings(event.detail.data);
    }
});
