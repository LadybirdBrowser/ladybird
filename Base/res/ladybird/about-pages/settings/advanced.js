const configFilter = document.querySelector("#config-filter");
const configList = document.querySelector("#config-list");

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
    row.classList.add("card-separator");
    row.classList.add("config-row");
    row.classList.add("inline-container");
    row.dataset.filterText = `${variable.name} ${variable.title} ${variable.description}`.toLowerCase();

    const name = document.createElement("p");
    name.classList.add("config-name");
    name.textContent = variable.name;

    const title = document.createElement("p");
    title.classList.add("config-title");
    title.textContent = variable.title;

    const description = document.createElement("p");
    description.classList.add("description");
    description.textContent = variable.description;

    const label = document.createElement("label");
    label.htmlFor = variable.name;
    label.append(name, title, description);

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
    const configVariables = settings.configVariableDefinitions || [];
    configList.innerHTML = "";

    configVariables.forEach(variable => {
        configList.append(createRow(variable));
    });

    applyFilter();
}

configFilter.addEventListener("input", applyFilter);

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadSettings") {
        loadConfigVariables(event.detail.data);
    }
});
