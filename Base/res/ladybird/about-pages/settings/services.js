const allowServicesNetworkAccess = document.querySelector("#allow-services-network-access");
const filterListUpdates = document.querySelector("#filter-list-updates");

allowServicesNetworkAccess.addEventListener("change", () => {
    ladybird.sendMessage("setServicesNetworkAccessEnabled", allowServicesNetworkAccess.checked);
});
filterListUpdates.addEventListener("change", () => {
    ladybird.sendMessage("setFilterListUpdatesEnabled", filterListUpdates.checked);
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name !== "loadSettings") return;
    const settings = event.detail.data.backgroundNetworking;
    allowServicesNetworkAccess.checked = settings.enabled;
    filterListUpdates.checked = settings.features.contentBlockerSubscriptionUpdates;
    filterListUpdates.disabled = !settings.enabled;
    document.querySelector("#services-paused-note").classList.toggle("hidden", settings.enabled);
});
