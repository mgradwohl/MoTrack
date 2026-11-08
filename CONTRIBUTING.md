# Contributing to MoTrack

MoTrack starts from [TaskSmack's conventions](https://github.com/mgradwohl/tasksmack/blob/9d2a973/CONTRIBUTING.md), adapted to Qt and three desktop platforms. The [README](README.md) describes toolchain setup and the [migration plan](docs/migration-plan.md) defines the component boundaries.

## Developer setup

Follow the [setup commands in the README](README.md#build-the-prototype). Use `-DryRun` on Windows or `--dry-run` on Linux and macOS to preview installation. WinGet may ask for administrator access for Visual Studio Build Tools. Ubuntu uses APT and sudo when required. On macOS, install Homebrew if needed. The script opens Apple's Command Line Tools installer if the tools are missing. Rerun setup after the installer finishes.

The setup scripts manage Qt 6.8.3, Qt Linguist Tools, and the LLVM 18 formatter under `.dev-tools`. They build FFmpeg with the pinned vcpkg release and enable the codec, demuxer, and pixel-conversion libraries. The first FFmpeg build can take several minutes. CMake fetches GoogleTest during configuration. An existing Qt 6.8 or newer SDK can also be used through `CMAKE_PREFIX_PATH`. A system FFmpeg 6.1 or newer is supported through pkg-config.

On Windows, vcpkg downloads a private MSYS2 environment under `.dev-tools/vcpkg/downloads/tools/msys2` to run FFmpeg's shell-based build. This supplies Bash and build utilities; MoTrack still uses Clang and the Windows SDK. Contributors do not need to configure MSYS2, and the packaged application does not require an MSYS2 installation.

In each new shell, activate the toolchain and build:

```powershell
. .\tools\dev-env.ps1
.\tools\check-prereqs.ps1
cmake --workflow --preset dev
```

```sh
source tools/dev-env.sh
./tools/check-prereqs.sh
cmake --workflow --preset dev
```

The checks locate the tools and compile and link a Qt probe without fetching project dependencies. Pass `-ToolsOnly` or `--tools-only` to skip the probe.

## C++ style

- Use `PascalCase` for types and enum values, `camelCase` for functions, parameters, locals, and public record fields, `m_PascalCase` for private and protected fields, and `UPPER_SNAKE_CASE` for named `constexpr` variables. Use the lowercase `motrack` root namespace.
- Use Allman braces, four spaces, left-aligned pointer and reference markers, and the formatter's 140-column limit. Include the matching header first, then project, external, and standard headers. Include what the file uses and keep headers self-contained.
- Prefer value semantics and RAII. Use `std::unique_ptr` for exclusive heap ownership and Qt parent ownership for widgets and other parented `QObject` instances. Raw pointers and references are borrows. Document lifetimes that are hard to infer.
- Build in C++23 mode with compiler extensions disabled. Prefer C++23 facilities when they simplify the code and are available in all supported standard libraries. Keep any temporary compatibility code at the affected boundary.
- Use `enum class`, `std::optional`, `std::span`, and standard algorithms when they express the contract clearly. Use `auto` where the type is evident. Check narrowing at file, decoder, and UI boundaries.
- Use Doxygen-style comments for API contracts that need explanation. Document units, coordinate spaces, ownership, and thread constraints. Keep scientific formulas and compatibility decisions traceable to their reference behavior.

## Desktop and analysis

- Keep Qt Widgets, platform APIs, and FFmpeg out of the analysis model. Convert at the boundary. Distinguish pixel coordinates from world coordinates and decoded frame numbers from clip steps and timestamps.
- Perform long-running work off the UI thread. Use cancellable tasks and hand results back to the owning thread before updating widgets. Treat missing, malformed, and unsupported project data as distinct cases where the user can act on the distinction.
- Keep Tracker terminology and workflows where they are clear. Wrap user-facing Qt strings with `tr()` or `QCoreApplication::translate()`. Use placeholders and Qt plural forms. Update translations or mark newly introduced strings unfinished.

## Checks before a pull request

Run the CMake configure, build, and test commands in the README. Configure the benchmark target before linting all sources so the compilation database covers it too.

```sh
git ls-files -z -- '*.cpp' '*.hpp' | xargs -0 clang-format --dry-run --Werror
cmake --preset dev -DMOTRACK_BUILD_BENCHMARKS=ON
git ls-files -z -- 'src/*.cpp' 'tests/*.cpp' 'benchmarks/*.cpp' | xargs -0 clang-tidy -p build/dev
```

CI caches the verified vcpkg checkout and bootstrap executable by platform, architecture and the
media setup script hash. `python tools/media_sdk.py bootstrap` checks the pinned revision and prepares
that executable before library installation. The bootstrap cache is saved at that point, so a later
build or test failure does not force the next run to download the same tool again. Library builds,
installed prefixes and downloads stay outside this cache; vcpkg's existing binary cache continues to
validate compiled dependencies against its build configuration. Cache misses still use the pinned
upstream downloads and can fail during an upstream outage.

Coverage profiles include both the instrumented binary signature and process ID. LLVM's merge mode
preserves counters if a process ID is reused, including separate invocations of the same test executable.
Collection clears only its previous `motrack-*.profraw` profiles before running the suite; the combined
90% production-line gate remains unchanged. See [LLVM's profile filename rules](https://clang.llvm.org/docs/SourceBasedCodeCoverage.html#running-the-instrumented-program).

CI builds and tests on all three OSes. New behavior needs tests through its normal entry points. Use exact comparisons where appropriate and explicit tolerances for scientific results. The combined production line coverage must stay at or above **90%**. Keep suppressions narrow, explain each in the code, and include known compatibility limits in the pull request.

The workspace executable runs in three complementary CTest groups: `workspace` excludes
`DocumentSession.*` and `WorkspaceDocument.*`, while `document_session` and `workspace_document`
run those save/recovery and persistence cases separately. Each retains a 60-second timeout, and
ordinary CTest/coverage collection runs all three groups. This keeps the growing real-file matrices
separate from the UI test budget without omitting cases.

CI uses [workflow concurrency](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/control-workflow-concurrency)
to cancel superseded or duplicate runs for the same pull request. Separate PRs retain independent
checks, including forks with matching branch names. Main-branch pushes use unique run IDs and are
not canceled by this policy. Required checks still have to pass on the revision being merged.

## Pull requests and review

Use a focused branch and the [PR template](.github/pull_request_template.md). Lead with the problem and resulting behavior, then describe validation and compatibility limits. Apply the [review checklist](docs/code-review.md) before opening a PR. Keep formatting-only or unrelated work separate; respond to concrete findings and distinguish blockers from suggestions.

For C++ changes, run formatting, relevant static analysis, and tests before opening the PR. For documentation-only changes, validate content and relative links; a local C++ rebuild is unnecessary. Report the platforms actually tested and link CI evidence when available. Do not mark unchecked platforms as passing. Merge only with the user's authorization, repository protections satisfied, required checks passing on the current revision, and blocking review discussions addressed.

Use the [issue templates](.github/ISSUE_TEMPLATE/) for reproducible defects and compatibility requests. Search existing issues first. Include a minimal fixture and numerical units/reference behavior where relevant; avoid uploading private experiment data.

The shared agent workflow and ongoing cleanup policy live in [AGENTS.md](AGENTS.md). Claude Code and Copilot entry points link to that guide and the one review checklist, rather than duplicating their contents. This adopts TaskSmack's documentation structure without assuming its release branches, labels, pre-commit hooks, or platform-specific rendering architecture exist in MoTrack.

## Build presets

Use `cmake --list-presets=all` for the available configure, build, test, and workflow names. The ordinary presets run on all three platforms. The `win-*` aliases match TaskSmack's Windows naming. CMake uses ccache when installed and respects a compiler launcher explicitly supplied by the developer. To disable automatic discovery, configure with `-DMOTRACK_CCACHE=OFF`.

| Presets | Use |
| --- | --- |
| `debug`, `relwithdebinfo`, `release` | Development, investigation with symbols, and the ordinary release build. |
| `optimized`, `release-compatible`, `unity` | ThinLTO (LLD required on Windows and Linux), a baseline desktop CPU target, and a combined translation-unit build. |
| `coverage`, `asan-ubsan`, `ubsan`, `tsan`, `msan` | LLVM coverage and separate sanitizer builds. Sanitizers require matching Clang runtimes. MSan is Linux-only and requires an instrumented Qt and C++ standard library. |
| `profile`, `benchmark` | A symbolized profiling build and an optimized legacy import benchmark. Run `motrack_import_benchmark <project.trk>` with representative fixtures. |
| `pgo-generate`, `pgo-use`, `pgo-baseline` | Instrument a build, exercise real workloads, merge profiles with matching `llvm-profdata` into `profiles/motrack.profdata`, and compare the optimized build with the uninstrumented baseline. |

## What MoTrack takes from TaskSmack

| Practice | MoTrack approach |
| --- | --- |
| CMake presets, Clang, GoogleTest, and LLVM coverage | Keep the same configure-build-test workflow. Add macOS to the matrix and enforce a fixed 90% floor on the union of measured lines. |
| Editor configuration, clang-format, clang-tidy, and clangd | Use the same naming and formatting style. Start with checks relevant to C++23 and Qt, and add rules as the code grows. |
| Separation of platform code, domain logic, and UI | Apply the same dependency discipline to the Tracker document model, numerical analysis, media decoding, and Qt interface. |
| Sanitizers, fuzzing, and benchmarks | Bring in ASan/UBSan and fuzzing when the project importer grows to handle archives and media. Add benchmarks against representative video and tracking workloads once those paths exist. |
| Release and supply-chain checks | Adopt signed artifacts, dependency scanning, notices, SBOMs, and provenance as part of the first installer milestone. |

TaskSmack's ImGui/SDL stack, privileged helpers, system probes, and release/backport automation serve its system-monitor workload. MoTrack uses Qt and will add platform adapters for its own desktop needs.
