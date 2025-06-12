function runUAStyleTests(props) {
  const refs = document.getElementById('refs');
  for (const el of document.querySelectorAll('#tests > *')) {
   const clone = fakeClone(el);
   refs.append(clone);
  }
  const testsContainer = document.getElementById('tests');
  const testEls = document.querySelectorAll('#tests *');
  const refEls = document.querySelectorAll('#refs *');
  for (let i = 0; i < testEls.length; ++i) {
   const testEl = testEls[i];
   if (testEl.hasAttribute('data-skip')) {
     continue;
   }
   const refEl = refEls[i];
   const testStyle = getComputedStyle(testEl);
   const refStyle = getComputedStyle(refEl);
   for (const prop of props) {
     // Don't test display for some elements.
     // TODO(zcorpan): https://github.com/whatwg/html/issues/4093
     // TODO(zcorpan): https://github.com/whatwg/html/issues/5063
     if (prop === 'display' &&
         (testEl.localName === 'optgroup' ||
          testEl.localName === 'option' ||
          testEl.localName === 'marquee')
      ) {
      continue;
     }
     if (prop === 'overflow' && testEl.localName === 'select') {
      // TODO: https://github.com/whatwg/html/issues/10031
      continue;
     }
     test(() => {
       assert_equals(testStyle.getPropertyValue(prop), refStyle.getPropertyValue(prop));
     }, `${testNameContext(testEl)} - ${prop}`);
   }
  }

  function fakeClone(el) {
   const clone = document.createElementNS('urn:not-html', el.localName);
   for (const att of el.attributes) {
     clone.setAttributeNS(att.namespaceURI, att.name, att.value);
   }
   // deep clone
   for (const child of el.children) {
     clone.append(fakeClone(child));
   }
   return clone;
  }

  function testNameContext(el) {
   const outerHTML = el.outerHTML;
   const startTags = outerHTML.substring(0, outerHTML.indexOf('</')) || outerHTML;

   let ancestors = [];
   let current = el.parentNode;
   while (current != testsContainer) {
     ancestors.unshift(`<${current.localName}${contextAttrs(current.attributes)}>`);
     current = current.parentNode;
   }
   return startTags + (ancestors.length ? ` (in ${ancestors.join('')})` : '');
  }

  function contextAttrs(attributes) {
    let rv = "";
    for (let i = 0; i < attributes.length; ++i) {
      if (attributes[i].name === 'data-skip') {
        continue;
      }
      rv += ` ${attributes[i].name}="${attributes[i].value}"`;
    }
    return rv;
  }
}
