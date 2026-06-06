Missing for "cargo build" UX:
 - Convention over configuration: Enforce a rigid project layout (src/, tests/, build.cpp for custom steps)
 - Convent Automatic toolchain management: Download and cache exact compiler versions (think rustup for C++)
 - Convent Header dependency extraction: Parse preprocessor directives without running the compiler (fast!)
 - Convent Build cache with strong hashing: Cache object files based on compiler flags, toolchain, and source content
 - Convent Linker deduplication: Automatically handle static library ordering hell

C++ specific landmines:
 - Module scanning: C++20 modules require two-phase dependency extraction (scan then compile)
 - Template instantiation islands: You can't know final symbols until link time; plan for lazy instantiation strategies
  - Compiler abstraction leaks: MSVC's `/Zc:__STDC__` vs GCC's `-fdeclspec` can't be fully hidden

Start here:
 - Design your build manifest format first (TOML is fine)
 - Make the dependency extractor a separate, blazing-fast executable
 - Treat header-only libraries as first-class citizens with virtual "link" steps

Phase 0 decision:
 - Manifest filename: scb.toml
 - First implementation milestone: in-memory manifest model + deterministic resolver
 - No TOML parser, compiler invocation, scheduler, cache, dependency scanner, modules, package manager, tests auto-discovery, or generated files yet
 - Split "target" by layer:
   - ManifestTarget: user-authored declaration
   - ResolvedTarget: normalized/defaulted/validated buildable unit
   - ActionNode: future concrete build graph step

Phase 0 conservative zero-config:
 - src/main.cpp infers an executable target named after the project directory
 - src/lib.cpp or include/ infers a library target named after the project directory
 - If both are inferred, the executable depends on the library
 - tests/, examples/, benchmarks, src/bin/, generated files, and modules are deferred

Example future scb.toml shape:

```toml
[project]
name = "hello"
version = "0.1.0"
standard = "c++20"

[build]
include_dirs = ["include"]
defines = { HELLO_VERSION = "1" }

[profile.release]
optimization = "speed"
debug_info = false

[[target]]
name = "hello"
kind = "exe"
sources.include = ["src/main.cpp"]
deps = ["lib:core"]

[[target]]
name = "core"
kind = "static-lib"
sources.include = ["src/**/*.cpp"]
sources.exclude = ["src/main.cpp"]
include_dirs = ["include"]
```
