# Contributing to MoTrack

MoTrack starts from [TaskSmack's conventions](https://github.com/mgradwohl/tasksmack/blob/9d2a973/CONTRIBUTING.md), adapted to Qt and three desktop platforms. The [README](README.md) describes toolchain setup and the [migration plan](docs/migration-plan.md) defines the component boundaries.

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

Run the CMake configure, build, and test commands in the README. The build exports a compilation database for clangd and clang-tidy. Run the LLVM tools against handwritten C++ sources.

```sh
clang-format --dry-run --Werror src/*.cpp src/*.hpp tests/*.cpp
clang-tidy -p build/dev src/*.cpp tests/*.cpp
```

CI builds and tests on all three OSes. New behavior needs tests through its normal entry points. Use exact comparisons where appropriate and explicit tolerances for scientific results. The combined production line coverage must stay at or above **90%**. Keep suppressions narrow, explain each in the code, and include known compatibility limits in the pull request.

## What MoTrack takes from TaskSmack

| Practice | MoTrack approach |
| --- | --- |
| CMake presets, Clang, GoogleTest, and LLVM coverage | Keep the same configure-build-test workflow. Add macOS to the matrix and enforce a fixed 90% floor on the union of measured lines. |
| Editor configuration, clang-format, clang-tidy, and clangd | Use the same naming and formatting style. Start with checks relevant to C++23 and Qt, and add rules as the code grows. |
| Separation of platform code, domain logic, and UI | Apply the same dependency discipline to the Tracker document model, numerical analysis, media decoding, and Qt interface. |
| Sanitizers, fuzzing, and benchmarks | Bring in ASan/UBSan and fuzzing when the project importer grows to handle archives and media. Add benchmarks against representative video and tracking workloads once those paths exist. |
| Release and supply-chain checks | Adopt signed artifacts, dependency scanning, notices, SBOMs, and provenance as part of the first installer milestone. |

TaskSmack's ImGui/SDL stack, privileged helpers, system probes, and release/backport automation serve its system-monitor workload. MoTrack uses Qt and will add platform adapters for its own desktop needs.
