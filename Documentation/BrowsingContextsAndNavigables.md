# LibWeb: Browsing Contexts and Navigables

**NOTE: This document is a work in progress!**

## Introduction: How does code execute, really?

Before we can dive into how LibWeb and Ladybird implement the HTML web page navigation operations,
we need to dive into some fundamental specification concepts. Starting with, how does code actually
execute in a  (possibly virtual) machine? Next we'll look at what that means for the ECMAScript
Specification (JavaScript), and finally how the ECMAScript code execution model ties into the
HTML specification to model how to display web content into a browser tab. 

### Native Code Execution: A Primer

When modeling the execution of a native program written in a popular systems language like
C, C++, or Rust, most systems programmers should be familiar with the concepts of *threads*
and *processes*. In a "hosted" environment, the execution of one's userspace program generally
starts with an underlying operating system creating a process for the application to run in.
This process will contain a memory space for program data and code to live in, and an initial,
or main thread to start execution on. In order for the operating system to change which
thread is executing on a particular CPU core, it needs to save and restore the *Execution Context*
for that thread. The Execution Context for a native thread generally consists of a set of
CPU registers, any floating point state, a program counter that tracks which instruction should
be loaded next, and a stack pointer that points to the local data the thread was using to track
its function call stack and local variables. The programmer can also request additional threads
through a system call, providing a set of thread attributes and a function to call as the entry
point.

For traditional compiled programs, the concept of accessing variables and functions is split into
two phases. At compile time, local variables and arguments are folded into stack slots and
allocated into registers. Exported variables and functions are written into the executable object
file (ELF, Mach-O, PE, etc.) and are visible to external tools as symbols, as referenced by a
symbol table contained within the object file format. Normally local variable and argument
names and locations are lost in the compile+link steps, but the compiler can be configured to
emit extra debug information to allow debuggers to access and modify them at runtime. In order
to support something like the dynamic imports of interpreted languages, the programmer has to
call a platform-specific function to load the new module (e.g. ``dlopen`` or ``LoadLibrary``).
But after the module is opened, in order to actually refer to any exported symbols from that module the
programmer has to retrieve the address of each symbol through another platform specific function
(e.g. ``dlsym`` or ``GetProcAddress``), once per symbol.

### ECMAScript Execution Model: Realms and Agents

The ECMAScript specification has analogs for almost all of these concepts in the section on
[Executable Code and Execution Contexts](https://tc39.es/ecma262/#sec-executable-code-and-execution-contexts).

Working in the other direction from the native code explanation, ECMAScript describes the accessibility
and scopes of functions, variables, and arguments in terms of [*Environment Records*](https://tc39.es/ecma262/#sec-environment-records).
Note that these Environment Records are not actually visible to executing code, and are simply a mechanism
used by the specification authors to model the language.  Every function and module has a type
of Environment Record that contains the variables, functions, catch clause bindings, and other
language constructs that affect which names are visible at any location in the code. These Environment Records
are nested, in a tree-like structure that somewhat matches the Abstract Syntax Tree (AST).

The root of the tree of Environment Records is the Global Environment Record, which corresponds to the
Global Object and its properties. In JavaScript, there is always a ``this`` value representing the current
object context. At global scope, the Global Object normally takes that responsibility. In a REPL, that might
be some REPL specific global object that has global functions to call for doing things like loading
from the filesystem, or even be as complex as Node or Bun. In a Browser context, the Global object is
normally the Window, unless there's a Worker involved. For historical reasons the global ``this`` binding for
Window contexts is actually a WindowProxy that wraps the Window. This concept is quite different from a native
executable, where there's no actual object representing the global scope, simply symbols that the
linker and loader make available to each module.

While the Global Object and its Global Environment represent the root of the tree of identifiers visible
to the executing JavaScript code, the Global Object isn't sufficient to model all the state around
a conceptual thread of execution in ECMAScript. This is where the two concepts of [*Realms*](https://tc39.es/ecma262/#sec-code-realms)
and [*Execution Contexts*](https://tc39.es/ecma262/#sec-execution-contexts) come into play.
A [*Realm Record*](https://tc39.es/ecma262/#realm-record) is a container that holds a global object,
its associated Global Environment, a set of intrinsic objects, and any *host* (also called an *embedder*
in some specification documents) defined extra state that needs to be associated with the realm.
In LibWeb, the Host Defined slot holds an object that has the HTML Environment Settings Object for each realm,
as well as all the prototypes, constructors, and namespaces that need to be exposed on the Global Object
for Web APIs. On top of the Realm abstraction, ECMAScript uses the Execution Context to model the state
of execution of one particular script or module. Each Execution Context belongs to an [*execution context stack*](https://tc39.es/ecma262/#execution-context-stack)
with the topmost context named the [*running execution context*](https://tc39.es/ecma262/#running-execution-context).
An Execution Context has information about the current function, the script or module that the current code block belongs to,
additional Environment Records required to access names in the current scope, any running generator state,
and most importantly to the thread analogy, the state needed to suspend and resume execution of that script.
As with Environment Records, Realms and Execution contexts are not directly accessible to running JavaScript code.

The final missing piece for the JavaScript execution model is how these stacks of Execution Contexts
are actually scheduled to run by the ECMAScript implementation. In the most common case, this means directly
mapping the ECMAScript model to the earlier native concepts of threads and processes in a way that
allows for flexibility in the implementation strategies. The last thing that the specification authors want
to do is constrain implementations so much that innovation and experimentation becomes impossible.
The method for this mapping is the two related specification mechanisms [*Agents*](https://tc39.es/ecma262/#sec-agents)
and [*Agent Clusters*](https://tc39.es/ecma262/#sec-agent-clusters). The Execution Context stack mentioned
above actually belongs to an Agent, which holds said stack, a set of metadata about the memory model,
and a shared reference to an [*executing thread*](https://tc39.es/ecma262/#executing-thread).
According to ECMAScript, there should always be at least one Execution Context on the stack, to allow concepts
such as the running execution context to always refer to the topmost Execution Context of the [*surrounding agent*](https://tc39.es/ecma262/#surrounding-agent).
However, the HTML specification opts to remove the default execution context from the execution context stack
at creation, and instead manually pushes and pops execution contexts for script, module, and callback execution.
The relationship between Realms and Agents is not 1-1, but N-1. In the ECMAScript specification, this manifests
as a part of the [*Shadow Realm proposal*](https://tc39.es/proposal-shadowrealm/), while the Web platform
requires multiple Realms per Agent to specify the historical behavior of ``<iframe>`` and related elements.

An Agent holds a stack of Execution Contexts, with the topmost entry being the running execution context.
Each Execution Context holds a Realm and a specific script's context, including the current function and
any state required to pause and resume the execution for that context. The Realm holds the Global
Object for the Execution Context, and any ECMAScript or host-specific intrinsics required to create the
desired environment for code to run in. More loosely, an Agent is a specification artifact that somewhat
maps the execution of a JavaScript script or module to a native thread of execution. But the specification
does so in a way that allows a host/embedder to choose to switch out which Agent is currently executing
its running execution context on that native thread, and which Realm within that Agent owns the running execution
context.

SharedArrayBuffers and Atomics add a special kind of wrinkle to the ECMAScript specification. Defining
how that work required the formalization of a memory model, similar to what C++11 and C11 and Java 5 had
to do before them. The Agent Cluster is the formalism that ties the memory model back to the execution
model. As described in the specification, an Agent Cluster is a set of Agents that can communicate
via shared memory. The exact mechanism is unspecified, but the hard rule is that all Agents within
a particular Agent Cluster must observe the same order of reads and writes to SharedArrayBuffers and
as a result of ECMAScript Atomic objects.

The net result of all this memory model and atomic specification language is that loosely, an Agent models
a "candidate execution" of some code module that can execute on a thread, and any suspended execution
contexts from things like generators or async that are part of that module and its dynamic imports.
An Agent Cluster models the interaction of agents that share the ability to communicate via shared memory.
The simplest reading of this is that the specification authors had in mind the type of memory sharing
that threads within the same process have in native code execution. So an Agent Cluster loosely models
a collection of Agents (read: threads) that execute independently of each other within the same implementation
defined manner for sharing memory between different threads (read: process).

### HTML Execution Model: Global Scopes

The Document Object Model (DOM) specifications are written in such a way that implementers can
create language bindings for any language to access the page. However, experience has shown that the
most popular way to script web content in modern web browsers is through JavaScript bindings. As such,
the HTML specification is specifically tailored to meet the constraints of JavaScript execution in its
scripting APIs and related concepts. Great care is taken to ensure that JavaScript written by different
authors cannot interfere with each other, and that arbitrary scripts cannot exfiltrate information about
the page content to third-party destinations.

The HTML specification therefore has a section on [Agents and Agent Clusters](https://html.spec.whatwg.org/multipage/webappapis.html#agents-and-agent-clusters)
at the start of the section on how scripting behaves on the Web platform.

TODO: Finish this section

## HTML Navigation: Juggling Origins



### Global Scopes, Browsing Contexts, Browsing Context Groups, Navigables, and Traversable Navigables

TODO:

- Agents defined by the HTML Spec
- Global Objects (Global Scopes) defined by the HTML Spec
- Agents and Browsing Context Groups
- Navigables and their relationship to Browsing Contexts
- Walk through construction of a browser tab, its traversable navigable, and its navigation both same and
  cross-origin
- Walk through construction of a browser tab with a nested browsing context and what happens when the
  nested context within its navigable container navigates on its own
