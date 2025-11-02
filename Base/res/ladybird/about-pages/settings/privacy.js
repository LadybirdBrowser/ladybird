const clearBrowsingData = document.querySelector("#clear-browsing-data");
const clearBrowsingDataCachedFiles = document.querySelector("#clear-browsing-data-cached-files");
const clearBrowsingDataCachedFilesSize = document.querySelector("#clear-browsing-data-cached-files-size");
const clearBrowsingDataClose = document.querySelector("#clear-browsing-data-close");
const clearBrowsingDataDialog = document.querySelector("#clear-browsing-data-dialog");
const clearBrowsingDataRemoveData = document.querySelector("#clear-browsing-data-remove-data");
const clearBrowsingDataSiteData = document.querySelector("#clear-browsing-data-site-data");
const clearBrowsingDataSiteDataSize = document.querySelector("#clear-browsing-data-site-data-size");
const clearBrowsingDataTimeRange = document.querySelector("#clear-browsing-data-time-range");
const clearBrowsingDataTotalSize = document.querySelector("#clear-browsing-data-total-size");
const globalPrivacyControlToggle = document.querySelector("#global-privacy-control-toggle");

const BYTE_UNITS = ["byte", "kilobyte", "megabyte", "gigabyte", "terabyte"];

const BYTE_FORMATTERS = {
    byte: undefined,
    kilobyte: undefined,
    megabyte: undefined,
    gigabyte: undefined,
    terabyte: undefined,
};

function formatBytes(bytes) {
    let index = 0;
    while (bytes >= 1024 && index < BYTE_UNITS.length - 1) {
        bytes /= 1024;
        ++index;
    }

    const unit = BYTE_UNITS[index];

    if (!BYTE_FORMATTERS[unit]) {
        BYTE_FORMATTERS[unit] = new Intl.NumberFormat([], {
            style: "unit",
            unit: unit,
            unitDisplay: unit === "byte" ? "long" : "short",
            maximumFractionDigits: 1,
        });
    }

    return BYTE_FORMATTERS[unit].format(bytes);
}

function loadSettings(settings) {
    globalPrivacyControlToggle.checked = settings.globalPrivacyControl;
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

function updateBrowsingDataSizes(sizes) {
    const totalSize = sizes.totalCacheSize + sizes.totalSiteDataSize;

    clearBrowsingDataTotalSize.innerText = `Your browsing data is currently using ${formatBytes(totalSize)} of disk space`;

    clearBrowsingDataCachedFilesSize.innerText = ` (remove ${formatBytes(sizes.cacheSizeSinceRequestedTime)})`;
    clearBrowsingDataSiteDataSize.innerText = ` (remove ${formatBytes(sizes.siteDataSizeSinceRequestedTime)})`;
}

clearBrowsingData.addEventListener("click", () => {
    estimateBrowsingDataSizes();
    clearBrowsingDataDialog.showModal();
});

clearBrowsingDataTimeRange.addEventListener("change", () => {
    estimateBrowsingDataSizes();
});

clearBrowsingDataClose.addEventListener("click", () => {
    clearBrowsingDataDialog.close();
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

    clearBrowsingDataDialog.close();
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
