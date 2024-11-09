# Frequently Asked Questions about Ladybird

## What does 'Independent' mean, if you're including third party dependencies?

Independent means:

- We implement the web platform standards ourselves: Ladybird is not a Blink/Chromium shell, not a WebKit port, not a Firefox fork.
- We don't take money from anyone with strings attached

## Windows support when?

There are very few Windows developers contributing to the project. As such, maintaining a native Windows port would be
a lot of effort that distracts from building out the web platform standards in a reasonable amount of time.

After we have a solid foundation, we may consider a Windows port, but it's not a priority. In the meantime, Windows
developers can use other tools such as WSL2 to work on Ladybird.

## Will Ladybird support `$THING`?

Eventually, probably, if there's a Web Spec for it!

## When will you implement `$THING`?

Maybe someday. Maybe never. If you want to see something happen, you can do it yourself!

## Is there a release roadmap?

- 2026: alpha release (daily driver for developers and early adopters) for Linux and macOS.
- 2027: beta release; downloadable app for Linux and macOS.
- 2028: stable release for general use
    ![image](https://github.com/user-attachments/assets/dec1ab54-5844-47dc-b365-03983bc00390)

## Well, how do I run this thing then?

Simple, my friend! Just refer to the [build instructions](BuildInstructionsLadybird.md).

## I did a `git pull` and now the build is broken! What do I do?

If it builds on CI, it should build for you too. You may need to rebuild the toolchain. If that doesn't help, try it with a clean repo.

If you can't figure out what to do, ask in the `#build-problems` channel on Discord.

## Where did Ladybird come from?

For full details, see the [Ladybird: A new cross-platform browser project](https://awesomekling.substack.com/p/ladybird-a-new-cross-platform-browser-project) announcement from 12 September 2022.

Here’s a short timeline:

- 2019 June: Work on what eventually became Ladybird started as _LibHTML_ — the beginnings of an HTML viewer for [SerenityOS](https://github.com/SerenityOS/serenity) — with a commit titled [“LibHTML: Start working on a simple HTML library”](https://github.com/SerenityOS/serenity/commit/a67e8238389), and with this commit description:

  > _I'd like to have rich text, and we might as well use HTML for that. :^)_

  LibHTML eventually became [LibWeb](https://github.com/LadybirdBrowser/ladybird/tree/master/Libraries/LibWeb) — which in turn eventually grew into being the core part of the browser engine and browser to which, on 4 July 2022, [the name _Ladybird_ was given](https://www.youtube.com/watch?v=X38MTKHt3_I&t=29s).

- 2022 July: Renamed _Ladybird_ by Andreas in [“Let's make a Linux GUI for the SerenityOS browser”](https://youtu.be/X38MTKHt3_I) live-coding video.
- 2022 Sept: Spun off from SerenityOS to separate project: [“A new cross-platform browser project”](https://awesomekling.substack.com/p/ladybird-a-new-cross-platform-browser-project) announcement.
- 2024 June: [“I'm forking Ladybird and stepping down as SerenityOS BDFL”](https://awesomekling.substack.com/p/forking-ladybird-and-stepping-down-serenityos) announcement from Andreas.
- 2024 July: [Ladybird Browser Initiative](https://ladybird.org/posts/announcement/) launched by Andreas and GitHub co-founder [defunkt](https://twitter.com/defunkt) (Chris Wanstrath).

## What makes Ladybird/[Ladybird Browser Initiative](https://ladybird.org/) different?

- Fully independent: Written from scratch, using no code from any other browser engine.
- Singular focus: Doing only one single thing: building a new browser engine and browser.
- No monetization: Will never take funding from default search deals or any other forms of user monetization, ever.

## Are there video/audio announcements and interviews about the start of the Ladybird Browser Initiative?

- Ladybird Browser Initiative [announcement video](https://www.youtube.com/watch?v=k9edTqPMX_k) from defunkt explaining the project _raison dʼêtre_ + goals (July 2024).
- [Why we need Ladybird](https://changelog.com/podcast/604#t=5:08): _Changelog_ podcast interview with Andreas and defunkt (August 2024); [transcript](https://changelog.com/podcast/604#transcript); [chapters](https://changelog.com/podcast/604#chapters).
- [Eron Wolf announcement grant of $200K](https://youtu.be/p6k9qcRpW_k) from [FUTO](https://www.futo.org/about/what-is-futo/) to the project (August 2024).
- [Eron Wolf interview with Andreas](https://youtu.be/4xhaAAcKLtI) (August 2024).

## Can you describe some of the project goals and its culture?

- Eventually give everybody the choice of a whole new browser they can use for their daily browsing.
- Prove it is in fact possible to build a completely new browser, by implementing from the WHATWG/W3C/etc. specs.
- Have a lot of real fun together actually doing it.
- Prove that developing an engine doesn’t take hundreds of engineers — and not anything close to even just a hundred.
- Browser engineering: Further help de-mystify it and make it a standard thing to learn (hat tip: https://browser.engineering/).
- Using project Discord server for communication [discord.gg/nvfjVJ4Svh](https://discord.gg/nvfjVJ4Svh).
- Using [one GithHub repo](https://github.com/LadybirdBrowser/ladybird) for everything: issues (no bugzilla or other), patch/PR submission/review, CI/test automation.

## What are some of the project coding conventions? And do you have any activity metrics?

- Implement web-platform features exactly according to the actual steps in spec algorithms.
- Abundant code comments with verbatim spec text copy/pasted in — showing exactly what’s being implemented.
- Additional _`“AD-HOC:”`_ comment convention to mark code that doesn’t map to any spec requirements.
- Class/file names tend to closely match actual current spec terms; e.g., `Navigable.h`, `Transferable.h`.
- [“critically reading standards and reporting what is wrong”](https://matrixlogs.bakkot.com/WHATWG/2024-08-23#L10)
- Project activity relative rankings: https://git-pulse.github.io/snapshots/?project=LadybirdBrowser_ladybird

## Do you have some general details about the code and basic architecture?

- C++ while selectively migrating parts to Swift and while keeping an eye on things like Sean Baxter’s [Circle](https://github.com/seanbaxter/circle) & [Safe C++](https://safecpp.org/draft.html).
- Some use of third-party libraries (e.g., Harfbuzz, Skia, [simdutf](https://github.com/simdutf/simdutf), libcurl).
- Performance optimizing is not yet a super-high priority (but performance-boosting changes are regularly getting made).
- Code size:
  - Roughly same size (number of lines of code) as Servo.
  - About 1/15th as many lines of C++ code as WebKit.
  - About 1/20th as many lines as C++ code Gecko.
  - About 1/50th as many lines as C++ code Chromium.
- Level of standards support: [wpt.fyi/results?product=ladybird](https://wpt.fyi/results/?product=ladybird) has current test results for all WPT tests.
- [LadybirdBrowser/ladybird#features](https://github.com/LadybirdBrowser/ladybird/#features):
  - UI process, ImageDecoder process, RequestServer process, WebContent processes.
  - LibWeb: core web-rendering engine (HTML, CSS, Events, DOM, APIs).
  - LibJS: JavaScript engine written from scratch (currently JIT-less).
  - LibWasm: WebAssembly implementation written from scratch.
  - [AK](https://github.com/LadybirdBrowser/ladybird/tree/master/AK): Ladybird standard library/abstractions: asserts, smart pointers, strings, numbers (e.g., [fast_float](https://github.com/fastfloat/fast_float) impl.), more…

## What about funding?

- Funded _entirely_ through donations and sponsorships.
- https://donorbox.org/ladybird – donations of any amount: $10, $50, $100, etc.
- https://polar.sh/LadybirdBrowser – set bounties to directly fund specific features/tasks; e.g., [$300 legacy-encoders bounty](https://github.com/LadybirdBrowser/ladybird/issues/824).
- [Sponsorship opportunities](https://ladybird.org/#sponsors): Platinum $100,000 • Gold $50,000 • Silver $10,000 • Bronze $5,000 • Copper $1,000.
- Ladybird Browser Initiative announced/seeded with [1 million dollar donation](https://twitter.com/defunkt/status/1807779408092234134) from defunkt and his family.

## Do you have some places where I can watch videos related to the project?

- Ladybird [YouTube channel](https://www.youtube.com/@LadybirdBrowser): monthly Ladybird project updates from Andreas.
- Andreas’ [YouTube channel](https://www.youtube.com/@awesomekling): 1000+ videos from 6+ years; incl. “car talk” + OS/browser “hacking” (live-coding) videos.

## Is there some related background to help me understand what a browser _engine_ is and why it’s important?

- [Understanding the role of browser engines](https://assets.publishing.service.gov.uk/media/61b86737e90e07043c35f5be/Appendix_F_-_Understanding_the_role_of_browser_engines.pdf) (UK Competition and Markets Authority [Mobile ecosystems market study](https://www.gov.uk/cma-cases/mobile-ecosystems-market-study#interim-report)).
