import { registerDialogDeepLink } from "./dialog-deep-link.js";

const PERMISSIONS = {
    geolocation: { hash: "location-permissions", button: "#location-permissions-settings" },
    microphone: { hash: "microphone-permissions", button: "#microphone-permissions-settings" },
};

let SITE_PERMISSIONS = { geolocation: [], microphone: [] };

function dialogFor(permission) {
    return document.querySelector(`#site-permissions-${permission}`);
}

function decisionOption(value, label) {
    const option = document.createElement("option");
    option.value = value;
    option.textContent = label;
    return option;
}

function renderSitePermissions(permission) {
    const list = dialogFor(permission).querySelector(".site-permissions-list");
    list.replaceChildren();

    const sites = SITE_PERMISSIONS[permission] ?? [];

    if (sites.length === 0) {
        const placeholder = document.createElement("div");
        placeholder.className = "dialog-list-item-placeholder";
        placeholder.textContent = "No sites yet";
        list.append(placeholder);
    }

    sites.forEach(site => {
        const row = document.createElement("div");
        row.classList.add("dialog-list-item", "site-permission-row");
        row.dataset.origin = site.origin;

        const origin = document.createElement("span");
        origin.className = "dialog-list-item-label";
        origin.textContent = site.origin;

        const select = document.createElement("select");
        select.append(decisionOption("allow", "Allow"));
        if (site.decision === "allow-once") {
            select.append(decisionOption("allow-once", "Allow this time"));
        }
        select.append(decisionOption("block", "Block"));
        select.value = site.decision;
        select.addEventListener("change", () => {
            ladybird.sendMessage("setSitePermission", { permission, origin: site.origin, decision: select.value });
        });

        const remove = document.createElement("button");
        remove.className = "dialog-button";
        remove.innerHTML = "&times;";
        remove.title = `Remove ${site.origin}`;
        remove.addEventListener("click", () => {
            ladybird.sendMessage("setSitePermission", { permission, origin: site.origin, decision: null });
        });

        row.append(origin, select, remove);
        list.append(row);
    });
}

for (const [permission, { hash, button }] of Object.entries(PERMISSIONS)) {
    const dialog = dialogFor(permission);

    document.querySelector(button).addEventListener("click", () => {
        location.hash = hash;
    });
    dialog.querySelector(".close-button").addEventListener("click", () => {
        dialog.close();
    });
    registerDialogDeepLink({
        hash,
        tab: "permissions",
        dialog,
        onOpen: () => renderSitePermissions(permission),
    });
}

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name !== "loadSettings") {
        return;
    }
    if (!Object.hasOwn(event.detail.data, "sitePermissions")) {
        return;
    }

    SITE_PERMISSIONS = event.detail.data.sitePermissions;
    for (const permission of Object.keys(PERMISSIONS)) {
        if (dialogFor(permission).open) {
            renderSitePermissions(permission);
        }
    }
});
