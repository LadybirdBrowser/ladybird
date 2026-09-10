const builtInLists = document.querySelector("#built-in-content-blocker-lists");
const languageLists = document.querySelector("#language-content-blocker-lists");
const updateLists = document.querySelector("#update-content-blocker-lists");
const subscriptionUrl = document.querySelector("#custom-content-blocker-subscription-url");
const addSubscription = document.querySelector("#add-content-blocker-subscription");
const customSubscriptions = document.querySelector("#custom-content-blocker-subscriptions");
const subscriptionStatus = document.querySelector("#custom-content-blocker-subscription-status");
const localListPicker = document.querySelector("#local-content-blocker-list-picker");
const importLocalList = document.querySelector("#import-local-content-blocker-list");
const localLists = document.querySelector("#local-content-blocker-lists");
const localListStatus = document.querySelector("#local-content-blocker-list-status");
const customFilters = document.querySelector("#custom-content-blocker-filters");
const saveCustomFilters = document.querySelector("#save-custom-content-blocker-filters");
const customFiltersStatus = document.querySelector("#custom-content-blocker-filters-status");
const maximumCustomFilterSize = 4 * 1024 * 1024;
let updateTimer;
let savedCustomFilters = "";

function listStatus(list) {
    if (list.lastUpdatedAt != null) {
        return `Updated ${new Date(list.lastUpdatedAt).toLocaleString()}`;
    }
    return list.enabled ? "Not downloaded yet" : null;
}

function createListRow(list, removable = false) {
    const row = document.querySelector("#filter-list-row").content.firstElementChild.cloneNode(true);
    const label = row.querySelector("label");
    const toggle = row.querySelector("input");
    const details = row.querySelector(".description");
    const metadata = row.querySelector(".filter-list-metadata");
    const name = list.name ?? list.url;
    label.textContent = name;
    label.htmlFor = toggle.id = `content-blocker-list-${list.identifier}`;
    details.id = `${toggle.id}-description`;
    details.textContent = list.description ?? "";
    metadata.id = `${toggle.id}-metadata`;
    metadata.textContent = [list.url ? new URL(list.url).host : "Local file", list.url && listStatus(list)]
        .filter(Boolean)
        .join(" · ");
    toggle.setAttribute("aria-describedby", `${details.id} ${metadata.id}`);
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

function showEmptyState(container, message) {
    const emptyState = document.createElement("p");
    emptyState.className = "description";
    emptyState.innerText = message;
    container.append(emptyState);
}

function loadSettings(settings) {
    const contentBlockers = settings.contentBlockers;
    if (!contentBlockers) {
        return;
    }

    for (const container of [builtInLists, languageLists, customSubscriptions, localLists]) {
        container.replaceChildren();
    }
    for (const list of settings.contentBlockerLists) {
        const container = list.builtIn
            ? list.languageSpecific
                ? languageLists
                : builtInLists
            : list.url
              ? customSubscriptions
              : localLists;
        container.append(createListRow(list, !list.builtIn));
    }
    if (!customSubscriptions.children.length) showEmptyState(customSubscriptions, "No custom subscriptions added.");
    if (!localLists.children.length) showEmptyState(localLists, "No local lists imported.");

    if (customFilters.value === savedCustomFilters) {
        customFilters.value = contentBlockers.customFilters;
    }
    savedCustomFilters = contentBlockers.customFilters;

    updateLists.disabled = settings.contentBlockerListUpdateInProgress;
    updateLists.innerText = settings.contentBlockerListUpdateInProgress ? "Updating…" : "Update enabled lists";

    clearTimeout(updateTimer);
    if (settings.contentBlockerListUpdateInProgress) {
        updateTimer = setTimeout(() => ladybird.sendMessage("loadCurrentSettings"), 1000);
    }
}

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
    updateLists.disabled = true;
    updateLists.innerText = "Updating…";
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
