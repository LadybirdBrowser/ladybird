const allowServicesNetworkAccess = document.querySelector("#allow-services-network-access");
const filterListUpdates = document.querySelector("#filter-list-updates");
const updateChecks = document.querySelector("#update-checks");

allowServicesNetworkAccess.addEventListener("change", () => {
    ladybird.sendMessage("setServicesNetworkAccessEnabled", allowServicesNetworkAccess.checked);
});
filterListUpdates.addEventListener("change", () => {
    ladybird.sendMessage("setFilterListUpdatesEnabled", filterListUpdates.checked);
});
updateChecks.addEventListener("change", () => {
    ladybird.sendMessage("setUpdateChecksEnabled", updateChecks.checked);
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name !== "loadSettings") {
        return;
    }
    if (!Object.hasOwn(event.detail.data, "backgroundNetworking")) {
        return;
    }

    const settings = event.detail.data.backgroundNetworking;
    allowServicesNetworkAccess.checked = settings.enabled;
    filterListUpdates.checked = settings.features.contentBlockerSubscriptionUpdates;
    filterListUpdates.disabled = !settings.enabled;
    if (Object.hasOwn(settings.features, "updateChecks")) {
        updateChecks.checked = settings.features.updateChecks;
        updateChecks.disabled = !settings.enabled;
    } else {
        updateChecks.checked = false;
        updateChecks.disabled = true;
        document.querySelector("#update-checks-description").textContent =
            "Updates for this build of Ladybird are managed externally.";
    }
    document.querySelector("#services-paused-note").classList.toggle("hidden", settings.enabled);
});
