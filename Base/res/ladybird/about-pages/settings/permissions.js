import { registerDialogDeepLink } from "./dialog-deep-link.js";

const siteSettings = document.querySelector("#site-settings");
const siteSettingsAdd = document.querySelector("#site-settings-add");
const siteSettingsClose = document.querySelector("#site-settings-close");
const siteSettingsList = document.querySelector("#site-settings-list");
const siteSettingsInput = document.querySelector("#site-settings-input");
const siteSettingsDecision = document.querySelector("#site-settings-decision");
const siteSettingsRemoveAll = document.querySelector("#site-settings-remove-all");
const siteSettingsTitle = document.querySelector("#site-settings-title");

const PERMISSION_POLICIES = [
    { value: "allow", label: "Allow" },
    { value: "block", label: "Block" },
];

const SITE_SETTINGS = {
    autoplay: {
        title: "Autoplay",
        policies: Array.from(document.querySelector("#autoplay-policy").options, option => ({
            value: option.value,
            label: option.textContent,
        })),
    },
    geolocation: { title: "Location", policies: PERMISSION_POLICIES },
    microphone: { title: "Microphone", policies: PERMISSION_POLICIES },
    notifications: { title: "Notifications", policies: PERMISSION_POLICIES },
};

let SETTINGS = {};
let CURRENT_SETTING = null;

function loadSiteSettings(data) {
    if (!Object.hasOwn(data, "siteSettings")) {
        return;
    }

    SETTINGS = data.siteSettings;

    for (const setting of Object.keys(SITE_SETTINGS)) {
        const control = document.querySelector(`#${setting}-policy`);
        if (control.type === "checkbox") {
            control.checked = SETTINGS[setting].policy === "ask";
        } else {
            control.value = SETTINGS[setting].policy;
        }
    }

    if (siteSettings.open) {
        renderSiteSettings();
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

function populatePolicySelect(select, value) {
    const policies = SITE_SETTINGS[CURRENT_SETTING].policies;
    select.replaceChildren();

    for (const { value, label } of policies) {
        const option = document.createElement("option");
        option.value = value;
        option.textContent = label;
        select.appendChild(option);
    }

    if (value === "allow-once") {
        const option = document.createElement("option");
        option.value = value;
        option.textContent = "Allow this time";
        select.appendChild(option);
    }

    select.value = value ?? policies[0].value;
}

function renderSiteSettings() {
    siteSettingsList.replaceChildren();

    // A deep link can open the dialog before settings have loaded.
    const siteFilters = SETTINGS[CURRENT_SETTING]?.siteFilters ?? [];
    if (siteFilters.length === 0) {
        const placeholder = document.createElement("div");
        placeholder.className = "dialog-list-item-placeholder";
        placeholder.textContent = "No sites added";
        siteSettingsList.appendChild(placeholder);
    }

    siteFilters.forEach(site => {
        const siteFilter = site.filter;
        const filter = document.createElement("span");
        filter.className = "dialog-list-item-label";
        filter.textContent = siteFilter;

        const select = document.createElement("select");
        select.setAttribute("aria-label", `Policy for ${siteFilter}`);
        populatePolicySelect(select, site.policy);
        select.addEventListener("change", () => {
            ladybird.sendMessage("addSiteSettingFilter", {
                setting: CURRENT_SETTING,
                filter: siteFilter,
                policy: select.value,
            });
        });

        const remove = document.createElement("button");
        remove.className = "dialog-button";
        remove.title = `Remove ${siteFilter}`;
        remove.innerHTML =
            '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true"><path d="M3 3l10 10M13 3L3 13" /></svg>';
        remove.addEventListener("click", () => {
            ladybird.sendMessage("removeSiteSettingFilter", {
                setting: CURRENT_SETTING,
                filter: siteFilter,
            });
        });

        const item = document.createElement("div");
        item.className = "dialog-list-item";
        item.dataset.filter = siteFilter;
        item.appendChild(filter);
        item.appendChild(select);
        item.appendChild(remove);

        siteSettingsList.appendChild(item);
    });
}

function showSiteSettings(setting) {
    if (CURRENT_SETTING !== setting) {
        siteSettingsInput.value = "";
        siteSettingsInput.setCustomValidity("");
    }

    const selectedPolicy = CURRENT_SETTING === setting ? siteSettingsDecision.value : undefined;
    CURRENT_SETTING = setting;

    siteSettingsTitle.innerText = SITE_SETTINGS[setting].title;
    populatePolicySelect(siteSettingsDecision, selectedPolicy);
    renderSiteSettings();

    setTimeout(() => siteSettingsInput.focus());
}

function addSiteSettingFilter() {
    let filter = ensureURLHasScheme(siteSettingsInput.value);
    if (!filter) {
        return;
    }

    const url = URL.parse(filter);
    if (!url || url.origin === "null") {
        siteSettingsInput.setCustomValidity("Enter a site URL, such as https://example.com.");
        siteSettingsInput.reportValidity();
        return;
    }
    filter = url.origin;

    ladybird.sendMessage("addSiteSettingFilter", {
        setting: CURRENT_SETTING,
        filter,
        policy: siteSettingsDecision.value,
    });

    siteSettingsInput.classList.add("success");
    siteSettingsInput.value = "";

    setTimeout(() => {
        siteSettingsInput.classList.remove("success");
    }, 1000);

    setTimeout(() => siteSettingsInput.focus());
}

siteSettingsAdd.addEventListener("click", addSiteSettingFilter);

siteSettingsInput.addEventListener("input", () => {
    siteSettingsInput.setCustomValidity("");
});

siteSettingsInput.addEventListener("keydown", event => {
    if (event.key === "Enter") {
        addSiteSettingFilter();
    }
});

siteSettingsClose.addEventListener("click", () => {
    siteSettings.close();
});

siteSettingsRemoveAll.addEventListener("click", () => {
    ladybird.sendMessage("removeAllSiteSettingFilters", { setting: CURRENT_SETTING });
});

for (const setting of Object.keys(SITE_SETTINGS)) {
    registerDialogDeepLink({
        hash: setting,
        tab: "permissions",
        dialog: siteSettings,
        onOpen: () => showSiteSettings(setting),
    });

    const control = document.querySelector(`#${setting}-policy`);
    control.addEventListener("change", () => {
        ladybird.sendMessage("setSiteSettingPolicy", {
            setting,
            policy: control.type === "checkbox" ? (control.checked ? "ask" : "block") : control.value,
        });
    });

    document.querySelector(`#${setting}-settings`).addEventListener("click", () => {
        location.hash = setting;
    });
}

document.addEventListener("WebUILoaded", () => {
    ladybird.sendMessage("loadForciblyEnabledSiteSettings");
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadSiteSettings(event.detail.data);
    } else if (event.detail.name === "forciblyEnableSiteSettings") {
        forciblyEnableSiteSettings(event.detail.data);
    } else if (event.detail.name === "loadFeatures") {
        for (const setting of Object.keys(SITE_SETTINGS)) {
            if (Object.hasOwn(event.detail.data, setting)) {
                document.querySelector(`#${setting}-policy`).disabled = event.detail.data[setting] !== true;
            }
        }
    }
});
