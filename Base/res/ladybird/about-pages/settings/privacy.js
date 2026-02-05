import { getByteFormatter } from "../../utils.js";

const byteFormatter = getByteFormatter(unit => {
    return {
        unitDisplay: unit === "byte" ? "long" : "short",
        maximumFractionDigits: 1,
    };
});

const browsingDataSettings = document.querySelector("#browsing-data-settings");
const browsingDataSettingsClose = document.querySelector("#browsing-data-settings-close");
const browsingDataSettingsDialog = document.querySelector("#browsing-data-settings-dialog");
const browsingDataSettingsMaxDiskCacheSize = document.querySelector("#browsing-data-settings-max-disk-cache-size");
const browsingDataSettingsMaxDiskCacheUnit = document.querySelector("#browsing-data-settings-max-disk-cache-unit");
const browsingDataTotalSize = document.querySelector("#browsing-data-total-size");

const clearBrowsingDataCachedFiles = document.querySelector("#clear-browsing-data-cached-files");
const clearBrowsingDataCachedFilesSize = document.querySelector("#clear-browsing-data-cached-files-size");
const clearBrowsingDataRemoveData = document.querySelector("#clear-browsing-data-remove-data");
const clearBrowsingDataSiteData = document.querySelector("#clear-browsing-data-site-data");
const clearBrowsingDataSiteDataSize = document.querySelector("#clear-browsing-data-site-data-size");
const clearBrowsingDataTimeRange = document.querySelector("#clear-browsing-data-time-range");

const globalPrivacyControlToggle = document.querySelector("#global-privacy-control-toggle");

const MiB = 1024 * 1024;
const GiB = MiB * 1024;

let BROWSING_DATA = {};

function loadSettings(settings) {
    BROWSING_DATA = settings.browsingData || {};

    globalPrivacyControlToggle.checked = settings.globalPrivacyControl;

    if (browsingDataSettingsDialog.open) {
        showBrowsingDataSettings();
    }
}

function updateBrowsingDataSizes(sizes) {
    const totalSize = sizes.totalCacheSize + sizes.totalSiteDataSize;

    browsingDataTotalSize.innerText = `Your browsing data is currently using ${byteFormatter.formatBytes(totalSize)} of disk space`;

    clearBrowsingDataCachedFilesSize.innerText = ` (remove ${byteFormatter.formatBytes(sizes.cacheSizeSinceRequestedTime)})`;
    clearBrowsingDataSiteDataSize.innerText = ` (remove ${byteFormatter.formatBytes(sizes.siteDataSizeSinceRequestedTime)})`;
}

function formatDiskCacheSize(bytes) {
    if (bytes >= GiB && bytes % GiB == 0) {
        return { value: bytes / GiB, unit: "GiB" };
    }

    return { value: bytes / MiB, unit: "MiB" };
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

function saveBrowsingDataSettings() {
    browsingDataSettingsMaxDiskCacheSize.classList.remove("success");
    browsingDataSettingsMaxDiskCacheSize.classList.remove("error");

    if (
        browsingDataSettingsMaxDiskCacheSize.value.length === 0 ||
        !browsingDataSettingsMaxDiskCacheSize.checkValidity()
    ) {
        browsingDataSettingsMaxDiskCacheSize.classList.add("error");
        return;
    }

    BROWSING_DATA.diskCache = {};
    BROWSING_DATA.diskCache.maxSize =
        browsingDataSettingsMaxDiskCacheUnit.value === "MiB"
            ? browsingDataSettingsMaxDiskCacheSize.value * MiB
            : browsingDataSettingsMaxDiskCacheSize.value * GiB;

    ladybird.sendMessage("setBrowsingDataSettings", BROWSING_DATA);
    browsingDataSettingsMaxDiskCacheSize.classList.add("success");

    setTimeout(() => {
        browsingDataSettingsMaxDiskCacheSize.classList.remove("success");
    }, 1000);
}

function showBrowsingDataSettings() {
    const maxDiskCacheSize = BROWSING_DATA.diskCache?.maxSize || 5 * GiB;

    const { value, unit } = formatDiskCacheSize(maxDiskCacheSize);
    browsingDataSettingsMaxDiskCacheSize.value = value;
    browsingDataSettingsMaxDiskCacheUnit.value = unit;

    if (!browsingDataSettingsDialog.open) {
        browsingDataSettingsDialog.showModal();
    }
}

browsingDataSettings.addEventListener("click", () => {
    estimateBrowsingDataSizes();
    showBrowsingDataSettings();
});

browsingDataSettingsClose.addEventListener("click", () => {
    browsingDataSettingsDialog.close();
});

browsingDataSettingsMaxDiskCacheSize.addEventListener("change", () => {
    saveBrowsingDataSettings();
});
browsingDataSettingsMaxDiskCacheUnit.addEventListener("change", () => {
    saveBrowsingDataSettings();
});

clearBrowsingDataTimeRange.addEventListener("change", () => {
    estimateBrowsingDataSizes();
});

function setRemoveDataEnabledState() {
    clearBrowsingDataRemoveData.disabled = !clearBrowsingDataCachedFiles.checked && !clearBrowsingDataSiteData.checked;
}

clearBrowsingDataCachedFiles.addEventListener("change", setRemoveDataEnabledState);
clearBrowsingDataSiteData.addEventListener("change", setRemoveDataEnabledState);

clearBrowsingDataRemoveData.addEventListener("click", () => {
    const since = computeTimeRange();

    ladybird.sendMessage("clearBrowsingData", {
        since: since?.epochMilliseconds,
        cachedFiles: clearBrowsingDataCachedFiles.checked,
        siteData: clearBrowsingDataSiteData.checked,
    });

    browsingDataSettingsDialog.close();
});

globalPrivacyControlToggle.addEventListener("change", () => {
    ladybird.sendMessage("setGlobalPrivacyControl", globalPrivacyControlToggle.checked);
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    } else if (event.detail.name === "estimatedBrowsingDataSizes") {
        updateBrowsingDataSizes(event.detail.data);
    }
});
