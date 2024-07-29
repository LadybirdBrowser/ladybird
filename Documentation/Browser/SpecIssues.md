# Specification Issues tracked by Ladybird

## Introduction

This document serves to track the status of issues discovered by or impacting the Ladybird Browser project.

The issues are organized by the specification they are related to, and are tracked in the following format:

- **Issue Link**: Link to the issue in the relevant specification's bug tracker.
- **Issue Description**: A more detailed description of the issue, if applicable.
- **Algorithm**: The algorithm in the specification that is impacted by the issue, if applicable.
- **Issue Resolution**: The resolution of the issue, if applicable.
- **Relevant Files**: The files in the Ladybird Browser project that are impacted by the issue.


## WhatWG Specifications

### [HTML](https://html.spec.whatwg.org/)

#### Open Issues

-  https://github.com/whatwg/html/issues/4980

   Description: The HTML spec editors wish to remove the "implied document" concept
 
   Algorithm: [HostEnqueuePromiseJob](https://html.spec.whatwg.org/multipage/webappapis.html#hostenqueuepromisejob)
  
   Relevant Files: https://github.com/LadybirdBrowser/ladybird/blob/01ff3d428684c1f638cfd47fb451179e66a78dd5/Userland/Libraries/LibWeb/Bindings/MainThreadVM.cpp#L267-L270
   

- https://github.com/whatwg/html/issues/2291

   Description: User agent styling for `<br>` and `<wbr>` elements is not defined in a way that is implementable by CSS
   
   Relevant Files: https://github.com/LadybirdBrowser/ladybird/blob/01ff3d428684c1f638cfd47fb451179e66a78dd5/Userland/Libraries/LibWeb/CSS/Default.css#L366-L371
   https://github.com/LadybirdBrowser/ladybird/blob/01ff3d428684c1f638cfd47fb451179e66a78dd5/Userland/Libraries/LibWeb/CSS/StyleComputer.cpp#L2352-L2356


- https://github.com/whatwg/html/issues/9711

  Description: Aborting a document load has implementation defined effects on child navigables

  Algorithm: [abort a document and its descendants](https://html.spec.whatwg.org/multipage/document-lifecycle.html#abort-a-document-and-its-descendants) 

  Relevant Files: https://github.com/LadybirdBrowser/ladybird/blob/01ff3d428684c1f638cfd47fb451179e66a78dd5/Userland/Libraries/LibWeb/DOM/Document.cpp#L3323-L3328


#### Resolved Issues

- https://github.com/whatwg/html/pull/9907

  Description: Document destruction in navigables overhaul. Closes https://github.com/whatwg/html/pull/9148 and https://github.com/whatwg/html/pull/9904

  Algorithm: Destroy a document and its descendants, abort a document and its descendants, unload a document and its descendants.

  Resolution: Not Yet Fixed in Ladybird

  Relevant Files: https://github.com/LadybirdBrowser/ladybird/blob/01ff3d428684c1f638cfd47fb451179e66a78dd5/Userland/Libraries/LibWeb/DOM/Document.cpp#L3210-L3212


### [Console](https://console.spec.whatwg.org/)

#### Open Issues

- https://github.com/whatwg/console/issues/134

  Description: Timing and counter methods need to report when there are invalid items specified

  Algorithm: [timeLog](https://console.spec.whatwg.org/#timelog)

  Relevant Files: https://github.com/LadybirdBrowser/ladybird/blob/01ff3d428684c1f638cfd47fb451179e66a78dd5/Userland/Libraries/LibJS/Console.cpp#L394-L403