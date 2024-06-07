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
