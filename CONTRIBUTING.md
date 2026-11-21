# Contributing to MoTrack

MoTrack starts from [TaskSmack's conventions](https://github.com/mgradwohl/tasksmack/blob/9d2a973/CONTRIBUTING.md), adapted to Qt and three desktop platforms. The [README](README.md) describes toolchain setup and the [migration plan](docs/migration-plan.md) defines the component boundaries.

## Developer setup

Follow the [setup commands in the README](README.md#build-the-prototype). Use `-DryRun` on Windows or `--dry-run` on Linux and macOS to preview every installation, uninstallation and download. The installers ask for administrator approval (UAC) themselves on Windows. Ubuntu uses APT and sudo when required. On macOS, install Homebrew if needed. The script opens Apple's Command Line Tools installer if the tools are missing. Rerun setup after the installer finishes. The scripts install the [pinned toolchain](#pinned-toolchain) and leave a tool that is already present at or above MoTrack's floor in place.

The setup scripts manage Qt 6.8.3 and Qt Linguist Tools under `.dev-tools`. They build FFmpeg with the pinned vcpkg release and enable the codec, demuxer, and pixel-conversion libraries. The first FFmpeg build can take several minutes. CMake fetches GoogleTest during configuration. An existing Qt 6.8 or newer SDK can also be used through `CMAKE_PREFIX_PATH`. A system FFmpeg 6.1 or newer is supported through pkg-config.

Qt comes from [aqtinstall](https://github.com/miurahr/aqtinstall) 3.3.0, installed from PyPI into the `.dev-tools` virtual environment. When PyPI or aqtinstall fails, for example on a network that blocks `files.pythonhosted.org`, `tools/dev_sdk.py` installs the same essential archives directly from Qt's online repository: it reads the package list and each archive's SHA-256 from `download.qt.io` itself (never through a mirror redirect), checks every archive before extracting it with libarchive's `bsdtar`, places each archive where the repository's extract operations say, and writes `bin/qt.conf` with `Prefix=..`. `python tools/dev_sdk.py install --qt-source direct` (or `MOTRACK_QT_SOURCE=direct`) uses that path on purpose; the Ubuntu contributor-setup CI job does, so both paths stay tested. Windows uses its built-in `tar.exe`, macOS `/usr/bin/tar`, and Ubuntu `libarchive-tools`. Older setups installed PyPI's `clang-format` 18 in that virtual environment; `dev_sdk.py install` removes it, and the activation scripts put the pinned LLVM ahead of it until then.

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

The checks report each tool's path and version against the pins, print how to fix anything missing or out of range, and then compile and link a Qt, FFmpeg and libarchive probe without fetching project dependencies. On Linux the probe also rejects a standard library other than GCC 14's libstdc++, and on macOS one other than Apple's libc++. Pass `-ToolsOnly` or `--tools-only` to skip the probe.

### Pinned toolchain

[`tools/toolchain.env`](tools/toolchain.env) is the single list of pinned versions. The setup and check scripts, the activation scripts and CI all read it, so a version changes in one place. The approach follows [TaskSmack's setup scripts](https://github.com/mgradwohl/tasksmack/tree/main/tools): one LLVM major everywhere, installers verified before they run, exact versions where the package manager allows them, and a check that names the fix.

| Component | Windows x64 | Ubuntu 24.04 | macOS |
| --- | --- | --- | --- |
| Compiler | LLVM 23.1.5 `clang++`, from LLVM's MSI checked against its SHA-256 (setup and CI install the same file) | `clang++` 23 from apt.llvm.org's `llvm-toolchain-noble-23` | Apple Clang 17+ (Xcode or Command Line Tools 16.3+; CI uses Xcode 16.4) |
| clang-format, clang-tidy | From the same LLVM 23.1.5 | `clang-format-23`, `clang-tidy-23` from the same repository | Homebrew `llvm@23` (keg-only) |
| Coverage tools | LLVM 23 `llvm-cov`, `llvm-profdata` | LLVM 23 `llvm-cov`, `llvm-profdata` | Apple's, which read Apple Clang's profiles |
| C++ standard library | MSVC STL from Visual Studio 2026 Build Tools (toolset 14.50+; CI has 14.52) and Windows SDK 10.0.26100 | GCC 14's libstdc++ | Apple's libc++ |
| CMake, Ninja, ccache, Python | Setup's WinGet versions: 4.4.4, 1.13.2, 4.14.3, 3.12.10 (CI uses the image's CMake and Ninja and setup-python's 3.12) | Ubuntu's packages (3.28, 1.11, 4.9, 3.12) | Homebrew's, with `python@3.12` |

Every LLVM tool except macOS's Apple Clang and its coverage tools must be the pinned major: another major fails the check, because formatting and static analysis results differ between majors. Windows installs exactly 23.1.5, and another 23.x release there passes with a note. apt.llvm.org and Homebrew publish only the current release of a major, so Linux and macOS take the newest LLVM 23 they serve; the check reports the release. apt.llvm.org's signing key is checked against its pinned fingerprint before APT trusts it. On Windows, setup leaves exactly one LLVM, of the pinned major, in `C:\Program Files\LLVM`: an installation of another major is uninstalled first, all of its installer registrations included, since side-by-side LLVM MSIs share that directory. CMake, Ninja, ccache, Python, Git, the MSVC toolset and Qt are checked against floors (CMake 3.25, Ninja 1.11, Python 3.12, Git 2.30, MSVC 14.50, Qt 6.8; ccache 4.9 is recommended). WinGet keeps only recent versions of a package; when a pin has left its catalog, setup warns and installs the newest version instead.

macOS keeps Apple Clang as the compiler because Qt's macOS binaries link the system libc++. A Homebrew LLVM `clang++` would compile against Homebrew's newer libc++ headers while the application loads the system library at run time. Homebrew's `llvm@23` gives macOS the same clang-format and clang-tidy major as the other platforms without replacing the compiler: it is keg-only, and `tools/dev-env.sh` puts Apple's toolchain ahead of it on `PATH`. Homebrew currently serves `llvm@23` as an alias of its `llvm` formula and keeps a separate `llvm@23` formula once `llvm` moves to 24.

Linux never uses libc++. Qt's Linux binaries use libstdc++, and `std::string` and containers cross the Qt API, so mixing the two libraries breaks the build or the program. Ubuntu 24.04's default GCC is 13, and LLVM Clang picks its newest GCC installation. `tools/dev-env.sh` and CI therefore set `MOTRACK_GCC_INSTALL_DIR` to GCC 14's directory. [`cmake/MoTrackLibstdcxx.cmake`](cmake/MoTrackLibstdcxx.cmake) passes it to every compile and link as `--gcc-install-dir`, CMake's own checks included. A build tree configured before that setting changed stops with a request to reconfigure with `--fresh`.

### C++ standard libraries

C++23 library support differs between the three standard libraries. GCC 14's libstdc++ adds these to GCC 13's: `std::print` and `std::println`, the `std::ranges::to` function, `std::generator`, `std::out_ptr` and `std::inout_ptr`, and `std::forward_like`. It still lacks `std::flat_map` and `std::flat_set` and range formatting (GCC 15), and `std::mdspan` and `std::ranges::starts_with`/`ends_with` (GCC 16).

Apple's libc++ in Xcode 16.4 corresponds roughly to upstream libc++ 19, so it lacks features libc++ added later or has not implemented: `std::flat_map` and `std::flat_set`, `views::zip` and `views::adjacent` (complete only in libc++ 22), `views::enumerate`, `views::stride` and the `ranges::fold_left` family (23), `views::join_with` (21), `views::chunk`, `views::slide` and `views::cartesian_product`, `std::generator`, `std::move_only_function` and `std::stacktrace`. Apple's availability annotations can also restrict features that live in the library binary, such as `std::print`, by deployment target.

Features available in all three libraries include `std::expected`, `std::print`, the `std::ranges::to` function, `std::out_ptr`, `std::forward_like`, `views::chunk_by`, `views::repeat` and `ranges::contains`. These versions come from the [libstdc++](https://gcc.gnu.org/onlinedocs/libstdc++/manual/status.html#status.iso.2023) and [libc++](https://libcxx.llvm.org/Status/Cxx23.html) status pages and [cppreference](https://en.cppreference.com/w/cpp/compiler_support/23). The Apple column is inferred from the libc++ version, so confirm a newly used feature in the macOS CI build before relying on it.

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

Run the CMake configure, build, and test commands in the README. Configure the benchmark target before linting all sources so the compilation database covers it too. Run these from an activated shell, so `clang-format` and `clang-tidy` are the pinned LLVM 23's; `check-prereqs` shows which ones it finds.

```sh
git ls-files -z -- '*.cpp' '*.hpp' | xargs -0 clang-format --dry-run --Werror
cmake --preset dev -DMOTRACK_BUILD_BENCHMARKS=ON
git ls-files -z -- 'src/*.cpp' 'tests/*.cpp' 'benchmarks/*.cpp' | xargs -0 clang-tidy -p build/dev
```

Changes to the setup tools have their own tests, which install nothing: `pwsh -NoProfile -File tools/test-setup-dev.ps1` and `pwsh -NoProfile -File tools/test-check-prereqs.ps1` on Windows, and `python -m unittest discover -s tests -p test_dev_tools.py` everywhere (its Bash cases run on Linux and macOS). The Windows build job runs the PowerShell tests and every build job runs the Python ones.

CI caches the verified vcpkg checkout and bootstrap executable by platform, architecture and the
media setup script hash. `python tools/media_sdk.py bootstrap` checks the pinned revision and prepares
that executable before library installation. The bootstrap cache is saved at that point, so a later
build or test failure does not force the next run to download the same tool again. Library builds,
installed prefixes and downloads stay outside this cache; vcpkg's existing binary cache continues to
validate compiled dependencies against its build configuration. That binary cache is also saved as soon
as the media libraries are built, rather than only when the whole job passes, so a later failure or
timeout does not make the next run rebuild FFmpeg (about 26 minutes on Windows). Cache misses still use
the pinned upstream downloads and can fail during an upstream outage.

Coverage profiles include both the instrumented binary signature and process ID. LLVM's merge mode
preserves counters if a process ID is reused, including separate invocations of the same test executable.
Collection clears only its previous `motrack-*.profraw` profiles before running the suite; the combined
90% production-line gate remains unchanged. See [LLVM's profile filename rules](https://clang.llvm.org/docs/SourceBasedCodeCoverage.html#running-the-instrumented-program).

CI builds and tests on all three OSes. New behavior needs tests through its normal entry points. Use exact comparisons where appropriate and explicit tolerances for scientific results. The combined production line coverage must stay at or above **90%**. Keep suppressions narrow, explain each in the code, and include known compatibility limits in the pull request.

CI installs the [pinned toolchain](#pinned-toolchain) from `tools/toolchain.env`: the LLVM 23.1.5 MSI on
Windows, apt.llvm.org's LLVM 23 with GCC 14's libstdc++ on Ubuntu (whose build job also runs
`tools/check-prereqs.sh --tools-only`), and Apple Clang on macOS. Each contributor-setup job runs
`setup-dev.sh`, which ends with the full `check-prereqs.sh`, then builds and tests the developer preset.
The Ubuntu contributor-setup job runs the full C++ formatting and static-analysis checks with LLVM 23 after its
developer build. It enables and builds the benchmark target before running clang-tidy against
`build/dev`, with at most two analysis processes. This runs alongside the platform build and Tracker
acceptance jobs, giving each workload its own time budget as the reference corpus grows. The final
coverage job waits for both the build and contributor-setup matrices to pass before combining reports.

The workspace executable runs in five complementary CTest groups. General UI cases run in
`workspace`; `workspace_saving` runs `PairedWorkspace.*` and `ImagePackageWorkspace.*`,
`workspace_document` runs `WorkspaceDocument.*`, and the two session groups partition `DocumentSession.*`.
`document_session_journals` includes names matching `*Journal*`; `document_session` uses the complementary
negative filter. Every current or future session case therefore runs exactly once. Each group retains
a 60-second timeout, and ordinary CTest/coverage collection runs all five groups. This separates the
real-file journal matrices whose cumulative runtime caused the intermittent Windows timeout in
[#137](https://github.com/mgradwohl/MoTrack/issues/137), without retries or omitted cases.

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
| Pinned LLVM and setup checks | Use the same LLVM major (23) and Windows MSI, CI-matching WinGet pins, verified apt.llvm.org key, dry-run previews, version checks with fixes, and tests for the scripts. Keep Apple Clang on macOS and libstdc++ on Linux because of Qt's binaries; TaskSmack's Linux libc++, pre-commit hooks and LLVM package caches are not adopted. |
| Separation of platform code, domain logic, and UI | Apply the same dependency discipline to the Tracker document model, numerical analysis, media decoding, and Qt interface. |
| Sanitizers, fuzzing, and benchmarks | Bring in ASan/UBSan and fuzzing when the project importer grows to handle archives and media. Add benchmarks against representative video and tracking workloads once those paths exist. |
| Release and supply-chain checks | Adopt signed artifacts, dependency scanning, notices, SBOMs, and provenance as part of the first installer milestone. |

TaskSmack's ImGui/SDL stack, privileged helpers, system probes, and release/backport automation serve its system-monitor workload. MoTrack uses Qt and will add platform adapters for its own desktop needs.
