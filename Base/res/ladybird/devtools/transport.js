export class DevToolsTransport {
    constructor() {
        this._listeners = new Set();
        document.addEventListener("WebUIMessage", e => {
            if (e.detail.name !== "devtools.receive") return;
            for (const listener of this._listeners) listener(e.detail.data);
        });
    }

    send(message) {
        ladybird.sendMessage("devtools.send", message);
    }

    addListener(fn) {
        this._listeners.add(fn);
    }

    removeListener(fn) {
        this._listeners.delete(fn);
    }
}

export class DevToolsClient {
    constructor(transport, { browserId = null } = {}) {
        this.transport = transport;
        this.browserId = browserId;
        this.consoleActor = null;
        this.inspectorActor = null;
        this.tabActor = null;
        this.frameActor = null;
        this._watcherActor = null;

        this._pendingResponses = new Map();
        this.transport.addListener(msg => this._dispatch(msg));
    }

    async connect() {
        let tab = null;

        if (this.browserId !== null) {
            const tabResponse = await this.request("root", "getTab", {
                browserId: this.browserId,
            });
            tab = tabResponse.tab ?? null;
        }

        if (!tab) {
            const tabsResponse = await this.request("root", "listTabs");
            if (!tabsResponse.tabs || tabsResponse.tabs.length === 0) throw new Error("No tabs available");
            tab = tabsResponse.tabs[0];
        }

        this.tabActor = tab.actor;

        const watcherResponse = await this.request(tab.actor, "getWatcher", {
            isServerTargetSwitchingEnabled: true,
        });
        this._watcherActor = watcherResponse.actor;

        const targetResponse = await this.request(this._watcherActor, "watchTargets", {
            targetType: "frame",
        });

        if (targetResponse.target) {
            this.frameActor = targetResponse.target.actor;
            this.consoleActor = targetResponse.target.consoleActor;
            this.inspectorActor = targetResponse.target.inspectorActor;
        }

        await this.request(this._watcherActor, "watchResources", {
            resourceTypes: ["console-message", "error-message"],
        });
    }

    request(to, type, params = {}) {
        return new Promise(resolve => {
            this._pendingResponses.set(to, resolve);
            this.transport.send({ to, type, ...params });
        });
    }

    _dispatch(msg) {
        // Match responses by "from" field
        if (msg.from && this._pendingResponses.has(msg.from)) {
            const resolve = this._pendingResponses.get(msg.from);
            this._pendingResponses.delete(msg.from);
            resolve(msg);
        }
    }
}

export function sendWebUIMessage(name, responseName, data) {
    return new Promise(resolve => {
        const handler = event => {
            if (event.detail.name !== responseName) return;
            document.removeEventListener("WebUIMessage", handler);
            resolve(event.detail.data);
        };

        document.addEventListener("WebUIMessage", handler);
        ladybird.sendMessage(name, data);
    });
}
