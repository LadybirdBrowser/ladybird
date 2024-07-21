# Frequently Asked Questions about Ladybird

## What does 'Independent' mean, if you're including third party dependencies?

Independent means:

- We implement the web platform standards ourselves
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

## Well, how do I run this thing then?

Simple, my friend! Just refer to the [build instructions](BuildInstructionsLadybird.md).

## I did a `git pull` and now the build is broken! What do I do?

If it builds on CI, it should build for you too. You may need to rebuild the toolchain. If that doesn't help, try it with a clean repo.

If you can't figure out what to do, ask in the `#build-problems` channel on Discord.

## Where did Ladybird come from?

For full details, see the [Ladybird: A new cross-platform browser project](https://awesomekling.substack.com/p/ladybird-a-new-cross-platform-browser-project) announcement from 12 September 2022.

Here’s a very short summary: Work on what eventually became Ladybird started on 15 June 2019, as _LibHTML_ — the beginnings of an HTML viewer for [SerenityOS](https://github.com/SerenityOS/serenity) — with a commit titled [“LibHTML: Start working on a simple HTML library”](https://github.com/SerenityOS/serenity/commit/a67e8238389), and with this commit description:

> _I'd like to have rich text, and we might as well use HTML for that. :^)_

LibHTML eventually became [LibWeb](https://github.com/LadybirdBrowser/ladybird/tree/master/Userland/Libraries/LibWeb) — which in turn eventually grew into being the core part of the browser engine and browser to which, on 4 July 2022, [the name _Ladybird_ was given](https://www.youtube.com/watch?v=X38MTKHt3_I&t=29s).
