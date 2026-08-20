# LampVM Full Application

This directory is intentionally isolated from the VM CLI and can be extracted
into its own repository or mounted as a git submodule later.

The call path is:

```text
Compose Desktop -> JNI glue -> Kotlin/Native bridge -> lampvm debug C ABI -> VM
```

The JNI source only converts JVM strings and arrays. Kotlin/Native owns the
native handle mapping and the versioned C interop boundary.

## Two launchables

- `lampvm`: the native VM CLI produced by the repository's CMake build.
- `lampvm-debugger`: the full Compose application with VM lifecycle controls,
  JediTerm serial console, VGA display, and debugger panels.

The Compose application is the default graphical LampVM experience. The native
CLI remains available for headless, selftest, benchmark, and automation use.

## Run on Apple Silicon

From this directory:

```bash
gradle -g .gradle-user :app:run
```

Create a self-contained macOS application:

```bash
gradle -g .gradle-user :app:createDistributable
```

The executable is under
`app/build/compose/binaries/main/app/lampvm-debugger.app/Contents/MacOS/`.
The native core and Kotlin/Native bridge are bundled into the application and
loaded automatically; development-only JVM system properties are not required.

The build creates `liblampvm_core.dylib`, compiles the small JNI object, links
it into the Kotlin/Native shared library, and launches Compose Desktop with the
two native-library paths configured.

The first milestone supports:

- VM attach/create, run, pause, resume, and stop
- exact single-instruction stepping while all other vCPUs remain paused
- selectable vCPU register, disassembly, memory-follow, and step target
- configurable guest RAM, vCPU count, CPU frequency, and execution engine
- coherent CPU register snapshots while paused
- paused memory inspection following the current instruction pointer
- Lamp/Polaris ISA disassembly around IP with current-instruction highlighting
- runtime statistics
- embedded JediTerm 3.74 xterm/VT100 serial terminal with editing, ANSI colors,
  scrollback, selection, and clipboard support; input typed while paused is
  queued and delivered immediately before the next single-step request
- an independent live Compose VGA window backed by coherent framebuffer snapshots
- VGA keyboard and relative-pointer capture through explicit PS/2 input ABI calls

Breakpoints and event batching can be added on top of the same ABI.
