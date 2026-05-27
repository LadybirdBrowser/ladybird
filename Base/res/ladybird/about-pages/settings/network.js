const customDnsSettings = document.querySelector("#custom-dns-settings");
const dnsForciblyEnabled = document.querySelector("#dns-forcibly-enabled");

const dnsUpstream = document.querySelector("#dns-upstream");
const dnsType = document.querySelector("#dns-type");
const dnsServer = document.querySelector("#dns-server");
const dnsPort = document.querySelector("#dns-port");
const dnssecToggle = document.querySelector("#dnssec-toggle");

let DNS_SETTINGS = {};
let PROXY_MODE = {};

function loadSettings(settings) {
    DNS_SETTINGS = settings.dnsSettings || {};
    loadDnsSettings();

    PROXY_MODE = settings.proxyMode || "system";
    loadProxyMode();
}

const proxyUpstream = document.querySelector("#proxy-upstream");

function loadProxyMode() {
    proxyUpstream.value = PROXY_MODE || "system";
}

proxyUpstream.addEventListener("change", () => {
    ladybird.sendMessage("setProxyMode", proxyUpstream.value);
});

function loadDnsSettings() {
    dnsUpstream.value = DNS_SETTINGS.mode || "system";

    if (dnsUpstream.value === "custom") {
        dnsType.value = DNS_SETTINGS.type;
        dnsServer.value = DNS_SETTINGS.server;
        dnsPort.value = DNS_SETTINGS.port;
        dnssecToggle.checked = DNS_SETTINGS.dnssec;

        customDnsSettings.classList.remove("hidden");
    } else {
        dnsType.value = "udp";
        dnsServer.value = "";
        dnsPort.value = "53";
        dnssecToggle.checked = false;

        customDnsSettings.classList.add("hidden");
    }

    if (DNS_SETTINGS.forciblyEnabled) {
        dnsForciblyEnabled.classList.remove("hidden");

        dnsUpstream.disabled = true;
        dnsType.disabled = true;
        dnsServer.disabled = true;
        dnsPort.disabled = true;
        dnssecToggle.disabled = true;
    } else {
        dnsForciblyEnabled.classList.add("hidden");

        dnsUpstream.disabled = false;
        dnsType.disabled = false;
        dnsServer.disabled = false;
        dnsPort.disabled = false;
        dnssecToggle.disabled = false;
    }
}

dnsUpstream.addEventListener("change", () => {
    if (dnsUpstream.value === "custom") {
        customDnsSettings.classList.remove("hidden");

        if (dnsServer.value.length !== 0 && dnsPort.value.length !== 0) {
            updateDnsSettings();
        }
    } else {
        customDnsSettings.classList.add("hidden");
        ladybird.sendMessage("setDNSSettings", { mode: "system" });
    }
});

function updateDnsSettings() {
    if (dnsUpstream.value !== "custom") {
        return;
    }

    dnsPort.placeholder = dnsType.value === "tls" ? "853" : "53";

    if ((dnsPort.value || 0) === 0) {
        dnsPort.value = dnsPort.placeholder;
    }

    ladybird.sendMessage("setDNSSettings", {
        mode: "custom",
        type: dnsType.value,
        server: dnsServer.value,
        port: dnsPort.value | 0,
        dnssec: dnssecToggle.checked,
    });
}

dnsServer.addEventListener("change", updateDnsSettings);
dnsPort.addEventListener("change", updateDnsSettings);
dnsType.addEventListener("change", updateDnsSettings);
dnssecToggle.addEventListener("change", updateDnsSettings);

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    }
});
