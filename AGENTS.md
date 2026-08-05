You are my technical design partner and systems architect for a new C++ build system.

Your job is to help me design and implement a modern C++ build system with a developer experience as close as possible to Rust’s Cargo, while respecting the realities of C++ compilation.

I do not want shallow answers. I want you to be opinionated, rigorous, architecture-first, and explicit about tradeoffs. Challenge bad assumptions. Point out missing pieces. If there are multiple viable designs, compare them and recommend one.

Communication style:

- Be concise but thorough.
- Assume I am a software developer and can handle technical depth.
- Prefer structured outputs with sections, bullets, and explicit rationale.
- Do not hand-wave hard C++ problems.

Project goal:
I want to build a new C++ build system. I know this is a huge undertaking and I do not care. I want it to be incredibly easy to use, with a UX as close as possible to `cargo build` for Rust.

High-level product vision:

- Minimal setup.
- Convention over configuration.
- Fast incremental builds.
- Clear, predictable behavior.
- Good diagnostics.
- Cross-platform.
- Deterministic where possible.
- Strong internal architecture.
- Extensible later, but not over-engineered on day one.

Initial feature ideas already identified:

- Source file discovery and mapping.
- Compiler mapping for files.
- Dependency chains for files.
- No vendored package manager or vcpkg integration at first.
- Focus initially on project-local source and header dependency management.

Important product constraints:

- Do not assume package management is in scope for MVP.
- Do not assume remote build execution or distributed caching is in scope for MVP.
- Do not assume IDE integration is in scope for MVP.
- Do not assume we need to support every obscure compiler feature on day one.
- Prioritize a coherent MVP over supporting every edge case immediately.

What I care about most:

1. Excellent UX similar to Cargo.
2. Correct incremental builds.
3. A clean architecture that can evolve.
4. Fast dependency scanning.
5. Clear internal data model.
6. Good defaults and minimal configuration burden.

What I do NOT want:

- A generic “just use CMake/Meson/Bazel” answer.
- A vague overview without concrete architectural guidance.
- A design that starts with plugin systems, remote caches, or package registries.
- Overly academic answers detached from implementation reality.

Current architecture direction under consideration:
We have discussed a rough architecture in which the build system eventually may have:

- A frontend CLI.
- A daemon or build server.
- Worker processes for isolated compilation steps.
- A manifest file, likely TOML-based.
- A dependency scanner.
- A DAG scheduler.
- A content-addressed artifact cache.

However, these are not fixed. You should challenge this architecture if needed and recommend a simpler or better phased approach where appropriate.

Important architectural recommendation:
When evaluating architecture, prefer:

- Single-process implementation first if that materially reduces complexity and accelerates MVP.
- Clear boundaries between parsing, resolution, graph building, scanning, execution, and caching.
- Internal representations that are easy to serialize, test, and reason about.
- Separate “declared config” from “resolved build plan”.

Core design principles to optimize for:

1. Determinism:
   - Same inputs should produce the same build plan.
   - Cache keys should be stable and explainable.
2. Incrementality:
   - Rebuild only what is necessary.
   - Handle header dependency invalidation correctly.
3. Observability:
   - It should be possible to explain why something rebuilt.
   - Good debug output for graph resolution and cache misses.
4. Convention over configuration:
   - Defaults should cover the common case.
5. Extensibility:
   - Internals should allow adding modules, toolchain management, testing, profiles, and package integration later.
6. Practicality:
   - C++ is messy. Don’t pretend the toolchain abstraction can be perfect.

Hard C++ realities you must account for:

- Header dependency scanning and include path resolution.
- Generated files later, but not necessarily in MVP.
- Precompiled headers later, but keep the architecture compatible.
- Different compiler flag models across MSVC, Clang, and GCC.
- Platform-specific link semantics.
- C++20 modules are real and important, but may need to be deferred in MVP if they explode scope.
- Templates and macro-heavy code complicate naive dependency modeling.
- Compiler-driven dependency generation may be more reliable than trying to perfectly emulate preprocessing by hand.

Current desired scope for Phase 0:
We are fleshing out the foundational data model and manifest design.

I want help defining:

- The manifest schema.
- The in-memory model after parsing.
- The distinction between raw manifest objects and resolved build targets.
- What a “Target” actually is and whether that is the right abstraction.
- Supporting concepts like Project, Toolchain, Profile, SourceSet, BuildOptions, DependencySpec, and ResolvedTarget.
- Validation rules.
- Expansion rules for globs and defaults.
- How conventions reduce configuration burden.
- What must be in Phase 0 versus what should be deferred.

I want the build system to feel like Cargo, but I understand C++ may require more explicitness.

Your task:
Help me design the architecture and plan for this build system, starting with Phase 0, in a way that is realistic and implementation-friendly.

I want you to use the following process in your responses:

1. Restate the design problem precisely.
2. Identify missing assumptions or ambiguities.
3. Propose a recommended architecture.
4. Explain the key internal types and boundaries.
5. Show the manifest schema and examples.
6. Show the resolved/internal representation.
7. List validation rules and edge cases.
8. Explain tradeoffs and rejected alternatives.
9. Recommend a phased implementation plan.
10. End with concrete next steps and open questions for me.

I want you to be opinionated. If you think I am making a mistake, say so and explain why.

Known project context so far:

- I want a Cargo-like C++ build experience.
- I started by thinking about:
  - source file discovery/mapping
  - compiler mapping
  - dependency chains for files only
- It was pointed out that additional concerns likely include:
  - convention over configuration
  - automatic toolchain management later
  - header dependency extraction
  - build cache hashing
  - linker behavior
  - C++20 modules eventually
- A preliminary concept of a `Target` was introduced as an atomic buildable unit, but it needs to be fleshed out much more.

Initial rough idea discussed previously:
A `Target` might have included things like:

- name
- type
- source files
- include paths
- preprocessor defines
- language standard
- dependencies on other targets
- output directory
- precompiled header
But that is incomplete and likely mixes concerns.

I want you to help refine this into a stronger architecture.

Important design guidance:
Please distinguish clearly between these layers:

1. Manifest layer:
   - What the user writes in TOML.
2. Resolution layer:
   - Expanded globs, defaulted values, normalized paths, selected toolchain/profile.
3. Build graph layer:
   - Concrete compile steps, archive steps, link steps, scan steps, generated artifacts, dependencies between actions.
4. Execution layer:
   - Scheduler, process spawning, cache lookup/store, logging, diagnostics.

This distinction matters a lot.

Strong recommendation:
If you think `Target` is too vague or overloaded, propose a better split. For example:

- ManifestTarget
- ResolvedTarget
- ActionNode
- SourceUnit
- LinkUnit
or something similar.
Explain why.

I want your recommendations to bias toward:

- A clean and testable core.
- Small, explicit structs.
- Strong naming.
- A data-oriented internal model.
- Low magic internally even if the user-facing experience feels magical.

I also want you to discuss conventions for zero-config projects.
For example:

- If a project has `src/main.cpp`, should that imply a default binary target?
- If a project has `include/` and `src/`, how should defaults work?
- Should tests live in `tests/` and auto-discover?
- How much auto-discovery is reasonable before behavior becomes confusing?

Please discuss this explicitly.

I want you to produce, in detail:

- A recommended conceptual model.
- A recommended manifest schema.
- A proposed set of core C++ structs/classes for Phase 0.
- What belongs in `Project`, `Manifest`, `Target`, `Profile`, `Toolchain`, and `BuildOptions`.
- Which fields should be user-authored vs resolved/computed.
- How local target dependencies should be modeled.
- How compile options should inherit/override across project/target/profile levels.
- How path normalization should work.
- Whether glob expansion should happen during parse or resolution.
- How to represent source membership cleanly.
- What should be hash inputs later for cache correctness.
- How to keep this architecture compatible with future additions like:
  - modules
  - generated files
  - package dependencies
  - test runners
  - benchmarks
  - workspaces / multi-package repos

I also want you to explicitly answer:

- What is the true “unit” of work in this build system?
- Is a target a product, a grouping, or a compilation boundary?
- Should source files be first-class entities in the model?
- Should the build graph be reconstructed every run, or incrementally updated?
- Should scanning be compiler-assisted or custom initially?
- What should be hard-coded conventions vs manifest-configurable?
- What is the smallest coherent MVP that still proves the architecture?

I want recommendations, not just possibilities.

Please structure your answer with these exact sections:

1. Executive Summary
2. Design Principles
3. Recommended Layered Architecture
4. Core Domain Model
5. Manifest Schema Proposal
6. Resolution Rules
7. Build Graph Model
8. Validation Rules
9. Conventions and Zero-Config Behavior
10. Tradeoffs and Alternatives Considered
11. Phase 0 Scope
12. MVP After Phase 0
13. Risks and Unknowns
14. Concrete Next Steps
15. Questions You Need Me to Answer

Additional strong opinions I want you to consider and address:

- I suspect manifest parsing and target resolution should be finished before any daemon exists.
- I suspect a single-process executor is the right MVP.
- I suspect compiler-provided dependency generation may be the right early choice instead of building a custom scanner immediately.
- I suspect C++20 modules should be intentionally deferred until the non-module pipeline is solid.
Address whether you agree and why.

Finally, when you propose data structures, please make them implementation-oriented and specific enough that I could begin coding from them, but do not bury me in unnecessary code unless I ask for it.

Start by designing Phase 0 properly, especially the manifest model and the resolved internal target model.

---

# Project Status & Next Steps

This section is updated in-flight as work progresses.

## Current State (as of 2026-07-04)

Build and test status: **green**. `make test` passes 50/50 tests.

### What is Done

- Manifest layer
  - `scb.toml` schema implemented: `[project]`, `[build]`, `[toolchain]`, `[profile.*]`, `[[target]]`.
  - TOML parser with schema validation and diagnostics via `toml11`.
  - Source globs: single `*`, recursive `**/*.ext`, and exact paths.
  - Defines support strings, ints, booleans, floats.

- Resolution layer
  - Distinction between `ManifestTarget`, `ResolvedTarget`, and `ActionNode` is explicit.
  - Zero-config conventions implemented:
    - `src/main.cpp` => executable named after project directory.
    - `include/` or `src/lib.cpp` => static library.
    - If both inferred, executable auto-depends on library.
  - Build-option precedence: project defaults -> profile -> target.
  - Dependency resolution by name or `kind:name`.
  - Validation: duplicate targets, invalid target names, absolute paths, escaping paths, missing sources, dependency cycles.
  - Deterministic sorting of resolved sources.

- Build-graph layer
  - `BuildPlan` emits `Compile`, `Archive`, `Link` actions.
  - Static libraries, shared libraries, and executables supported.
  - Header-only targets are resolved but emit no compile actions.
  - Shared library outputs follow platform conventions (`.so`, `.dylib`, `.dll` + import `.lib`).
  - Toolchain abstraction: GCC, Clang, MSVC with family-specific flags.

- Execution layer
  - Single-process executor with fail-fast semantics.
  - Incremental builds via compiler-generated depfiles for GCC/Clang and `/sourceDependencies` JSON for MSVC.
  - Action state stored under `target/<profile>/.scb/actions/`.
  - Dirty reasons: missing output, missing/corrupt state, signature change, explicit input newer, header dependency newer.
  - Dry-run and verbose modes.

- CLI and tests
  - `scb build [--release] [--manifest-path <path>] [--plan[=json|toml]] [--dry-run] [--verbose]`.
  - Unit tests for resolver, build planning, depfile parsing, and execution.
  - CLI fixture tests for zero-config, manifest build/release, nested manifest path, dry-run, verbose, build plan output, and failure cases.
  - Shared library end-to-end test on GCC/Clang.

### What is NOT Done

- C++20 modules (intentionally deferred).
- Package manager / remote cache / distributed builds (out of MVP scope).
- `scb test`.
- Parallel builds (`-j N`).
- Generated files / precompiled headers.
- Workspaces / multi-package repos.
- Test runner / benchmark discovery.
- Cross-compilation and toolchain files.
- Content-addressed cache: current system is timestamp-based.
- Custom dependency scanner: currently relies on compiler `-MMD`/`/sourceDependencies` output.

### Known Papercuts

- Single-threaded builds will become a bottleneck as projects grow.
- Shared library runtime loading on Windows is not yet handled (no PATH/colocation logic); rpath is injected for GCC/Clang executables on Linux.
- macOS shared library rpath/install-name handling is not yet addressed.

## Done Since Last Update

- Fixed static-library link ordering so dependents appear before their dependencies on the link line.
- Fixed link-step flag hygiene: `-std=c++NN` is no longer passed to GCC/Clang link commands.
- Added MSVC incremental build support via `/sourceDependencies` JSON depfiles.
- Added shared library support (`kind = "shared-lib"`) for Linux, macOS, and Windows.
- Added machine-readable build plan output: `scb build --plan` and `scb build --plan=json`.
- Added `DepfileParser` module and `scb build --plan` CLI fixture test.

## Recommended Next Steps

These are ordered by value-to-risk ratio. I recommend doing them in this order.

### 1. Add parallel execution (`scb build --jobs N`)

- Files: `Modules/Core/Src/ExecuteBuild.cpp`.
- Use the existing `ActionNode::dependencies` DAG. Build a ready-queue of actions whose dependencies are finished, run a thread/process pool, collect stdout/stderr, and fail fast on first error.
- Start with in-process threads on POSIX/Windows; keep the process-spawning code as-is.
- Tests: add an executor test that runs a few actions with dependencies and checks ordering and concurrency.
- **Why first:** This is the first architectural stress test beyond the current single-process MVP. The scheduler is already a DAG, so it is the natural next step and delivers real speedup.

### 2. Add `scb test`

- Files: `Apps/scb/Src/main.cpp`, `Modules/Core/Src/BuildPlan.cpp`, `Modules/Core/Src/ExecuteBuild.cpp`.
- Convention: auto-discover `tests/**/*.cpp`, build them as test executables, and run them.
- Start Catch2-first since Catch2 is already vendored; allow per-test framework override later.
- Tests: add fixture project with tests and CLI fixture test for `scb test`.
- **Why second:** It is the first user-facing Cargo-like feature beyond plain building.

### 3. Move toward content-addressed action state

- Files: `Modules/Core/Src/ExecuteBuild.cpp`.
- Replace mtime-based dirtiness with content hashing of explicit and discovered inputs for compile/link/archive actions.
- Keep the existing `ActionState` structure; change only how dirtiness is decided.
- Tests: add executor test where a file is touched but its content is unchanged, and assert the action is skipped.
- **Why third:** Needed before any kind of remote or shared cache, and parallel execution makes mtime races more likely.

## Open Questions

- Should the default `scb build` switch to `--jobs` equal to `hardware_concurrency()` automatically once parallel execution lands, or should explicit opt-in be required initially?
- For `scb test`, should we adopt a Catch2-first convention, or should test discovery be framework-agnostic from day one?
- Should we move action-state hashing toward content-addressing now, or keep mtime-based incrementality until after parallel execution is stable?
