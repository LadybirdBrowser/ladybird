"use strict";

test(() => {
  const doc = document.cloneNode(document);
  doc.write('<div><template shadowrootmode=open>test</template></div>');
  assert_true(!!doc.body.firstChild.shadowRoot);
  assert_equals(doc.body.firstChild.shadowRoot.textContent, "test");
}, "cloneNode() and document's allow declarative shadow roots");
