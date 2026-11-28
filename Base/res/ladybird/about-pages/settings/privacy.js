import { getByteFormatter } from "../../utils.js";
import { registerDialogDeepLink } from "./dialog-deep-link.js";

const byteFormatter = getByteFormatter(unit => {
    return {
        unitDisplay: unit === "byte" ? "long" : "short",
        maximumFractionDigits: 1,
    };
});

const browsingDataSettings = document.querySelector("#browsing-data-settings");
const browsingDataSettingsClose = document.querySelector("#browsing-data-settings-close");
const browsingDataSettingsDialog = document.querySelector("#browsing-data-settings-dialog");
const browsingDataTotalSize = document.querySelector("#browsing-data-total-size");

const clearBrowsingDataCachedFiles = document.querySelector("#clear-browsing-data-cached-files");
const clearBrowsingDataCachedFilesSize = document.querySelector("#clear-browsing-data-cached-files-size");
const clearBrowsingDataDownloadHistory = document.querySelector("#clear-browsing-data-download-history");
const clearBrowsingDataHistory = document.querySelector("#clear-browsing-data-history");
const clearBrowsingDataRemoveData = document.querySelector("#clear-browsing-data-remove-data");
const clearBrowsingDataSiteData = document.querySelector("#clear-browsing-data-site-data");
const clearBrowsingDataSiteDataSize = document.querySelector("#clear-browsing-data-site-data-size");
const clearBrowsingDataTimeRange = document.querySelector("#clear-browsing-data-time-range");

const globalPrivacyControlToggle = document.querySelector("#global-privacy-control-toggle");

const passkeysDescription = document.querySelector("#passkeys-description");
const passkeysList = document.querySelector("#passkeys-list");
const passkeysShow = document.querySelector("#passkeys-show");

function loadSettings(settings) {
    if (Object.hasOwn(settings, "globalPrivacyControl")) {
        globalPrivacyControlToggle.checked = settings.globalPrivacyControl;
    }
}

function updateBrowsingDataSizes(sizes) {
    const totalSize = sizes.totalCacheSize + sizes.totalSiteDataSize;

    browsingDataTotalSize.innerText = `Your browsing data is currently using ${byteFormatter.formatBytes(totalSize)} of disk space`;

    clearBrowsingDataCachedFilesSize.innerText = ` (remove ${byteFormatter.formatBytes(sizes.cacheSizeSinceRequestedTime)})`;
    clearBrowsingDataSiteDataSize.innerText = ` (remove ${byteFormatter.formatBytes(sizes.siteDataSizeSinceRequestedTime)})`;
}

function computeTimeRange() {
    const now = Temporal.Now.zonedDateTimeISO();

    switch (clearBrowsingDataTimeRange.value) {
        case "lastHour":
            return now.subtract({ hours: 1 });
        case "last4Hours":
            return now.subtract({ hours: 4 });
        case "today":
            return now.startOfDay();
        case "all":
            return null;
        default:
            console.error(`Unrecognized time range: ${clearBrowsingDataTimeRange.value}`);
            return now;
    }
}

function estimateBrowsingDataSizes() {
    const since = computeTimeRange();

    ladybird.sendMessage("estimateBrowsingDataSizes", {
        since: since?.epochMilliseconds,
    });
}

registerDialogDeepLink({
    hash: "clearBrowsingData",
    tab: "privacy",
    dialog: browsingDataSettingsDialog,
    onOpen: estimateBrowsingDataSizes,
});

browsingDataSettings.addEventListener("click", () => {
    location.hash = "clearBrowsingData";
});

browsingDataSettingsClose.addEventListener("click", () => {
    browsingDataSettingsDialog.close();
});

clearBrowsingDataTimeRange.addEventListener("change", () => {
    estimateBrowsingDataSizes();
});

function setRemoveDataEnabledState() {
    clearBrowsingDataRemoveData.disabled =
        !clearBrowsingDataCachedFiles.checked &&
        !clearBrowsingDataHistory.checked &&
        !clearBrowsingDataDownloadHistory.checked &&
        !clearBrowsingDataSiteData.checked;
}

clearBrowsingDataCachedFiles.addEventListener("change", setRemoveDataEnabledState);
clearBrowsingDataHistory.addEventListener("change", setRemoveDataEnabledState);
clearBrowsingDataDownloadHistory.addEventListener("change", setRemoveDataEnabledState);
clearBrowsingDataSiteData.addEventListener("change", setRemoveDataEnabledState);

clearBrowsingDataRemoveData.addEventListener("click", () => {
    const since = computeTimeRange();

    ladybird.sendMessage("clearBrowsingData", {
        since: since?.epochMilliseconds,
        cachedFiles: clearBrowsingDataCachedFiles.checked,
        history: clearBrowsingDataHistory.checked,
        downloadHistory: clearBrowsingDataDownloadHistory.checked,
        siteData: clearBrowsingDataSiteData.checked,
    });

    browsingDataSettingsDialog.close();
});

// NB: The list is loaded only when the user asks for it, as the platform can ask the user before it gives the passkeys.
passkeysShow.addEventListener("click", () => {
    ladybird.sendMessage("loadPasskeys");
});

const dateFormatter = new Intl.DateTimeFormat(undefined, { dateStyle: "medium" });

function updatePasskeys(data) {
    passkeysList.replaceChildren();
    if (!data.available) {
        passkeysDescription.textContent = "Ladybird cannot keep passkeys on this device yet.";
        passkeysShow.disabled = true;
        return;
    }
    // NB: The control stays after a read that failed, so that the user can try again.
    if (data.failed) {
        const failure = document.createElement("p");
        failure.className = "card-group description";
        failure.textContent = "Ladybird could not read the passkeys on this device. Try again.";
        passkeysList.appendChild(failure);
        return;
    }
    passkeysShow.hidden = true;

    if (data.passkeys.length === 0) {
        const empty = document.createElement("p");
        empty.className = "card-group description";
        empty.textContent = "There are no passkeys in Ladybird.";
        passkeysList.appendChild(empty);
        return;
    }

    for (const passkey of data.passkeys) {
        const row = document.createElement("div");
        row.className = "card-group inline-container control-row";

        const label = document.createElement("label");
        label.textContent = passkey.site;
        const account = document.createElement("p");
        account.className = "description";
        account.textContent = passkey.account || "No account name";
        label.appendChild(account);

        const dates = [];
        if (Object.hasOwn(passkey, "created")) {
            dates.push(`Created ${dateFormatter.format(new Date(passkey.created))}`);
        }
        if (Object.hasOwn(passkey, "lastUsed")) {
            dates.push(`Last used ${dateFormatter.format(new Date(passkey.lastUsed))}`);
        }
        if (dates.length > 0) {
            const usage = document.createElement("p");
            usage.className = "description";
            usage.textContent = dates.join(" · ");
            label.appendChild(usage);
        }

        const remove = document.createElement("button");
        remove.className = "secondary-button";
        remove.textContent = "Remove";
        const name = passkey.account ? `${passkey.account} on ${passkey.site}` : passkey.site;
        remove.setAttribute("aria-label", `Remove the passkey for ${name}`);
        remove.addEventListener("click", () => {
            if (!window.confirm(`Remove the passkey for ${name}? You can then no longer sign in with it.`)) {
                return;
            }
            ladybird.sendMessage("removePasskey", passkey.id);
        });

        row.append(label, remove);
        passkeysList.appendChild(row);
    }
}

globalPrivacyControlToggle.addEventListener("change", () => {
    ladybird.sendMessage("setGlobalPrivacyControl", globalPrivacyControlToggle.checked);
});

document.addEventListener("WebUILoaded", estimateBrowsingDataSizes);

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    } else if (event.detail.name === "estimatedBrowsingDataSizes") {
        updateBrowsingDataSizes(event.detail.data);
    } else if (event.detail.name === "clearedBrowsingData") {
        estimateBrowsingDataSizes();
    } else if (event.detail.name === "loadPasskeys") {
        updatePasskeys(event.detail.data);
    } else if (event.detail.name === "passkeyRemovalFailed") {
        window.alert("Ladybird could not remove the passkey. It is still on this device.");
    }
});
