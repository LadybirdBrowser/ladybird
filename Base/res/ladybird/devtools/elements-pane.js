export class ElementsPanel {
    constructor(client) {
        this.client = client;
        this.walkerActor = null;
        this.treeEl = document.getElementById("dom-tree");
    }

    async initialize() {
        this.treeEl.textContent = "";

        if (!this.client.inspectorActor) {
            this.treeEl.textContent = "No inspector actor available";
            return;
        }

        this.treeEl.textContent = "Loading DOM tree\u2026";

        const resp = await this.client.request(this.client.inspectorActor, "getWalker");
        if (!resp || !resp.walker) {
            this.treeEl.textContent = "Failed to get walker";
            return;
        }

        this.walkerActor = resp.walker.actor;
        this.treeEl.textContent = "";

        const root = resp.walker.root;
        if (root) {
            const children = await this._getChildren(root.actor);
            if (children) {
                for (const child of children) {
                    await this._appendNode(this.treeEl, child, 0);
                }
            }
        }
    }

    async reload() {
        this.walkerActor = null;
        await this.initialize();
    }

    async _appendNode(parentEl, node, depth) {
        const hasChildren = node.numChildren > 0;
        const wrapper = document.createElement("div");

        const line = document.createElement("div");
        line.className = "dom-line";
        line.style.paddingLeft = `${depth * 16 + 8}px`;

        const toggle = document.createElement("span");
        toggle.className = "dom-toggle";
        if (hasChildren) {
            toggle.textContent = "\u25B6";
        }
        line.appendChild(toggle);

        const content = document.createElement("span");
        content.innerHTML = this._renderNode(node);
        line.appendChild(content);

        wrapper.appendChild(line);

        let childrenEl = null;
        let closingEl = null;

        if (hasChildren) {
            childrenEl = document.createElement("div");
            childrenEl.className = "dom-children collapsed";
            wrapper.appendChild(childrenEl);

            if (node.nodeType === 1) {
                closingEl = this._makeClosingTag(node, depth);
                closingEl.hidden = true;
                wrapper.appendChild(closingEl);
            }

            let loaded = false;
            const doToggle = async () => {
                if (childrenEl.classList.contains("collapsed")) {
                    if (!loaded) {
                        loaded = true;
                        const children = await this._getChildren(node.actor);
                        if (children) {
                            for (const child of children) {
                                await this._appendNode(childrenEl, child, depth + 1);
                            }
                        }
                    }
                    setCollapsed(false);
                    return;
                }
                setCollapsed(true);
            };

            toggle.addEventListener("click", e => {
                e.stopPropagation();
                doToggle();
            });

            const tag = (node.displayName || node.nodeName || "").toLowerCase();
            if (tag === "html" || tag === "body") {
                await doToggle();
            }

            function setCollapsed(collapsed) {
                childrenEl.classList.toggle("collapsed", collapsed);
                toggle.textContent = collapsed ? "\u25B6" : "\u25BC";
                if (closingEl) closingEl.hidden = collapsed;
            }
        }

        parentEl.appendChild(wrapper);
    }

    _renderNode(node) {
        switch (node.nodeType) {
            case 1:
                return this._renderElement(node);
            case 3:
                return `<span class="dom-text">${escapeHtml(truncate(node.nodeValue || "", 80))}</span>`;
            case 8:
                return `<span class="dom-comment">&lt;!-- ${escapeHtml(truncate(node.nodeValue || "", 60))} --&gt;</span>`;
            case 10:
                return `<span class="dom-doctype">&lt;!DOCTYPE ${escapeHtml((node.nodeName || "html").toLowerCase())}&gt;</span>`;
            default:
                return escapeHtml(node.nodeName || `[${node.nodeType}]`);
        }
    }

    _renderElement(node) {
        const tag = node.displayName || node.nodeName.toLowerCase();
        let html = `<span class="dom-bracket">&lt;</span><span class="dom-tag">${escapeHtml(tag)}</span>`;

        if (node.attrs) {
            for (const attr of node.attrs) {
                html += ` <span class="dom-attr-name">${escapeHtml(attr.name)}</span>`;
                if (attr.value !== "") html += `=<span class="dom-attr-value">"${escapeHtml(attr.value)}"</span>`;
            }
        }

        html += `<span class="dom-bracket">&gt;</span>`;

        if (node.numChildren === 0 && !VOID_ELEMENTS.has(tag))
            html += `<span class="dom-bracket">&lt;/</span><span class="dom-tag">${escapeHtml(tag)}</span><span class="dom-bracket">&gt;</span>`;

        return html;
    }

    _makeClosingTag(node, depth) {
        const tag = node.displayName || node.nodeName.toLowerCase();
        const line = document.createElement("div");
        line.className = "dom-line";
        line.style.paddingLeft = `${depth * 16 + 8}px`;
        const spacer = document.createElement("span");
        spacer.className = "dom-toggle";
        line.appendChild(spacer);
        const span = document.createElement("span");
        span.innerHTML = `<span class="dom-bracket">&lt;/</span><span class="dom-tag">${escapeHtml(tag)}</span><span class="dom-bracket">&gt;</span>`;
        line.appendChild(span);
        return line;
    }

    async _getChildren(nodeActor) {
        const resp = await this.client.request(this.walkerActor, "children", { node: nodeActor });
        return resp ? resp.nodes : null;
    }
}

const VOID_ELEMENTS = new Set([
    "area", "base", "br", "col", "embed", "hr", "img", "input",
    "link", "meta", "param", "source", "track", "wbr",
]);

function escapeHtml(str) {
    return String(str).replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;");
}

function truncate(str, max) {
    str = str.trim();
    return str.length > max ? str.substring(0, max) + "\u2026" : str;
}
