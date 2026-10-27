const configFilter = document.querySelector("#config-filter");
const configList = document.querySelector("#config-list");

const CONFIG_VARIABLES = new Map();

function sendConfigVariableValue(variable, value) {
    ladybird.sendMessage("setConfigVariable", {
        name: variable.name,
        value,
    });
}

function createControl(variable) {
    const type = variable.type === "array" && variable.elementType === "string" ? "textarea" : "input";
    const input = document.createElement(type);
    input.id = variable.name;

    if (variable.type === "boolean") {
        input.type = "checkbox";
        input.switch = true;
        input.checked = !!variable.value;
        input.addEventListener("change", () => {
            sendConfigVariableValue(variable, input.checked);
        });

        return input;
    }

    if (type === "textarea") {
        input.rows = 3;
        input.value = Array.isArray(variable.value) ? variable.value.join("\n") : "";
        input.addEventListener("change", () => {
            const value = input.value
                .split(/\r?\n/)
                .map(entry => entry.trim())
                .filter(entry => entry.length !== 0);

            sendConfigVariableValue(variable, value);
        });

        return input;
    }

    input.value = variable.value ?? "";

    if (variable.type === "number") {
        input.type = "number";
        input.step = "any";
        input.addEventListener("change", () => {
            const value = Number.parseFloat(input.value);
            if (!Number.isNaN(value)) {
                sendConfigVariableValue(variable, value);
            }
        });
    } else {
        input.type = "text";
        input.addEventListener("change", () => {
            sendConfigVariableValue(variable, input.value);
        });
    }

    return input;
}

function createRow(variable) {
    const row = document.createElement("div");
    row.classList.add("card-group");
    row.classList.add("config-row");
    row.classList.add("inline-container");
    row.dataset.filterText = `${variable.title} ${variable.description}`.toLowerCase();

    const title = document.createElement("p");
    title.textContent = variable.title;

    const description = document.createElement("p");
    description.classList.add("description");
    description.textContent = variable.description;

    const label = document.createElement("label");
    label.htmlFor = variable.name;
    label.append(title, description);

    row.append(label, createControl(variable));
    return row;
}

function applyFilter() {
    const filter = configFilter.value.trim().toLowerCase();

    document.querySelectorAll(".config-row").forEach(row => {
        row.classList.toggle("hidden", filter.length !== 0 && !row.dataset.filterText.includes(filter));
    });
}

function loadConfigVariables(settings) {
    if (!Object.hasOwn(settings, "configVariableDefinitions")) {
        return;
    }

    const configVariables = settings.configVariableDefinitions || [];
    configVariables.forEach(variable => {
        const previous = CONFIG_VARIABLES.get(variable.name);
        if (previous && JSON.stringify(previous.value) === JSON.stringify(variable.value)) {
            return;
        }

        CONFIG_VARIABLES.set(variable.name, variable);

        const row = createRow(variable);
        const existing = document.getElementById(variable.name)?.closest(".config-row");
        if (existing) {
            existing.replaceWith(row);
        } else {
            configList.append(row);
        }
    });

    applyFilter();
}

configFilter.addEventListener("input", applyFilter);

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadConfigVariables(event.detail.data);
    }
});

const crashReportsStatus = document.querySelector("#crash-reports-status");
document.querySelector("#open-crash-reports").addEventListener("click", () => {
    crashReportsStatus.hidden = true;
    ladybird.sendMessage("showCrashReports");
});
document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "crashReportsStatus") {
        crashReportsStatus.textContent = event.detail.data;
        crashReportsStatus.hidden = !event.detail.data;
    }
});
