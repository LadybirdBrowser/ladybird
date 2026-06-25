import { registerDialogDeepLink } from "./dialog-deep-link.js";

const siteSettings = document.querySelector("#site-settings");
const siteSettingsAdd = document.querySelector("#site-settings-add");
const siteSettingsClose = document.querySelector("#site-settings-close");
const siteSettingsPolicy = document.querySelector("#site-settings-policy");
const siteSettingsList = document.querySelector("#site-settings-list");
const siteSettingsInput = document.querySelector("#site-settings-input");
const siteSettingsRemoveAll = document.querySelector("#site-settings-remove-all");
const siteSettingsTitle = document.querySelector("#site-settings-title");

const autoplaySettings = document.querySelector("#autoplay-settings");

const SITE_SETTING_POLICY_OPTIONS = {
    autoplay: [
        { value: "allow-audio-and-video", label: "Allow Audio and Video" },
        { value: "block-audio", label: "Block Audio" },
        { value: "block-audio-and-video", label: "Block Audio and Video" },
    ],
};

let AUTOPLAY_SETTINGS = {};

function loadSiteSettings(settings) {
    AUTOPLAY_SETTINGS = settings.autoplay;

    const siteSetting = currentSiteSetting();

    if (siteSetting === "autoplay") {
        showSiteSettings("Autoplay", AUTOPLAY_SETTINGS);
    }
}

function forciblyEnableSiteSettings(settings) {
    settings.forEach(setting => {
        const label = document.querySelector(`#${setting}-forcibly-enabled`);
        label.classList.remove("hidden");

        const button = document.querySelector(`#${setting}-settings`);
        button.classList.add("hidden");
    });
}

function currentSiteSetting() {
    if (!siteSettings.open) {
        return null;
    }

    return siteSettingsTitle.innerText.toLowerCase();
}

function showSiteSettings(title, settings) {
    const setting = title.toLowerCase();

    siteSettingsTitle.innerText = title;

    siteSettingsPolicy.innerHTML = "";

    const policyOptions = SITE_SETTING_POLICY_OPTIONS[setting] ?? [];

    policyOptions.forEach(({ value, label }) => {
        const option = document.createElement("option");
        option.value = value;
        option.textContent = label;

        siteSettingsPolicy.appendChild(option);
    });

    siteSettingsPolicy.value = settings.policy;
    siteSettingsList.innerHTML = "";

    siteSettingsPolicy.onchange = () => {
        ladybird.sendMessage("setSiteSettingPolicy", {
            setting: currentSiteSetting(),
            policy: siteSettingsPolicy.value,
        });
    };

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
