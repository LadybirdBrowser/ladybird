"use strict";

// Shared helpers for the adoption-agency-reparenting tests.

// Compact serializer: element -> <name>[#id][children]; text -> "data".
// <script> contents are elided to keep expectations stable.
function serialize(n) {
  if (n.nodeType === Node.TEXT_NODE)
    return `"${n.data}"`;
  if (n.nodeType !== Node.ELEMENT_NODE)
    return "";
  const label = `<${n.localName}${n.id ? "#" + n.id : ""}>`;
  if (n.localName === "script")
    return label;
  const kids = tree(n);
  return kids ? `${label}[${kids}]` : label;
}

function tree(node) {
  return [...node.childNodes].map(serialize).join("");
}

// Parse srcdoc in an iframe (so inline scripts run during parsing) and resolve
// with the iframe once loaded.
function parseInIframe(t, srcdoc) {
  const iframe = document.createElement("iframe");
  iframe.srcdoc = srcdoc;
  return new Promise(resolve => {
    iframe.onload = t.step_func(() => resolve(iframe));
    document.body.appendChild(iframe);
  });
}
