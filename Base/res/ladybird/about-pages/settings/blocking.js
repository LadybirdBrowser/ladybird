const contentBlockerLists = document.querySelector("#content-blocker-lists");
const languageLists = document.querySelector("#language-content-blocker-lists");
const updateLists = document.querySelector("#update-content-blocker-lists");
const addFilterList = document.querySelector("#add-filter-list");
const addFilterListForm = document.querySelector("#add-filter-list-form");
const subscriptionUrl = document.querySelector("#custom-content-blocker-subscription-url");
const addSubscription = document.querySelector("#add-content-blocker-subscription");
const subscriptionStatus = document.querySelector("#custom-content-blocker-subscription-status");
const localListPicker = document.querySelector("#local-content-blocker-list-picker");
const importLocalList = document.querySelector("#import-local-content-blocker-list");
const localListStatus = document.querySelector("#local-content-blocker-list-status");
const disabledSitesSection = document.querySelector("#content-blocker-disabled-sites-section");
const disabledSites = document.querySelector("#content-blocker-disabled-sites");
const customFilters = document.querySelector("#custom-content-blocker-filters");
const saveCustomFilters = document.querySelector("#save-custom-content-blocker-filters");
const customFiltersStatus = document.querySelector("#custom-content-blocker-filters-status");
const enableContentBlocking = document.querySelector("#enable-content-blocking");

const maximumCustomFilterSize = 4 * 1024 * 1024;

let updateTimer;
let savedCustomFilters = "";

function createListRow(list, removable = false) {
    const row = document.querySelector("#filter-list-row").content.firstElementChild.cloneNode(true);
    const toggle = row.querySelector("input");
    const label = row.querySelector("label");
    const labelText = row.querySelector(".label");
    const details = row.querySelector(".description");

    const id = `content-blocker-list-${list.identifier}`;
    const name = list.name ?? list.url;

    toggle.id = id;
    label.htmlFor = id;
    labelText.textContent = name;

    // A list added by the user has no description of its own, so it is described by where it comes from.
    details.textContent = list.description || (list.url ? new URL(list.url).host : "Local file");

    toggle.checked = list.enabled;
    toggle.addEventListener("change", () => {
        ladybird.sendMessage("setContentBlockerListEnabled", { identifier: list.identifier, enabled: toggle.checked });
    });

    const remove = row.querySelector("button");
    if (removable) {
        remove.setAttribute("aria-label", `Remove ${name}`);
        remove.addEventListener("click", () => ladybird.sendMessage("removeContentBlockerList", list.identifier));
    } else {
        remove.remove();
    }

    return row;
}

function createDisabledSiteRow(site) {
    const row = document.createElement("div");

    const tile = document.createElement("span");
    tile.className = "letter-tile";
    tile.setAttribute("aria-hidden", "true");
    tile.textContent = site.charAt(0).toUpperCase();

    const name = document.createElement("span");
    name.className = "item-name";
    name.textContent = site;

    const turnBackOn = document.createElement("button");
    turnBackOn.className = "link-button";
    turnBackOn.textContent = "Turn back on";
    turnBackOn.setAttribute("aria-label", `Turn content blocking back on for ${site}`);
    turnBackOn.addEventListener("click", () => {
        ladybird.sendMessage("setContentBlockerEnabledForSite", { site, enabled: true });
    });

    row.append(tile, name, turnBackOn);
    return row;
}

function loadSettings(settings) {
    if (Object.hasOwn(settings, "contentBlockers")) {
        const contentBlockers = settings.contentBlockers;
        enableContentBlocking.checked = contentBlockers.enabled;

        if (customFilters.value === savedCustomFilters) {
            customFilters.value = contentBlockers.customFilters;
        }

        savedCustomFilters = contentBlockers.customFilters;

        const sites = [...(contentBlockers.disabledSites ?? [])].sort();
        disabledSites.replaceChildren(...sites.map(createDisabledSiteRow));
        disabledSitesSection.hidden = sites.length === 0;
    }

    if (Object.hasOwn(settings, "contentBlockerLists")) {
        contentBlockerLists.replaceChildren();
        languageLists.replaceChildren();

        for (const list of settings.contentBlockerLists) {
            const container = list.builtIn && list.languageSpecific ? languageLists : contentBlockerLists;
            container.append(createListRow(list, !list.builtIn));
        }
    }

    if (Object.hasOwn(settings, "contentBlockerListUpdateInProgress")) {
        updateLists.disabled = settings.contentBlockerListUpdateInProgress;
        updateLists.innerText = settings.contentBlockerListUpdateInProgress ? "Updating…" : "Update enabled lists";

        clearInterval(updateTimer);
        if (settings.contentBlockerListUpdateInProgress) {
            updateTimer = setInterval(() => ladybird.sendMessage("refreshSettings"), 1000);
        }
    }
}

addFilterList.addEventListener("click", () => {
    addFilterListForm.hidden = !addFilterListForm.hidden;
    addFilterList.setAttribute("aria-expanded", String(!addFilterListForm.hidden));
    if (!addFilterListForm.hidden) subscriptionUrl.focus();
});

enableContentBlocking.addEventListener("change", () => {
    ladybird.sendMessage("setContentBlockerEnabled", enableContentBlocking.checked);
});

addSubscription.addEventListener("click", () => {
    if (!subscriptionUrl.reportValidity() || subscriptionUrl.value.length === 0) {
        return;
    }
    const url = new URL(subscriptionUrl.value);
    if (url.protocol !== "http:" && url.protocol !== "https:") {
        subscriptionStatus.innerText = "Enter an HTTP or HTTPS URL.";
        return;
    }
    ladybird.sendMessage("addCustomContentBlockerSubscription", subscriptionUrl.value);
    subscriptionUrl.value = "";
});

updateLists.addEventListener("click", () => {
    ladybird.sendMessage("updateContentBlockerLists");
});

importLocalList.addEventListener("click", () => localListPicker.click());
localListPicker.addEventListener("change", async () => {
    const file = localListPicker.files[0];
    if (!file) {
        return;
    }
    if (file.size > 64 * 1024 * 1024) {
        localListStatus.innerText = "The selected list is larger than 64 MiB.";
        localListPicker.value = "";
        return;
    }
    const data = {
        name: file.name,
        contents: await file.text(),
    };
    // IPC's JSON serializer uses six-byte escapes for carriage returns and form feeds.
    const serialized = JSON.stringify(data, (_, value) =>
        typeof value === "string" ? value.replace(/[\r\f]/g, "\0") : value
    );
    // Leave room for the IPC message headers and method name.
    const maximumPayloadSize = 64 * 1024 * 1024 - 1024;
    if (
        serialized.length > maximumPayloadSize ||
        new TextEncoder().encode(serialized).byteLength > maximumPayloadSize
    ) {
        localListStatus.innerText = "The encoded list is too large to import.";
        localListPicker.value = "";
        return;
    }
    ladybird.sendMessage("importLocalContentBlockerList", data);
    localListPicker.value = "";
});

saveCustomFilters.addEventListener("click", () => {
    if (new TextEncoder().encode(customFilters.value).byteLength > maximumCustomFilterSize) {
        customFiltersStatus.innerText = "Custom filters must be 4 MiB or smaller.";
        customFilters.focus();
        return;
    }
    ladybird.sendMessage("setCustomContentBlockerFilters", customFilters.value);
    customFiltersStatus.innerText = "Saved";
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    } else if (event.detail.name === "contentBlockerResult") {
        const result = event.detail.data;
        const status = result.operation === "subscription" ? subscriptionStatus : localListStatus;
        status.innerText = result.message;
    }
});
