import { sendWebUIMessage } from "./transport.js";

export class SettingsPanel {
    constructor() {
        this.enabledEl = document.getElementById("remote-debugging-enabled");
        this.portEl = document.getElementById("remote-debugging-port");
        this.noteEl = document.getElementById("remote-debugging-note");
    }

    initialize() {
        this.enabledEl.addEventListener("change", () => void this.#save());
        this.portEl.addEventListener("change", () => void this.#save());

        void this.#reload();
    }

    #load(settings) {
        this.enabledEl.checked = !!settings.enabled;
        this.portEl.value = `${settings.port || 6000}`;

        const forciblyEnabled = !!settings.forciblyEnabled;
        this.enabledEl.disabled = forciblyEnabled;
        this.#setPortDisabled(forciblyEnabled);
        this.noteEl.hidden = !forciblyEnabled;
        this.noteEl.textContent = forciblyEnabled ? "Controlled by command line" : "";
    }

    async #save() {
        if (!this.portEl.reportValidity()) return;
        this.#setPortDisabled(false);
        this.#load(
            (await sendWebUIMessage("devtools.setRemoteDebuggingSettings", "devtools.remoteDebuggingSettings", {
                enabled: this.enabledEl.checked,
                port: this.portEl.valueAsNumber || 6000,
            })) || {}
        );
    }

    async #reload() {
        this.#load(
            (await sendWebUIMessage("devtools.loadRemoteDebuggingSettings", "devtools.remoteDebuggingSettings")) || {}
        );
    }

    #setPortDisabled(forciblyEnabled) {
        this.portEl.disabled = forciblyEnabled || !this.enabledEl.checked;
    }
}
