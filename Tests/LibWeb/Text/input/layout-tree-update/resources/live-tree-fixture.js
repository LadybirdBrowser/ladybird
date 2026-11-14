// A fixture with every kind of box a layout tree build makes: pseudo-elements, list markers, counters and quotes,
// ::first-letter, a top-layer element with its ::backdrop, SVG resources, tables with missing parts, display:
// contents, shadow trees with slots, anonymous wrappers and form controls.
const liveTreeFixtureStyle = `<style>
    body { counter-reset: section; quotes: "<" ">" "[" "]"; }
    section { counter-increment: section; }
    section::before { content: "S" counter(section) " "; }
    section::after { content: " end"; display: block; }
    .first-letter::first-letter { font-size: 2em; }
    .contents { display: contents; }
    .loose-cell { display: table-cell; }
    .loose-row { display: table-row; }
    dialog::backdrop { background: rgba(0, 0, 0, 0.1); }
    ol.reversed-list li::marker { content: counter(list-item) ") "; }
    q::before { content: open-quote; }
    q::after { content: close-quote; }
</style>`;

const liveTreeFixtureMarkup = `
<section><p class="first-letter">Lorem <b>ipsum</b> dolor</p><ol><li>one</li><li>two<ol><li>nested</li></ol></li></ol>
    <ol reversed class="reversed-list"><li>a</li><li>b</li></ol><p>quoted <q>outer <q>inner</q></q> text</p></section>
<section><div>inline start<div>block in the middle</div>inline end</div>
    <span>inline <div>block inside inline</div> after</span>
    <div class="contents"><p>in contents</p>text in contents</div></section>
<section><table><caption>caption</caption><tr><td>a</td><td rowspan="2">b</td></tr><tr><td>c</td></tr></table>
    <div><span class="loose-cell">loose cell</span><span class="loose-cell">another</span></div>
    <div class="loose-row"><div class="loose-cell">row cell</div></div></section>
<section><svg width="40" height="20"><defs><linearGradient id="gradient"><stop offset="0" stop-color="red"/>
    <stop offset="1" stop-color="blue"/></linearGradient><clipPath id="clip"><rect width="10" height="10"/></clipPath></defs>
    <rect width="20" height="20" fill="url(#gradient)" clip-path="url(#clip)"/></svg>
    <div id="host"><span slot="named">slotted named</span><span>slotted default</span></div>
    <button>button <b>label</b></button><fieldset><legend>legend</legend>fieldset content</fieldset></section>
<dialog id="modal">modal <b>dialog</b></dialog>`;

function setUpLiveTreeFixture(container) {
    container.innerHTML = liveTreeFixtureStyle + liveTreeFixtureMarkup;
    const shadow = container.querySelector("#host").attachShadow({ mode: "open" });
    shadow.innerHTML = `<style>p { margin: 0; }</style><p>shadow before</p><slot name="named"></slot><p>between</p><slot></slot>`;
    container.querySelector("#modal").showModal();
}
