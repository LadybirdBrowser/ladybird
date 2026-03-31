export class ConsolePanel {
    constructor(transport, client) {
        this.transport = transport;
        this.client = client;
        this.ui = new ConsoleUI(document.getElementById("output"));
        this.history = new CommandHistory();

        this._setupInput();
        this._setupTransport();
    }

    _setupInput() {
        const input = document.getElementById("input");

        document.getElementById("clear-btn").addEventListener("click", () => {
            this.ui.clear();
        });

        input.addEventListener("keydown", e => {
            if (e.key === "Enter" && !e.shiftKey) {
                e.preventDefault();
                const text = input.value.trim();
                if (!text) return;

                this.history.push(text);
                this.ui.appendInput(text);
                this.evaluate(text);
                input.value = "";
            } else if (e.key === "ArrowUp") {
                e.preventDefault();
                input.value = this.history.up();
            } else if (e.key === "ArrowDown") {
                e.preventDefault();
                input.value = this.history.down();
            }
        });
    }

    _setupTransport() {
        this.transport.addListener(msg => {
            if (msg.type === "evaluationResult") {
                if (msg.exceptionMessage) {
                    this.ui.appendError(msg);
                } else {
                    this.ui.appendResult(msg);
                }
            }

            if (msg.type === "resources-available-array" && msg.array) {
                for (const resource of msg.array) {
                    if (resource.resourceType === "console-message") {
                        this.ui.appendConsoleMessage(resource);
                    } else if (resource.resourceType === "error-message") {
                        this.ui.appendConsoleMessage({
                            level: "error",
                            message: resource.message || resource.text,
                        });
                    }
                }
            }
        });
    }

    evaluate(text) {
        if (!this.client.consoleActor) return;
        this.transport.send({
            to: this.client.consoleActor,
            type: "evaluateJSAsync",
            text,
            eager: false,
        });
    }
}

class ConsoleUI {
    constructor(outputEl) {
        this.outputEl = outputEl;
    }

    appendInput(text) {
        const el = document.createElement("div");
        el.className = "entry input-echo";
        el.textContent = text;
        this.outputEl.appendChild(el);
        this.#scrollToBottom();
    }

    appendResult(msg) {
        const el = document.createElement("div");
        const formatted = formatValue(msg.result);
        const isUndefined = formatted === "undefined";
        el.className = isUndefined ? "entry result result-undefined" : "entry result";
        el.textContent = formatted;
        this.outputEl.appendChild(el);
        this.#scrollToBottom();
    }

    appendError(msg) {
        this.#append("error", msg.exceptionMessage || msg.message || "Error");
    }

    appendConsoleMessage(resource) {
        const level = resource.level || "log";
        this.#append(
            level,
            resource.arguments ? resource.arguments.map(a => formatValue(a)).join(" ") : resource.message
        );
    }

    clear() {
        this.outputEl.innerHTML = "";
    }

    #append(type, text) {
        const el = document.createElement("div");
        el.className = `entry ${type}`;
        el.textContent = text;
        this.outputEl.appendChild(el);
        this.#scrollToBottom();
    }

    #scrollToBottom() {
        this.outputEl.scrollTop = this.outputEl.scrollHeight;
    }
}

class CommandHistory {
    constructor() {
        this.entries = [];
        this.index = -1;
    }

    push(cmd) {
        if (cmd && (this.entries.length === 0 || this.entries[this.entries.length - 1] !== cmd)) {
            this.entries.push(cmd);
        }
        this.index = this.entries.length;
    }

    up() {
        if (this.index > 0) {
            this.index--;
            return this.entries[this.index];
        }
        return this.entries[0] || "";
    }

    down() {
        if (this.index < this.entries.length - 1) {
            this.index++;
            return this.entries[this.index];
        }
        this.index = this.entries.length;
        return "";
    }
}

function formatValue(value) {
    if (value === undefined) return "undefined";
    if (value === null) return "null";
    if (typeof value === "object") {
        if (value.type === "undefined") return "undefined";
        if (value.type === "null") return "null";
        if (value.type === "NaN") return "NaN";
        if (value.type === "Infinity") return "Infinity";
        if (value.type === "-Infinity") return "-Infinity";
        if (value.type === "-0") return "-0";
        if (value.type === "BigInt") return `${value.text}n`;
        try {
            return JSON.stringify(value);
        } catch {
            return String(value);
        }
    }
    return String(value);
}
