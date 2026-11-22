# Contributing to MoTrack

MoTrack starts from [TaskSmack's conventions](https://github.com/mgradwohl/tasksmack/blob/9d2a973/CONTRIBUTING.md), adapted to Qt and three desktop platforms. The [README](README.md) describes toolchain setup and the [migration plan](docs/migration-plan.md) defines the component boundaries.

## Developer setup

Follow the [setup commands in the README](README.md#build-the-prototype). Use `-DryRun` on Windows or `--dry-run` on Linux and macOS to preview every installation, uninstallation and download. The installers ask for administrator approval (UAC) themselves on Windows. Ubuntu uses APT and sudo when required. On macOS, install Homebrew if needed. The script opens Apple's Command Line Tools installer if the tools are missing. Rerun setup after the installer finishes. The scripts install the [pinned toolchain](#pinned-toolchain) and leave a tool that is already present at or above MoTrack's floor in place.

The setup scripts manage Qt 6.8.3, Qt Linguist Tools and the Qt Multimedia add-on (cameras for capture) under `.dev-tools`. They build FFmpeg with the pinned vcpkg release and enable the codec, demuxer, and pixel-conversion libraries. The first FFmpeg build can take several minutes. CMake fetches GoogleTest during configuration. An existing Qt 6.8 or newer SDK can also be used through `CMAKE_PREFIX_PATH`. A system FFmpeg 6.1 or newer is supported through pkg-config.

A checkout and all of its linked `git worktree`s share one SDK install. `tools/dev_sdk.py` chooses the `.dev-tools` directory, and the setup and activation scripts and `tools/media_sdk.py` use its choice:

1. The directory `MOTRACK_DEV_TOOLS` names, when it is set. A relative path is relative to the checkout, so setup and activation agree whatever the working directory. The variable also lets separate clones, or the layouts in rule 3 that keep their own directory, share one install.
2. Otherwise the checkout's own `.dev-tools`, when setup has installed an SDK there: a `<platform>/qt` directory or the `vcpkg` checkout. Other contents do not count, such as the corpus cache, macOS tool links or a stray file.
3. Otherwise, in a linked worktree, the main checkout's `.dev-tools`, even before setup has created it. The main checkout is the parent of `git rev-parse --path-format=absolute --git-common-dir`. It counts only when that directory is named `.git` and is the main working tree's own git directory, and the main working tree is a MoTrack checkout. A worktree of a bare or `--separate-git-dir` repository or of a submodule, and MoTrack files inside another repository, keep their own `.dev-tools`.
4. Otherwise the checkout's own `.dev-tools`. This includes the main checkout itself, and running without Git or with Git older than 2.31.

Setup run in a worktree therefore installs into the main checkout instead of downloading a second copy, and no junctions or symlinks are needed. `python tools/dev_sdk.py dev-tools` prints the directory in use. The reference corpus cache of `tools/corpus.py` stays in each checkout's own `.dev-tools/tracker-reference`, because its reports and scratch projects belong to that checkout.

Qt comes from [aqtinstall](https://github.com/miurahr/aqtinstall) 3.3.0, installed from PyPI into the `.dev-tools` virtual environment. When PyPI or aqtinstall fails, for example on a network that blocks `files.pythonhosted.org`, `tools/dev_sdk.py` installs the same essential and Qt Multimedia archives directly from Qt's online repository. It reads the package list from `download.qt.io` itself over HTTPS (never through a mirror redirect), and that list must name exactly the archives in [`tools/qt-archives.sha256`](tools/qt-archives.sha256). Every archive, which may come from a mirror, is checked against the SHA-256 pinned there before libarchive's `bsdtar` extracts it. Qt's servers are therefore trusted only for the package list and the archives' placement, not for the archives' contents. `python tools/dev_sdk.py qt-archive-checksums` regenerates the pins after a Qt version change; review them before committing. The fallback places each archive where the repository's extract operations say and writes `bin/qt.conf` with `Prefix=..`. A Qt installed before Qt Multimedia was required gets just that module from the same pinned archives; files already in the prefix are never replaced or deleted, and a conflicting file stops the amendment before anything is added (`--qt-source aqt` uses aqt's `--noarchives` amendment instead). [Camera capture](docs/camera-capture.md#platform-requirements) lists what Qt Multimedia bundles and what each platform needs. `python tools/dev_sdk.py install --qt-source direct` (or `MOTRACK_QT_SOURCE=direct`) uses that path on purpose; the Ubuntu contributor-setup CI job does, so both paths stay tested. Windows uses its built-in `tar.exe`, macOS `/usr/bin/tar`, and Ubuntu `libarchive-tools`. Older setups installed PyPI's `clang-format` 18 in that virtual environment; `dev_sdk.py install` removes it, and the activation scripts put the pinned LLVM ahead of it until then.

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

Every LLVM tool except macOS's Apple Clang and its coverage tools must be the pinned major: another major fails the check, because formatting and static analysis results differ between majors. Windows installs exactly 23.1.5, and another 23.x release there passes with a note. apt.llvm.org and Homebrew publish only the current release of a major, so Linux and macOS take the newest LLVM 23 they serve; the check reports the release. apt.llvm.org's signing key is checked against its pinned fingerprint before APT trusts it. On Windows, setup leaves exactly one LLVM, of the pinned major, in `C:\Program Files\LLVM`. An installation of another major there has to be uninstalled first, all of its installer registrations included, since side-by-side LLVM installers share that directory. Setup does that only when given `-ReplaceLlvm`, and only after the new MSI has been downloaded and verified. Without `-ReplaceLlvm` it stops and lists what it would remove; `-DryRun -ReplaceLlvm` previews the removal. LLVM installed in any other directory is reported and left alone. For NSIS installations, which record no install location, setup reads the directory from the uninstaller's path. CMake, Ninja, ccache, Python, Git, the MSVC toolset and Qt are checked against floors (CMake 3.25, Ninja 1.11, Python 3.12, Git 2.30, MSVC 14.50, Qt 6.8; ccache 4.9 is recommended). Visual Studio Build Tools (WinGet's current 2026 release) and Git are installed unversioned, and only their floors are checked. WinGet keeps only recent versions of a package; when a pin has left its catalog, setup warns and installs the newest version instead.

macOS keeps Apple Clang as the compiler because Qt's macOS binaries link the system libc++. A Homebrew LLVM `clang++` would compile against Homebrew's newer libc++ headers while the application loads the system library at run time. Homebrew's `llvm@23` gives macOS the same clang-format and clang-tidy major as the other platforms without replacing the compiler: it is keg-only, and `tools/dev-env.sh` puts links to just its clang-format and clang-tidy (in `.dev-tools/macos/llvm-23-tools`) ahead of Apple's toolchain, which ships an older clang-format of its own. The rest of `llvm@23`, its `clang++` included, stays off `PATH`. Homebrew currently serves `llvm@23` as an alias of its `llvm` formula and keeps a separate `llvm@23` formula once `llvm` moves to 24.

Linux builds use libstdc++, never libc++. The one exception is the `msan` preset, which needs a MemorySanitizer-instrumented libc++ and Qt built for it. Qt's Linux binaries use libstdc++, and `std::string` and containers cross the Qt API, so mixing the two libraries breaks the build or the program. Ubuntu 24.04's default GCC is 13, and LLVM Clang picks its newest GCC installation. `tools/dev-env.sh` and CI therefore set `MOTRACK_GCC_INSTALL_DIR` to GCC 14's directory. [`cmake/MoTrackLibstdcxx.cmake`](cmake/MoTrackLibstdcxx.cmake) passes it to every compile and link as `--gcc-install-dir`, CMake's own checks included. A build tree configured before that setting changed stops with a request to reconfigure with `--fresh`.

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
- Keep Tracker terminology and workflows where they are clear. Wrap user-facing Qt strings with `tr()` or `QCoreApplication::translate()`. Use placeholders and Qt plural forms. Add new strings to every catalog as described in [Translations](#translations).

## Translations

`translations/motrack_<locale>.ts` holds one Qt catalog per non-English locale in Tracker's `Tracker.getLocales()`
(SwingJS branch). `tools/translations.py` (standard-library Python) lists them in `LOCALES`; CMake's
`motrack_ts_files` must match. Tracker's `fil_PH`, `sn` and `sr` catalogs are not in its language list and its
`ca_UTF` file duplicates Catalan, so MoTrack does not ship them. `translations/motrack_en.ts`
(`PLURALS_TS_FILE`) holds only the English plural forms; write them by hand. The catalogs are hand-maintained in
a compact layout; the tool edits only the messages it adds or fills, and Tracker-seeded entries while they keep
their provenance comment. It never changes a human or reviewed translation.

After adding or changing a user-visible string, run from an activated developer shell (lupdate on PATH):

```sh
python tools/translations.py update
python tools/translations.py check --lrelease "$(command -v lrelease)"
```

`update` adds the string, empty and `unfinished`, to every catalog. CTest's `translation_catalogs` test fails while
a catalog lacks a source string or has an invalid translation (placeholders, `[*]`, file patterns, line breaks,
one `&` mnemonic when the source has one, Qt's plural-form count for the language), and checks that lrelease keeps
every unfinished entry. `translation_tool` tests the tool itself.

To fill new entries, seed them from Tracker, then machine-translate the rest:

1. Clone [tracker](https://github.com/OpenSourcePhysics/tracker) (`SwingJS`) and
   [osp](https://github.com/OpenSourcePhysics/osp) (`swingJS`) into an empty directory; only their `.properties`
   files are read. Run `python tools/translations.py update --tracker <tracker> --osp <osp>`. Where Tracker's
   English text matches a source string (ignoring mnemonics, ellipses, spacing and a trailing colon), the
   translation is copied with the source's mnemonic and ellipsis and marked finished. Matches that also ignore
   case or terminal punctuation, or where Tracker's keys disagree, stay unfinished. A translator comment records
   the bundle and key. `STALE_KEYS`, `HOMOGRAPH_KEYS` and `LOCALE_REJECTED` exclude Tracker translations that no
   longer mean, or never meant, the MoTrack text ([#274](https://github.com/mgradwohl/MoTrack/issues/274)); a
   seed whose match disappears is emptied for a new translation. Turkish is not seeded because Tracker's
   catalog omits Turkish letters.
2. `python tools/translations.py pending --locale <locale> --out pending.json --tracker <tracker> --osp <osp>
   --reference de fr es` lists the remaining strings with Tracker's terminology, the plural rule and finished
   German/French/Spanish translations for context.
3. Write a JSON object mapping each source string to its translation (a list of plural forms for numerus
   messages) and run `python tools/translations.py import --locale <locale> --input <file>`. Invalid entries are
   reported and nothing is written. Imported entries are `unfinished` with the comment "Machine translation;
   review before marking finished."

By maintainer decision (2026-11-22), unfinished machine translations ship visible without a label; do not pass
`-nounfinished` to lrelease. To review a catalog, open it in Qt Linguist, check the translation against the
English text and the translator comment, correct it, mark it done and remove the provenance comment.
`python tools/translations.py stats` reports finished, unfinished, Tracker-derived and machine-translated counts.
At run time `motrack::UiTranslations` loads the first language in the system's list that has a catalog
(English included, so an English-first list stays English; Hong Kong and Macao use Traditional, Singapore
Simplified Chinese), Qt's own catalog for that language, and a right-to-left layout for Arabic and Hebrew;
without a match the interface is English.

## Text encoding and locale

This follows [TaskSmack's policy](https://github.com/mgradwohl/tasksmack/blob/7a41c20/CONTRIBUTING.md#text-encoding-and-locale), adapted to Qt. Where Tracker's files require something else, the [exceptions](#tracker-compatibility-exceptions) below say so and name their tests.

### Source encoding

Every MoTrack target compiles its sources as UTF-8 with UTF-8 narrow literals (`/utf-8` with clang-cl, `-finput-charset=UTF-8 -fexec-charset=UTF-8` with clang), and every Windows target defines `UNICODE` and `_UNICODE`, so an unsuffixed Win32 macro resolves to its `W` function. Call `W` functions by name; don't use `TCHAR` or the `A` functions. `motrack_apply_text_encoding()` in [cmake/MoTrackTextEncoding.cmake](cmake/MoTrackTextEncoding.cmake) sets both, and the end of `CMakeLists.txt` applies it to every target the project directory defines, so a new target needs nothing; FetchContent dependencies are defined in their own directories and keep their settings.

### Rules

- **Text is UTF-8 or UTF-16.** The Qt-free model uses UTF-8 `std::string`; the UI and paths use `QString`. Convert only with `toStdString`/`fromStdString` (UTF-8 in Qt 6) or `toUtf8`/`fromUtf8`. `toLocal8Bit`/`fromLocal8Bit` convert through the system code page: use them only at a documented OS boundary. Lengths are bytes or UTF-16 units, not characters.
- **`motrack.exe` runs in UTF-8 on Windows.** Its manifest ([src/motrack.manifest](src/motrack.manifest)) sets `activeCodePage` to UTF-8 (Windows 10 1903+), so `GetACP()` is 65001; the `desktop_locale` test checks it on CI, whose system code page is 1252. The test binaries have no manifest, so code must never depend on the code page: convert explicitly.
- **Paths meet text only through [src/filesystem_path.hpp](src/filesystem_path.hpp).** `motrack::toFilesystemPath(QString)` and `fromFilesystemPath(path)` use UTF-16 on Windows and UTF-8 elsewhere; Qt-free code uses `motrack::utf8Path(std::string_view)`, `utf8String(path)` and `genericUtf8String(path)` from [src/utf8_path.hpp](src/utf8_path.hpp). Never `path.string()`, `path(std::string)`, `std::filesystem::u8path` or a narrow `fopen`. Open files with `QFile`/`QSaveFile`, `std::fstream(path)` or a `W` API. libarchive opens archives with `archive_read_open_filename_w` on Windows and `QFile::encodeName` (UTF-8) elsewhere, and on POSIX converts member names through the calling thread's `LC_CTYPE` without iconv, so every archive read or write runs inside `motrack::ScopedUtf8ThreadLocale` ([src/utf8_thread_locale.hpp](src/utf8_thread_locale.hpp)), which never touches the global locale (the checker requires it in any file that calls `archive_read_new` or `archive_write_new`); FFmpeg reads movies through a `QFile`-backed `AVIOContext` with protocols disabled, so the UTF-8 name passed to `avformat_open_input` is only a probe hint; `QImageReader` takes the `QString`. [tests/non_ascii_round_trip_test.cpp](tests/non_ascii_round_trip_test.cpp) saves and reopens Tracker, archive and native files and opens images and a movie under CJK, emoji, accented Latin and Arabic names.
- **The locale is set once per process** by `motrack::configureProcessLocale()` ([src/app_locale.hpp](src/app_locale.hpp)), immediately after the `Q*Application` is constructed: on Unix `QCoreApplication` calls `setlocale(LC_ALL, "")`, so until then `strtod`, `printf("%f")`, `std::to_string(double)` and the C libraries MoTrack links (FFmpeg parses numbers with `strtod`) use the user's decimal mark. It gives the C locale a UTF-8 `LC_CTYPE` (the user's locale on Windows, a UTF-8 fallback where the environment's is not UTF-8) and `LC_NUMERIC` `"C"`. The C++ global locale stays the classic locale: MoTrack takes display formats from `QLocale`, and a default-constructed stream stays locale-independent. Every executable that constructs a `Q*Application`, tests included, calls it. Nothing else calls `setlocale` (except to query it) or `std::locale::global`. The result is logged to the `motrack.locale` category: warnings by default, the summary line with `QT_LOGGING_RULES="motrack.locale.info=true"` (and `QT_FORCE_STDERR_LOGGING=1` on Windows).
- **Displayed numbers use `QLocale`, never the C or C++ global locale.** Data views (the marks table, plot axes) use `motrack::dataDisplayLocale()` ([src/display_locale.hpp](src/display_locale.hpp)): the user's decimal mark and digits without grouping, as Tracker's fixed patterns show data ([#278](https://github.com/mgradwohl/MoTrack/issues/278)). Read-outs and dialogs use `QLocale()`, and typed numbers are parsed with the same `QLocale` (ambiguous input: [#66](https://github.com/mgradwohl/MoTrack/issues/66)). Don't show a number with `QString::number`, `std::format("{:L}")`, `%f` or a hard-coded `.`.
- **Machine-readable text is classic:** Tracker XML, `.motrack` JSON, CSV export, library options and anything read back. Use `std::to_chars`/`std::from_chars`, `QString::number`/`QString::toDouble`, `QLocale::c()`, `QJsonDocument`, or a stream imbued with `std::locale::classic()`. Never use `std::stod`/`strtod`/`atof`/`sscanf`, the `printf` family, `std::to_string(double)` or a default-locale stream on text that is read back. The `locale` test runs the XML, CSV and native paths under de-DE and fr-FR with the C, C++ and Qt locales all switched, skipping a locale the machine lacks; CI generates both on Linux.

### Tracker compatibility exceptions

Tracker is the compatibility target for its own files, so these follow Tracker rather than the rules above where they differ. Evidence is from upstream `SwingJS` [`34b1b50`](https://github.com/OpenSourcePhysics/tracker/tree/34b1b503f6cb55bee031d64ea916bf4f3262504d) and OSP `swingJS` [`d49c6ec`](https://github.com/OpenSourcePhysics/osp/tree/d49c6ecf41f48dc53fa85af1806241b46f7a90b4), confirmed with the pinned Tracker 6.3.5 JAR and JDK 17 under en-US, de-DE and fr-FR (see the PR that introduced this section).

| Data | Tracker | MoTrack |
| --- | --- | --- |
| XMLControl files: `.trk`, `.trz` members, tabsets, `tracker.prefs`, `.xml` | OSP writes UTF-8 with `<?xml version="1.0" encoding="UTF-8"?>` and the platform line separator, CRLF on Windows ([`XMLControlElement`](https://github.com/OpenSourcePhysics/osp/blob/d49c6ecf41f48dc53fa85af1806241b46f7a90b4/src/org/opensourcephysics/controls/XMLControlElement.java#L741-L743), [`XML.NEW_LINE`](https://github.com/OpenSourcePhysics/osp/blob/d49c6ecf41f48dc53fa85af1806241b46f7a90b4/src/org/opensourcephysics/controls/XML.java#L39)). It reads every file as UTF-8 whatever the declaration says and silently turns invalid bytes into U+FFFD ([`readerForStream`](https://github.com/OpenSourcePhysics/osp/blob/d49c6ecf41f48dc53fa85af1806241b46f7a90b4/src/org/opensourcephysics/tools/ResourceLoader.java#L3326-L3333)). Numbers are `Double.toString`/`Integer.toString`, classic in every locale. Tracker's own **Export > Tracker file** writes the JVM default charset under that UTF-8 declaration, typically Windows-1252 on JDK 17 Windows ([#277](https://github.com/mgradwohl/MoTrack/issues/277)). | UTF-8 and Java's number spelling, both ways; MoTrack never substitutes U+FFFD silently. Decided ([#277](https://github.com/mgradwohl/MoTrack/issues/277)): a file that is not valid UTF-8 but decodes as Windows-1252 with no undefined bytes opens with a visible warning naming the assumed encoding and is saved back as UTF-8; any other invalid file is refused with a diagnostic. Until that warning lands ([#302](https://github.com/mgradwohl/MoTrack/issues/302)), the B1 reader refuses every file that is not valid UTF-8, whatever it declares, and names Windows-1252 when the bytes decode as it: `LegacyProject.RefusesWindows1252BytesWhateverTheDeclaration` and `LegacyProject.ReadsDocumentsWithTrackersLineOrientedReader`. The OSP library's rules for bytes, declarations (one other than UTF-8 is refused), a byte-order mark, line endings and `Double.toString` are in [docs/osp-xmlcontrol.md](docs/osp-xmlcontrol.md). Numbers: the `locale` test and the corpus round trips. |
| ZIP member names (`.trz`) | `java.util.zip` writes every name as UTF-8 with the UTF-8 flag (bit 11), ASCII names included, and reads every name as UTF-8 whether or not the flag is set. A name that is not UTF-8 makes `ZipInputStream` and `ZipFile` throw, so Tracker cannot open that archive; there is no CP437 fallback. | Names are read as UTF-8 regardless of the flag, and a non-UTF-8 name refuses the archive with a translated message saying so and that Tracker cannot open it either (libarchive `zip:hdrcharset=UTF-8`, [#275](https://github.com/mgradwohl/MoTrack/issues/275)); an opt-in legacy code-page import is a deferred proposal ([#305](https://github.com/mgradwohl/MoTrack/issues/305)). Member identities are NFC on every platform, since libarchive hands macOS NFD names ([#308](https://github.com/mgradwohl/MoTrack/issues/308)); the stored bytes are kept, and `motrack::archiveMemberKey` normalizes member names that native companions saved before the fix. Only libarchive's name-conversion warning gets the non-UTF-8 message; CRC, size and truncation warnings keep the generic one. New archives get UTF-8 names, flagged when non-ASCII. Saving into an existing archive keeps each name's bytes and flags and accepts a non-ASCII name only when flagged. Tested by `TrackerArchive.*Unflagged*`, `TrackerArchive.NamesOnlyNameConversionWarningsAsNonUtf8Names`, `ArchiveGroup.NormalizesNfdMemberNamesSavedBeforeNfcIdentities` and `NonAsciiRoundTrip.*`. |
| Exported data and copied tables (I7, not yet implemented) | **Export Data** and table **Copy** use `DecimalFormat` with `OSPRuntime.getDecimalFormatSymbols()`: the decimal separator is Tracker's `decimal_separator` preference, else its UI locale's (period or comma), in fixed patterns without grouping. The delimiter defaults to tab (also space, comma, semicolon, custom), rows end with the platform line separator, and exported files are UTF-8 ([`ExportDataDialog`](https://github.com/OpenSourcePhysics/tracker/blob/34b1b503f6cb55bee031d64ea916bf4f3262504d/src/org/opensourcephysics/cabrillo/tracker/ExportDataDialog.java#L627-L628), [`TrackerIO.getData`](https://github.com/OpenSourcePhysics/tracker/blob/34b1b503f6cb55bee031d64ea916bf4f3262504d/src/org/opensourcephysics/cabrillo/tracker/TrackerIO.java#L1614-L1641), [`OSPRuntime`](https://github.com/OpenSourcePhysics/osp/blob/d49c6ecf41f48dc53fa85af1806241b46f7a90b4/src/org/opensourcephysics/display/OSPRuntime.java#L1814-L1854)). Under de-DE and fr-FR the unformatted copy of 1234.5 is `1,234500000E3`. | I7's Export Data and Copy follow Tracker: the user's decimal mark (the I8 preference when it exists), no grouping, Tracker's delimiters, UTF-8, so the text pastes into a spreadsheet in that locale. Today's **Export measurements/plot data** CSV is a MoTrack format for scripts and stays classic: comma-delimited, `.` decimals, CRLF rows, UTF-8. |
| Paths: launch arguments, drops, export file names, preferences | Java on JDK 17 Windows receives `argv` in the ANSI code page. Tracker keeps preferences in `tracker.prefs`, an XMLControl file. | Paths stay `QString` end to end: `QCoreApplication::arguments()` (built from the UTF-16 command line on Windows), never `argv`; `QUrl::toLocalFile` for drops; `QFileOpenEvent` on macOS (the command-line and drop opening work, [#68](https://github.com/mgradwohl/MoTrack/issues/68)). I7's image and video export writes through `QImageWriter`/`QSaveFile` or a `QFile`-backed `AVIOContext`. MoTrack does not read `tracker.prefs`; if it does, the XMLControl row applies. |
| Java `.properties` translations | `ResourceBundle` reads a bundle as UTF-8 and falls back to ISO-8859-1 when it is not valid UTF-8; English uses `Properties.load(InputStream)`, ISO-8859-1. Upstream's files mix raw ISO-8859-1 (`de`, `fr`), `\uXXXX` escapes (`ar`, `ja`) and raw UTF-8 (`ca_UTF`). | `tools/translations.py` (PR [#270](https://github.com/mgradwohl/MoTrack/pull/270)) decodes each *line* as UTF-8, else ISO-8859-1, then applies the `\uXXXX` and other `.properties` escapes, and writes UTF-8 `.ts` catalogs. This deliberately improves on Tracker: a few catalogs mix ISO-8859-1 with UTF-8 lines (for example `tracker_sv.properties`), which Java's whole-file fallback shows as mojibake. |

### Files and tooling

- **Every text file in the repository is UTF-8 without a BOM, with LF line endings** ([.editorconfig](.editorconfig), [.gitattributes](.gitattributes)). Media and archives are `binary`. Test inputs under `tests/fixtures/` and `tests/corpus/fixtures/` are `-text`: Git keeps their exact bytes, which may be CRLF, CR or another encoding on purpose, and `tests/corpus/manifest.json` pins their digests. Never rewrite an existing fixture's bytes.
- **CI checks it** in the Linux contributor-setup job: `tools/check-utf8.py` (strict UTF-8, BOMs, double-encoded text, `.ps1` that Windows PowerShell 5.1 would misread; skips `binary` and `-text` files) and `tools/check-text-apis.py` (the API rules above, plus the Python and PowerShell rules below), with their tests in `tests/test_check_*.py`. A deliberate exception carries `text-api: allowed (reason)` on its line or the line before.
- **PowerShell scripts** stay ASCII to remain Windows PowerShell 5.1-compatible, or start with `#Requires -Version 7`. A script names the encoding on every write (`Set-Content`/`Add-Content`/`Out-File -Encoding utf8`; `-Encoding ascii` only for `.cmd` stubs). Parse a native tool's output only after setting `[Console]::OutputEncoding` to UTF-8, and make the tool write UTF-8 (`python -X utf8`, `vswhere -utf8`); the dot-sourced `tools/dev-env.ps1` does both with `Invoke-MoTrackUtf8`, which restores the user's console encoding ([#279](https://github.com/mgradwohl/MoTrack/issues/279)). Pass `-c core.quotePath=false` to git commands that list paths.
- **Python tools** pass `encoding="utf-8"` to every text `open()`, `read_text()`, `write_text()` and text-mode `subprocess` call (add `errors="replace"` for output from tools outside the repository), and run as `python -I -X utf8`.
- **Java reference sources** compile with `javac -encoding UTF-8`; JDK 17 otherwise reads them in the platform charset ([#276](https://github.com/mgradwohl/MoTrack/issues/276)). Sources run directly with the `java Source.java` launcher, which has no `-encoding` option, stay ASCII; write other characters as `\u` escapes.

## Checks before a pull request

Run the CMake configure, build, and test commands in the README. Configure the benchmark target before linting all sources so the compilation database covers it too. Run these from an activated shell, so `clang-format` and `clang-tidy` are the pinned LLVM 23's; `check-prereqs` shows which ones it finds.

```sh
git ls-files -z -- '*.cpp' '*.hpp' | xargs -0 clang-format --dry-run --Werror
python -I -X utf8 tools/check-utf8.py
python -I -X utf8 tools/check-text-apis.py
python -I -X utf8 -m unittest discover -s tests -p "test_check_*.py"
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

The workspace executable runs in ten complementary CTest groups. General UI cases run in
`workspace`; `workspace_shell` runs the `WorkspaceShell.*` save/revert flows
([#283](https://github.com/mgradwohl/MoTrack/issues/283)); `workspace_saving` runs `PairedWorkspace.*` and
`ImagePackageWorkspace.*`, `workspace_document` runs `WorkspaceDocument.*`, `ui_translations` runs
`UiTranslations.*` (every catalog), `camera_capture` runs the fake-camera `CameraCapture.*` cases, `workspace_text`
runs `NonAsciiRoundTrip.*` and `WorkspaceLocale.*`, `desktop_launch` runs `DesktopLaunch.*` (which also starts the
real `motrack` executable), and the two session groups partition `DocumentSession.*`.
`document_session_journals` includes names matching `*Journal*`; `document_session` uses the complementary
negative filter. Every current or future session case therefore runs exactly once. Each group retains
a 60-second timeout, and ordinary CTest/coverage collection runs all ten groups. This separates the
real-file journal matrices whose cumulative runtime caused the intermittent Windows timeout in
[#137](https://github.com/mgradwohl/MoTrack/issues/137), without retries or omitted cases.

CI uses [workflow concurrency](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/control-workflow-concurrency)
to cancel superseded or duplicate runs for the same pull request. Separate PRs retain independent
checks, including forks with matching branch names. Main-branch pushes use unique run IDs and are
not canceled by this policy. Required checks still have to pass on the revision being merged.

## Pull requests and review

Use a focused branch and the [PR template](.github/pull_request_template.md). Lead with the problem and resulting behavior, then describe validation and compatibility limits. Apply the [review checklist](docs/code-review.md) before opening a PR. Keep formatting-only or unrelated work separate; respond to concrete findings and distinguish blockers from suggestions.

For C++ changes, run formatting, relevant static analysis, and tests before opening the PR. For documentation-only changes, validate content and relative links; a local C++ rebuild is unnecessary. Report the platforms actually tested and link CI evidence when available. Do not mark unchecked platforms as passing. Merge only with the user's authorization, repository protections satisfied, required checks passing on the current revision, and blocking review discussions addressed.

Use the [issue templates](.github/ISSUE_TEMPLATE/) for reproducible defects and compatibility requests. Search existing issues first. Include a minimal fixture and numerical units/reference behavior where relevant; avoid uploading private experiment data. When updating the [Tracker issue triage](docs/upstream-issues.md), run `python -I -X utf8 tools/check_triage_links.py` (it needs `gh` and the network) to list each "Filed upstream as" comment whose upstream issue the triage row does not link.

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
