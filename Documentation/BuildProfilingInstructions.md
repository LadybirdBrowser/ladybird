# Build profiling instructions

There are three ways you can get information about compile times:

1. Just using `time ninja install` instead of `ninja install` to time it overall.
2. Reading ninja's log files to get a per-file compile time.
3. Enabling GCC or Clang flags to get a more detailed per-file breakdown of times for compiler passes within a file.

## Ninja log files

Ninja produces a handy log file that includes a per-cpp-file compilation time. This is useful for discovering which files take the most time, and so are good targets for speeding up the overall build time. A python3 script named `ninjatracing` converts the ninja log file into a JSON format that's readable by several profile viewers.

### Prerequisites

`ninjatracing` is available on [GitHub](https://github.com/nico/ninjatracing/blob/master/ninjatracing)

It requires `python3`, available at `python`. You can either create the symlink yourself, or just modify the ninjatracing file to say `python3` instead of `python`.

You also need to make sure `ninjatracing` is marked as executable, and available from your PATH (or somewhere convenient where you can manually call it from).

### Usage

First, we need to clean the build (and `ccache` if present) to make sure every file gets compiled and gives a meaningful time reading.
```console
ninja clean

ccache --clear
```

Then, execute ninja as normal:
```console
ninja
```

The log will be written to `.ninja_log` in the current directory by default. To convert it to the more useful JSON file, call `ninjatracing` from the directory you called ninja from:

```console
ninjatracing .ninja_log > trace.json

```

You can then drag-and-drop the file onto [Speedscope](https://www.speedscope.app/) or any other compatible flamegraph visualizer.

## GCC/Clang

Adding the `-ftime-report` flag to GCC will cause it to output a breakdown for each file it compiles, which looks like this:

<details>
	<summary>GCC -ftime-report output</summary>

```console
Time variable                                   usr           sys          wall           GGC
 phase setup                        :   0.00 (  0%)   0.00 (  0%)   0.01 (  0%)  1326k (  2%)
 phase parsing                      :   0.57 ( 61%)   0.19 ( 83%)   1.63 ( 65%)    59M ( 74%)
 phase lang. deferred               :   0.10 ( 11%)   0.03 ( 13%)   0.30 ( 12%)  8761k ( 11%)
 phase opt and generate             :   0.23 ( 25%)   0.01 (  4%)   0.48 ( 19%)    10M ( 13%)
 phase last asm                     :   0.03 (  3%)   0.00 (  0%)   0.08 (  3%)   539k (  1%)
 |name lookup                       :   0.11 ( 12%)   0.01 (  4%)   0.25 ( 10%)  2004k (  2%)
 |overload resolution               :   0.08 (  9%)   0.00 (  0%)   0.26 ( 10%)  7900k ( 10%)
 dump files                         :   0.02 (  2%)   0.00 (  0%)   0.00 (  0%)     0  (  0%)
 callgraph construction             :   0.04 (  4%)   0.00 (  0%)   0.06 (  2%)  2677k (  3%)
 callgraph optimization             :   0.00 (  0%)   0.00 (  0%)   0.01 (  0%)  4752  (  0%)
 callgraph functions expansion      :   0.15 ( 16%)   0.00 (  0%)   0.32 ( 13%)  6267k (  8%)
 callgraph ipa passes               :   0.03 (  3%)   0.01 (  4%)   0.08 (  3%)  1413k (  2%)
 ipa inheritance graph              :   0.00 (  0%)   0.00 (  0%)   0.02 (  1%)  3760  (  0%)
 ipa profile                        :   0.00 (  0%)   0.00 (  0%)   0.01 (  0%)     0  (  0%)
 trivially dead code                :   0.01 (  1%)   0.00 (  0%)   0.00 (  0%)   224  (  0%)
 df reg dead/unused notes           :   0.01 (  1%)   0.00 (  0%)   0.00 (  0%)   104k (  0%)
 alias analysis                     :   0.01 (  1%)   0.00 (  0%)   0.00 (  0%)    70k (  0%)
 preprocessing                      :   0.06 (  6%)   0.03 ( 13%)   0.16 (  6%)  1365k (  2%)
 parser (global)                    :   0.04 (  4%)   0.04 ( 17%)   0.13 (  5%)  6894k (  8%)
 parser struct body                 :   0.09 ( 10%)   0.03 ( 13%)   0.20 (  8%)  9020k ( 11%)
 parser function body               :   0.00 (  0%)   0.00 (  0%)   0.01 (  0%)    83k (  0%)
 parser inl. func. body             :   0.03 (  3%)   0.01 (  4%)   0.05 (  2%)  1567k (  2%)
 parser inl. meth. body             :   0.11 ( 12%)   0.03 ( 13%)   0.30 ( 12%)    10M ( 13%)
 template instantiation             :   0.27 ( 29%)   0.08 ( 35%)   0.89 ( 36%)    25M ( 32%)
 constant expression evaluation     :   0.01 (  1%)   0.00 (  0%)   0.01 (  0%)   356k (  0%)
 constraint satisfaction            :   0.01 (  1%)   0.00 (  0%)   0.02 (  1%)   130k (  0%)
 early inlining heuristics          :   0.00 (  0%)   0.00 (  0%)   0.02 (  1%)    21k (  0%)
 inline parameters                  :   0.01 (  1%)   0.00 (  0%)   0.03 (  1%)   146k (  0%)
 integration                        :   0.00 (  0%)   0.00 (  0%)   0.01 (  0%)   564k (  1%)
 tree CFG cleanup                   :   0.00 (  0%)   0.00 (  0%)   0.01 (  0%)  5768  (  0%)
 tree SSA other                     :   0.00 (  0%)   0.00 (  0%)   0.01 (  0%)    13k (  0%)
 tree operand scan                  :   0.01 (  1%)   0.00 (  0%)   0.00 (  0%)   429k (  1%)
 tree CCP                           :   0.01 (  1%)   0.00 (  0%)   0.00 (  0%)    18k (  0%)
 expand                             :   0.01 (  1%)   0.00 (  0%)   0.03 (  1%)  1303k (  2%)
 varconst                           :   0.00 (  0%)   0.00 (  0%)   0.04 (  2%)  6744  (  0%)
 forward prop                       :   0.01 (  1%)   0.00 (  0%)   0.02 (  1%)  3520  (  0%)
 CSE                                :   0.01 (  1%)   0.00 (  0%)   0.00 (  0%)  1144  (  0%)
 loop fini                          :   0.00 (  0%)   0.00 (  0%)   0.01 (  0%)     0  (  0%)
 branch prediction                  :   0.00 (  0%)   0.01 (  4%)   0.00 (  0%)    20k (  0%)
 combiner                           :   0.01 (  1%)   0.00 (  0%)   0.02 (  1%)   138k (  0%)
 integrated RA                      :   0.01 (  1%)   0.00 (  0%)   0.01 (  0%)  1112k (  1%)
 LRA reload inheritance             :   0.00 (  0%)   0.00 (  0%)   0.02 (  1%)    13k (  0%)
 LRA create live ranges             :   0.00 (  0%)   0.00 (  0%)   0.01 (  0%)  8568  (  0%)
 reload CSE regs                    :   0.01 (  1%)   0.00 (  0%)   0.02 (  1%)   100k (  0%)
 final                              :   0.00 (  0%)   0.00 (  0%)   0.02 (  1%)   323k (  0%)
 variable output                    :   0.01 (  1%)   0.00 (  0%)   0.00 (  0%)    15k (  0%)
 symout                             :   0.09 ( 10%)   0.00 (  0%)   0.20 (  8%)    13M ( 17%)
 variable tracking                  :   0.00 (  0%)   0.00 (  0%)   0.04 (  2%)   471k (  1%)
 var-tracking dataflow              :   0.02 (  2%)   0.00 (  0%)   0.03 (  1%)    18k (  0%)
 var-tracking emit                  :   0.00 (  0%)   0.00 (  0%)   0.04 (  2%)   117k (  0%)
 rest of compilation                :   0.01 (  1%)   0.00 (  0%)   0.03 (  1%)  1258k (  2%)
 TOTAL                              :   0.93          0.23          2.50           79M
[24/3139] Building CXX object Kernel/CMakeFiles/Kernel.dir/CommandLine.cpp.o
```

</details>

Depending on whether you understand the internals of the compiler, this may or may not be helpful to you! Generally, this is not recommended.

Clang also supports `-ftime-report`, but I have not tested the output for it.

To add the flag, edit the `CMakeLists.txt` in the ladybird root directory, and add `add_compile_options(-ftime-report)` to the list of compile options that start around line 220.

Additionally, you can add `-ftime-report-details` too, which again I have not tested.
