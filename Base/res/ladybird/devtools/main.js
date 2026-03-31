import { DevToolsTransport, DevToolsClient, sendWebUIMessage } from "./transport.js";
import { ConsolePanel } from "./console-pane.js";
import { ElementsPanel } from "./elements-pane.js";
import { SettingsPanel } from "./settings-pane.js";

document.addEventListener("WebUILoaded", async () => {
    const transport = new DevToolsTransport();
    const browserId = await sendWebUIMessage("devtools.getInspectedBrowserId", "devtools.inspectedBrowserId");
    const client = new DevToolsClient(transport, {
        browserId: Number.isInteger(browserId) ? browserId : null,
    });
    new ConsolePanel(transport, client);
    const elementsPanel = new ElementsPanel(client);
    const settingsPanel = new SettingsPanel();
    initializeTabs();

    const tree = document.getElementById("dom-tree");
    tree.textContent = "Connecting\u2026";
    settingsPanel.initialize();

    try {
        await client.connect();
        transport.addListener(msg => {
            if (msg.type !== "tabNavigated" || msg.from !== client.frameActor) {
                return;
            }

            if (msg.state === "start") {
                tree.textContent = "Loading DOM tree\u2026";
                return;
            }

            if (msg.state === "stop") {
                void elementsPanel.reload();
            }
        });
        await elementsPanel.initialize();
    } catch (err) {
        tree.textContent = `Connection failed: ${err.message}`;
    }
});

function initializeTabs() {
    const buttons = document.querySelectorAll(".tab-button");
    const panels = document.querySelectorAll(".panel");

    for (const button of buttons) {
        button.addEventListener("click", () => {
            const panelId = `panel-${button.dataset.panel}`;
            for (const item of buttons) item.classList.toggle("active", item === button);
            for (const panel of panels) panel.hidden = panel.id !== panelId;
        });
    }
}
