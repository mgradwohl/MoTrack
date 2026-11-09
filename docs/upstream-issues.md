# Tracker issue audit and MoTrack triage

Audited **2026-11-07** against [OpenSourcePhysics/tracker issues](https://github.com/OpenSourcePhysics/tracker/issues).
Reviewed all **64 issues: 52 open and 12 closed**, including their complete comment threads.
The repository searches `is:issue`, `is:issue is:open` and `is:issue is:closed`, ordered by creation
with a 100-result limit, returned 64, 52 and 12 respectively. Pull requests were excluded.
This is a dated issue/discussion audit, not a claim that every attached experiment, screenshot or
historical environment was reproduced. Refresh live status and comments before implementing a case.

The [full-port plan](migration-plan.md) owns requirements and dependency order. This audit supplies
regressions and feature decisions for that plan; it does not expand the frozen baseline to include every
upstream request. The 32 corresponding MoTrack issues are **24 bug/regression tasks, two investigations
and six accepted feature/workflow tasks**. They cover 44 distinct upstream reports plus two defects
observed in our reference tests. Related reports share a filing only when they feed one native
acceptance area; grouping does not assert a common root cause.

## Working policy and priority

1. Before implementing a port slice, consult this audit and its linked MoTrack issues, then refresh the
   relevant upstream discussion. Reproduce reported failures with a small licensed/provenanced fixture
   when possible. An open issue is not proof of a current defect; a closed issue can still supply a regression.
2. Fix reproduced data loss, crashes and incorrect scientific results in an already supported workflow
   before unrelated feature work. Current compatibility investigations start with [MoTrack #63](https://github.com/mgradwohl/MoTrack/issues/63) (vector sums)
   and [MoTrack #64](https://github.com/mgradwohl/MoTrack/issues/64) (unvisited table state), alongside phase-3 media save/reopen [MoTrack #65](https://github.com/mgradwohl/MoTrack/issues/65) and numeric/time
   input [MoTrack #66](https://github.com/mgradwohl/MoTrack/issues/66), [MoTrack #67](https://github.com/mgradwohl/MoTrack/issues/67) as their affected paths are implemented.
3. Keep planned feature work in the dependency order of phases 1–12. **P1** below means required in its
   prerequisite phase, not permission to jump ahead of standard-file preservation. **P2** means useful
   follow-up/design work; **P3** means research or optimization after baseline correctness.
4. For every accepted report, retain a link to the original, the tested version/platform and an explicit
   reproduction status. Require a regression or documented cannot-reproduce/not-applicable disposition,
   fixing PR and acceptance evidence before closing the MoTrack issue. Preserve independently expected
   scientific values even when Tracker disagrees; classify a reference bug rather than normalize it away.
5. Keep upstream status independent of native status. Java/Swing/Xuggle-specific causes need not be ported,
   but native install, lifecycle and resource behavior still require tests. Architecture alone is not a fix.
6. Deferred proposals remain visible here with a decision and prerequisites. They are not accepted work,
   parity exceptions, promised implementation dates or permission to publish releases. Re-triage them
   when prerequisites or the maintainer's priorities change. New accepted work gets a corresponding
   MoTrack issue; do not silently add it to the full-port completion contract.

This triage does not close any behavior issue. Each implementation PR should link the applicable issue,
record the test evidence and update the plan. The broader post-port review in
[MoTrack #39](https://github.com/mgradwohl/MoTrack/issues/39) remains separate from correctness and
compatibility work that must happen during the port.

## Defects discovered by the reference harness

| Observation | Evidence and current limit | MoTrack tracking |
| --- | --- | --- |
| Tracker 6.3.5 loses vector-sum members and computed samples on normal load/resave | [PR #61](https://github.com/mgradwohl/MoTrack/pull/61) reproduces the defect with pinned Java and independent expected sums. [PR #62](https://github.com/mgradwohl/MoTrack/pull/62) preserves source links and refuses affected edited standard exports. These are mitigation and diagnosis, not accepted dependent-vector round trips. | [MoTrack #63](https://github.com/mgradwohl/MoTrack/issues/63) |
| Immediate resave of an unvisited custom table can lose columns | [PR #59](https://github.com/mgradwohl/MoTrack/pull/59) deliberately visits custom tables and restores selection before its reference resave. That accepted case does not prove unvisited-view fidelity. | [MoTrack #64](https://github.com/mgradwohl/MoTrack/issues/64) |
| Tracker 6.3.5 cannot extract the pinned AVI movie from an original archive | [PR #139 initial CI](https://github.com/mgradwohl/MoTrack/actions/runs/42565821290/job/128991986521) fails before native edits. Pinned bytecode omits AVI from extraction registration. The original case stays outside accepted application round trips; the proposed native writer restricts movie archives to audited MOV files, with an independent MOV reference required. Observed 2026-11-09, after the initial audit. | [MoTrack #140](https://github.com/mgradwohl/MoTrack/issues/140) |

No matching upstream filing was identified in the initial audit for its first two observations. Neither has been filed
in Tracker by this work. The later AVI/archive issue search also found no matching filing; #140 records its source link and separate reproduction. Tracker #14 is a momentum-analysis request, not the vector-sum reload defect.
The native issues record the evidence and must be linked to an upstream report if one is subsequently
published. The [corpus](../tests/corpus/README.md) remains the authority for pinned bytes, procedures,
tolerances and deliberately unaccepted reference results.

## Feature-request decisions

Accepted rows cover existing baseline workflows or applicable native release requirements. Partial
acceptance explicitly separates the useful baseline from a proposed extension. Features already fixed
upstream still require native regression coverage. No legal conclusion is inferred from issue comments.

| Original | Decision | Priority / order | Scope, tracking and reason |
| --- | --- | --- | --- |
| [Tracker #7](https://github.com/OpenSourcePhysics/tracker/issues/7) | Required port | P1 / phase 12 | Non-root/native installation acceptance; [MoTrack #83](https://github.com/mgradwohl/MoTrack/issues/83). Verify supported package privileges; do not port Xuggle. |
| [Tracker #12](https://github.com/OpenSourcePhysics/tracker/issues/12) | Required port; upstream fixed | P1 / phase 5 | Two-click calibration and visible endpoints; [MoTrack #78](https://github.com/mgradwohl/MoTrack/issues/78). Preserve the existing workflow and its regression. |
| [Tracker #14](https://github.com/OpenSourcePhysics/tracker/issues/14) | Partially accepted | P1 / phases 5, 8 | Center-of-mass total momentum and Data Tool column combination; [MoTrack #89](https://github.com/mgradwohl/MoTrack/issues/89). Defer new arbitrary cross-track expressions until dependency, gap and persistence semantics are designed. |
| [Tracker #19](https://github.com/OpenSourcePhysics/tracker/issues/19) | Required port | P1 / phase 7 | Live/fast AutoTracker modes; [MoTrack #90](https://github.com/mgradwohl/MoTrack/issues/90). Decouple rendering from processing; identical scientific results and responsive cancellation. |
| [Tracker #20](https://github.com/OpenSourcePhysics/tracker/issues/20) | Existing foundation | Ongoing / R01 | Documented setup and clean build; README / CONTRIBUTING. Already present; keep setup CI and fix concrete gaps rather than open a duplicate. |
| [Tracker #23](https://github.com/OpenSourcePhysics/tracker/issues/23) | Existing foundation | Ongoing / R01 | Native build instructions; README / CONTRIBUTING. Windows/macOS/Linux coverage; FreeBSD is a separate platform proposal. |
| [Tracker #30](https://github.com/OpenSourcePhysics/tracker/issues/30) | Existing foundation plus planned help | Ongoing / R25 | Installation and user guidance; README / R25. Finish baseline help/diagnostics; do not claim current docs are complete. |
| [Tracker #46](https://github.com/OpenSourcePhysics/tracker/issues/46) | Applicable portion accepted | P1 / phase 11 | Robust desktop library URLs; [MoTrack #92](https://github.com/mgradwohl/MoTrack/issues/92). Extensionless/query-bearing media and archives; browser execution stays outside scope. |
| [Tracker #51](https://github.com/OpenSourcePhysics/tracker/issues/51) | Required port | P1 / phases 5, 8 | Cumulative traveled distance; [MoTrack #91](https://github.com/mgradwohl/MoTrack/issues/91). Independent segment-sum expectations, units, gaps, editing and persistence. |
| [Tracker #58](https://github.com/OpenSourcePhysics/tracker/issues/58) | Applicable portion accepted | P1 / phase 12 | Accessible installer/license notices; [MoTrack #93](https://github.com/mgradwohl/MoTrack/issues/93). Inspect actual UX; keep approved license. Disputed legal arguments and dual licensing are not accepted. |
| [Tracker #70](https://github.com/OpenSourcePhysics/tracker/issues/70) | Required native install workflow | P1 / phase 12 | Windows per-user installation; [MoTrack #83](https://github.com/mgradwohl/MoTrack/issues/83). Verify without elevation; deployment must not alter unrelated runtimes. |
| [Tracker #29](https://github.com/OpenSourcePhysics/tracker/issues/29) | Packaging decision pending | P1 decision / before phase 12 | Reliable supported Linux package; [MoTrack #95](https://github.com/mgradwohl/MoTrack/issues/95). Choose distribution/format after clean-machine tests; AppImage and Flatpak are candidates, not both commitments. This decision is separate from #82's root-XDG installation regression. |
| [Tracker #27](https://github.com/OpenSourcePhysics/tracker/issues/27) | Deferred proposal | P2 / after full data export | Direct XLSX export; No implementation issue. Useful classroom improvement. First complete column/text/precision/locale export; then assess a maintained native writer and round-trip spreadsheet tests. |
| [Tracker #53](https://github.com/OpenSourcePhysics/tracker/issues/53) | Deferred proposal | P2 / after task lifecycle/recovery | Optional sleep inhibition; No implementation issue. Use a scoped OS lease only during active work; release on all completion paths, respect manual sleep and test resume. No global power-setting changes. |
| [Tracker #16](https://github.com/OpenSourcePhysics/tracker/issues/16) | Deferred proposal | P3 / after AutoTracker parity | Stop when a target remains still; No implementation issue. Upstream declined. Need thresholds in defined units, noise/gap behavior and false-stop tests; clip limits already address a simpler workflow. |
| [Tracker #47](https://github.com/OpenSourcePhysics/tracker/issues/47) | Deferred proposal | P3 / tracking research | Rotation-aware needle/line tracking; No implementation issue. Define rotation center/angle, interpolation and independent accuracy benchmarks before promising a new matcher. |
| [Tracker #32](https://github.com/OpenSourcePhysics/tracker/issues/32) | Deferred proposal | P3 / measured optimization | GPU acceleration; No implementation issue. Profile first; require CPU fallback, reproducibility, device coverage and a demonstrated gain within memory budgets. |
| [Tracker #33](https://github.com/OpenSourcePhysics/tracker/issues/33) | Deferred proposal | P3 / tracking research | ML-based tracking; No implementation issue. Requires representative evaluation data, uncertainty/failure behavior, native deployment and model provenance/licensing decisions. |
| [Tracker #75](https://github.com/OpenSourcePhysics/tracker/issues/75) | Deferred scientific design | P2 design; implementation unscheduled | User-specified and propagated uncertainty; No implementation issue. Separate random/systematic error, correlations, calibration and time uncertainty; matching confidence is not measurement uncertainty. R17 baseline fit uncertainty remains required. |
| [Tracker #60](https://github.com/OpenSourcePhysics/tracker/issues/60) | Deferred distribution proposal | P2 / after native Linux packaging | Nix package; No implementation issue. Needs a tested package/upgrade path and an identified maintenance owner; does not block desktop parity. |
| [Tracker #37](https://github.com/OpenSourcePhysics/tracker/issues/37) | Deferred distribution proposal | P2 / after release readiness | Microsoft Store; No implementation issue. Depends on clean packages, signing, update policy, store review and explicit publication authority. |
| [Tracker #63](https://github.com/OpenSourcePhysics/tracker/issues/63) | Deferred distribution proposal | P2 / after release readiness | Apple Store; No implementation issue. Depends on clean packages/signing, sandbox and licensing review, and explicit publication authority; no compatibility claim yet. |
| [Tracker #31](https://github.com/OpenSourcePhysics/tracker/issues/31) | Outside frozen scope | Separate product/platform decision | Browser/Chromebook execution; No implementation issue. The port is native desktop. A browser build or Chromebook-specific support needs an explicit scope decision. |

The automatic uncertainty proposal in Tracker #75 is broader than the baseline Data Tool/Fit Builder
uncertainty work in R17. Deferring that proposal does not remove existing Tracker uncertainty workflows
from the port. Likewise, deferring direct cross-track expressions does not remove the existing Data Tool
column-combination workflow accepted in [MoTrack #89](https://github.com/mgradwohl/MoTrack/issues/89).

## Complete issue inventory

Upstream state is the audited state on 2026-11-07. Native filings remain open until their acceptance
criteria are resolved with evidence; historical closed reports below are not being reopened upstream.

| Original report | Upstream state | Disposition / evidence | Native tracking |
| --- | --- | --- | --- |
| [Tracker #1](https://github.com/OpenSourcePhysics/tracker/issues/1) — Acceleration off in free fall | Closed | Unconfirmed scientific discrepancy; independent timing/acceleration investigation, not a proven gravity algorithm defect. | [MoTrack #87](https://github.com/mgradwohl/MoTrack/issues/87) |
| [Tracker #3](https://github.com/OpenSourcePhysics/tracker/issues/3) — Window Size | Open | Historical window-size report; fresh/restored geometry acceptance. | [MoTrack #77](https://github.com/mgradwohl/MoTrack/issues/77) |
| [Tracker #4](https://github.com/OpenSourcePhysics/tracker/issues/4) — Would you tell me why tracker need to be open-source? | Open | Open-source rationale/governance; no product defect or native implementation task. | See disposition; no new implementation issue. |
| [Tracker #5](https://github.com/OpenSourcePhysics/tracker/issues/5) — Installer breaks other Java apps | Open | Maintainer/reporter describe a 4.87 improvement despite open status; preserve runtime isolation. | [MoTrack #83](https://github.com/mgradwohl/MoTrack/issues/83) |
| [Tracker #6](https://github.com/OpenSourcePhysics/tracker/issues/6) — Tracker cannot open files from a file manager when a space is in the path | Open | Acknowledged file-manager path-splitting defect; test real OS entry points. | [MoTrack #68](https://github.com/mgradwohl/MoTrack/issues/68) |
| [Tracker #7](https://github.com/OpenSourcePhysics/tracker/issues/7) — Can I install Tracker/Xuggle on Linux without being root? | Open | Non-root installation workflow; native packaging acceptance, not Xuggle installation. | [MoTrack #83](https://github.com/mgradwohl/MoTrack/issues/83) |
| [Tracker #8](https://github.com/OpenSourcePhysics/tracker/issues/8) — High-dpi display  | Closed | Closed with reporter-confirmed font-size workaround; retain DPI/accessibility regression. | [MoTrack #77](https://github.com/mgradwohl/MoTrack/issues/77) |
| [Tracker #9](https://github.com/OpenSourcePhysics/tracker/issues/9) — Video File Size Limit | Open | Memory-pressure report; bounded end-to-end workloads, not only a decoder-cache claim. | [MoTrack #72](https://github.com/mgradwohl/MoTrack/issues/72) |
| [Tracker #10](https://github.com/OpenSourcePhysics/tracker/issues/10) — Save/load does now work correctly in tracker 4.97 | Open | Historical save/load loss; obtain a fixture and test all open/save entry points. | [MoTrack #65](https://github.com/mgradwohl/MoTrack/issues/65) |
| [Tracker #11](https://github.com/OpenSourcePhysics/tracker/issues/11) — Creating KeyFrame reads autoload_search_paths from osp.prefs, not tracker.prefs | Open | AutoTracker must not trigger unrelated FitBuilder autoload; Data Tool's own autoload remains legitimate. | [MoTrack #74](https://github.com/mgradwohl/MoTrack/issues/74) |
| [Tracker #12](https://github.com/OpenSourcePhysics/tracker/issues/12) — [feature request] Quick calibration stick | Closed | Two-click calibration implemented/confirmed upstream; preserve baseline workflow. | [MoTrack #78](https://github.com/mgradwohl/MoTrack/issues/78) |
| [Tracker #13](https://github.com/OpenSourcePhysics/tracker/issues/13) — 2nd end of calibration stick won't place | Open | Second endpoint report not reproduced by maintainer; investigate and cover placement. | [MoTrack #78](https://github.com/mgradwohl/MoTrack/issues/78) |
| [Tracker #14](https://github.com/OpenSourcePhysics/tracker/issues/14) — Calculate sum of momentums in Tracker | Open | Accept existing center-of-mass/Data Tool solution; defer new arbitrary cross-track table/expression extension. | [MoTrack #89](https://github.com/mgradwohl/MoTrack/issues/89) |
| [Tracker #15](https://github.com/OpenSourcePhysics/tracker/issues/15) — Linux installation doesn't work | Open | Missing root XDG profile; grouped native Linux installer acceptance. | [MoTrack #82](https://github.com/mgradwohl/MoTrack/issues/82) |
| [Tracker #16](https://github.com/OpenSourcePhysics/tracker/issues/16) — [feature request] Autostop of autotracker | Open | Defer stationary-target autostop; upstream declined it. Need noise thresholds/false-stop study after baseline tracking. | See disposition; no new implementation issue. |
| [Tracker #17](https://github.com/OpenSourcePhysics/tracker/issues/17) — [feature request] Treat the dot as a valid decimal delimiter | Open | Acknowledged decimal input problem; avoid truncation and ambiguous grouping. | [MoTrack #66](https://github.com/mgradwohl/MoTrack/issues/66) |
| [Tracker #18](https://github.com/OpenSourcePhysics/tracker/issues/18) — [bug] Autosearch in a tab stops when closing another tab | Open | Tracking state leaks across tab closure; test independent sessions/jobs. | [MoTrack #69](https://github.com/mgradwohl/MoTrack/issues/69) |
| [Tracker #19](https://github.com/OpenSourcePhysics/tracker/issues/19) — [feature request] Disable autoplay while autotracking | Open | Accept live/fast rendering modes already in R11; numerical results must agree. | [MoTrack #90](https://github.com/mgradwohl/MoTrack/issues/90) |
| [Tracker #20](https://github.com/OpenSourcePhysics/tracker/issues/20) — [request] Guidelines for building Tracker from source | Open | Build documentation exists in README/CONTRIBUTING and setup CI (R01); keep clean-setup checks, no duplicate bug. | See disposition; no new implementation issue. |
| [Tracker #21](https://github.com/OpenSourcePhysics/tracker/issues/21) — [request] Adding new members to the organisation | Open | Organization membership/governance; outside native product scope. | See disposition; no new implementation issue. |
| [Tracker #23](https://github.com/OpenSourcePhysics/tracker/issues/23) — Please add build instructions | Open | Native build guidance exists (R01); FreeBSD-specific support is not in the declared three-platform scope. | See disposition; no new implementation issue. |
| [Tracker #24](https://github.com/OpenSourcePhysics/tracker/issues/24) — Drawing "Match" with sub-pixel accuracy in AutoTracker wizard | Open | Subpixel results with integer-rounded preview; separate numerical and rendering checks. | [MoTrack #71](https://github.com/mgradwohl/MoTrack/issues/71) |
| [Tracker #25](https://github.com/OpenSourcePhysics/tracker/issues/25) — Running `java-ffmpeg` version on ReactOS / winxp | Open | ReactOS/Windows XP decoder support is outside the currently planned platform matrix; no legacy-OS commitment. | See disposition; no new implementation issue. |
| [Tracker #26](https://github.com/OpenSourcePhysics/tracker/issues/26) — Cannot install in Manjaro/Arch linux | Open | Same root-XDG failure family; native Linux installer acceptance. | [MoTrack #82](https://github.com/mgradwohl/MoTrack/issues/82) |
| [Tracker #27](https://github.com/OpenSourcePhysics/tracker/issues/27) — [Feature request] Export data to Excel file | Open | Defer direct XLSX export until full column/precision export is complete; native dependency decision required. | See disposition; no new implementation issue. |
| [Tracker #28](https://github.com/OpenSourcePhysics/tracker/issues/28) — Maximize window broken on HiRes | Open | High-resolution maximization failure; native display-layout acceptance. | [MoTrack #77](https://github.com/mgradwohl/MoTrack/issues/77) |
| [Tracker #29](https://github.com/OpenSourcePhysics/tracker/issues/29) — AppImage or flatpak packaging? | Open | Accept reliable Linux distribution, defer choosing AppImage versus Flatpak to the R27 packaging decision; no promise of both. | [MoTrack #95](https://github.com/mgradwohl/MoTrack/issues/95) |
| [Tracker #30](https://github.com/OpenSourcePhysics/tracker/issues/30) — No Documentation | Open | Setup/user docs have a native foundation; help and diagnostics remain R25/R27, no specific reproduced bug. | See disposition; no new implementation issue. |
| [Tracker #31](https://github.com/OpenSourcePhysics/tracker/issues/31) — Tracker for chromebook? | Open | Browser/Chromebook execution is outside desktop-port scope; Linux-on-Chromebook support would need its own platform decision. | See disposition; no new implementation issue. |
| [Tracker #32](https://github.com/OpenSourcePhysics/tracker/issues/32) — GPU Acceleration | Open | Defer GPU acceleration until profiling demonstrates a bottleneck after bounded CPU baseline correctness. | See disposition; no new implementation issue. |
| [Tracker #33](https://github.com/OpenSourcePhysics/tracker/issues/33) — ML-based Autotracker | Open | Defer ML tracking; needs evaluated datasets, model/dependency policy and accuracy/failure criteria. | See disposition; no new implementation issue. |
| [Tracker #34](https://github.com/OpenSourcePhysics/tracker/issues/34) — Is there a way to change the location of .tracker.prefs | Open | Per-user writable preferences on shared computers; no permission-changing workaround. | [MoTrack #75](https://github.com/mgradwohl/MoTrack/issues/75) |
| [Tracker #35](https://github.com/OpenSourcePhysics/tracker/issues/35) — Autotracker crashes when the window is maximized on Mac | Open | Unconfirmed macOS AutoTracker hang; interactive window/lifecycle acceptance. | [MoTrack #70](https://github.com/mgradwohl/MoTrack/issues/70) |
| [Tracker #36](https://github.com/OpenSourcePhysics/tracker/issues/36) — Installation on Ubuntu 20.04 crashes | Open | Root-XDG installer assumption; grouped with related Linux reports. | [MoTrack #82](https://github.com/mgradwohl/MoTrack/issues/82) |
| [Tracker #37](https://github.com/OpenSourcePhysics/tracker/issues/37) — Microsoft Store release | Open | Defer Microsoft Store distribution until tested packaging/signing, update ownership and publication approval. | See disposition; no new implementation issue. |
| [Tracker #38](https://github.com/OpenSourcePhysics/tracker/issues/38) — Error on installation (Ubuntu 20.04) | Open | Root-XDG installer assumption; grouped with related Linux reports. | [MoTrack #82](https://github.com/mgradwohl/MoTrack/issues/82) |
| [Tracker #39](https://github.com/OpenSourcePhysics/tracker/issues/39) — Installer splash window shown in the middle of 2 displays | Open | Installer placement on multiple monitors; test actual package UI. | [MoTrack #84](https://github.com/mgradwohl/MoTrack/issues/84) |
| [Tracker #40](https://github.com/OpenSourcePhysics/tracker/issues/40) — I can't read content of installer when I use dark mode on Ubuntu 20.04 | Open | Installer dark-theme readability; test actual package UI and keyboard access. | [MoTrack #84](https://github.com/mgradwohl/MoTrack/issues/84) |
| [Tracker #41](https://github.com/OpenSourcePhysics/tracker/issues/41) — NullPointerException thrown when using tracker jar with Xuggle on linux | Open | Java/Xuggle startup exception is not code to port; verify deployed native runtime and startup diagnostics. | [MoTrack #83](https://github.com/mgradwohl/MoTrack/issues/83) |
| [Tracker #42](https://github.com/OpenSourcePhysics/tracker/issues/42) — On Linux Xuggle not available if /tmp/xuggle ends up owned by another user (e.g. root if as part of install) | Open | Shared /tmp ownership failure reported again in 6.3.1; native private cache/temp acceptance. | [MoTrack #76](https://github.com/mgradwohl/MoTrack/issues/76) |
| [Tracker #43](https://github.com/OpenSourcePhysics/tracker/issues/43) — After launch, the principal window is not loaded | Open | Unconfirmed Java startup failure redirected to forum; native clean-system launch/diagnostics. | [MoTrack #83](https://github.com/mgradwohl/MoTrack/issues/83) |
| [Tracker #44](https://github.com/OpenSourcePhysics/tracker/issues/44) — Right window Plot/Table stays not hidden | Open | Maintainer acknowledged hidden pane reappearing and planned fix; current release not verified. | [MoTrack #79](https://github.com/mgradwohl/MoTrack/issues/79) |
| [Tracker #45](https://github.com/OpenSourcePhysics/tracker/issues/45) — Errors on compilation | Open | Old-source/dependency compilation report; native pinned setup/build CI is the response, not porting Java build errors. | See disposition; no new implementation issue. |
| [Tracker #46](https://github.com/OpenSourcePhysics/tracker/issues/46) — TrackerJS needs to be able to load videos using URL | Closed | Closed browser URL recognition issue; accept applicable desktop library identification, exclude SwingJS arguments. | [MoTrack #92](https://github.com/mgradwohl/MoTrack/issues/92) |
| [Tracker #47](https://github.com/OpenSourcePhysics/tracker/issues/47) — Tracker is not good at tracking a rotating line such as a gauge needle | Open | Defer rotation-aware matching; requires template resampling, angle/center semantics and performance/accuracy experiments. | See disposition; no new implementation issue. |
| [Tracker #48](https://github.com/OpenSourcePhysics/tracker/issues/48) — Exporting text columns | Closed | Closed with maintainer-reported fix; preserve text-column export as a native regression. | [MoTrack #80](https://github.com/mgradwohl/MoTrack/issues/80) |
| [Tracker #50](https://github.com/OpenSourcePhysics/tracker/issues/50) —  physical constraints resolution | Closed | Uncertainty/resolution usage question; distinguish measurement uncertainty from pixel/frame resolution in R17/R25 help. | See disposition; no new implementation issue. |
| [Tracker #51](https://github.com/OpenSourcePhysics/tracker/issues/51) — Is there a way to calculate total distance moved? | Closed | Accept cumulative path length already in R06/R14; define missing-sample semantics. | [MoTrack #91](https://github.com/mgradwohl/MoTrack/issues/91) |
| [Tracker #52](https://github.com/OpenSourcePhysics/tracker/issues/52) — osp-tracker 6.1.6 installation failed on osx | Closed | Closed with installer fix reported; macOS package resource/upgrade regression. | [MoTrack #85](https://github.com/mgradwohl/MoTrack/issues/85) |
| [Tracker #53](https://github.com/OpenSourcePhysics/tracker/issues/53) — [Suggestion] Tracker does not prevent windows from putting the PC on sleep | Open | Defer optional sleep inhibition until cancel/recovery works; any future lease must end on finish/cancel/error and respect explicit sleep. | See disposition; no new implementation issue. |
| [Tracker #58](https://github.com/OpenSourcePhysics/tracker/issues/58) — The GPL is not a EULA (and its disclaimers do not protect you) | Open | Accept accessible installer/license presentation review; no adoption of disputed legal claims or relicensing proposal. | [MoTrack #93](https://github.com/mgradwohl/MoTrack/issues/93) |
| [Tracker #59](https://github.com/OpenSourcePhysics/tracker/issues/59) — Tracking objects outside of measurement bar not showing up | Open | Insufficient reproduction; investigate visibility conditions and record outcome rather than declaring a fix. | [MoTrack #88](https://github.com/mgradwohl/MoTrack/issues/88) |
| [Tracker #60](https://github.com/OpenSourcePhysics/tracker/issues/60) — Create package for nix | Open | Defer Nix package pending baseline Linux distribution and identified maintenance owner. | See disposition; no new implementation issue. |
| [Tracker #61](https://github.com/OpenSourcePhysics/tracker/issues/61) — can't upload videos in newer versions after 6.1.1 | Open | Media regression lacks sample; establish native codec matrix and seek a licensed reproducible case. | [MoTrack #73](https://github.com/mgradwohl/MoTrack/issues/73) |
| [Tracker #62](https://github.com/OpenSourcePhysics/tracker/issues/62) — . | Closed | Closed empty issue; no actionable request. | See disposition; no new implementation issue. |
| [Tracker #63](https://github.com/OpenSourcePhysics/tracker/issues/63) — Apple app store release | Open | Defer Apple Store distribution pending package/signing readiness, licensing review and publication decision; no legal conclusion here. | See disposition; no new implementation issue. |
| [Tracker #64](https://github.com/OpenSourcePhysics/tracker/issues/64) — Failed to Configure the environment of Java Runtime Environment 1.8.0_00(64bit) in windows11 | Closed | Closed possible Java installation problem; native no-Java clean-system launch acceptance. | [MoTrack #83](https://github.com/mgradwohl/MoTrack/issues/83) |
| [Tracker #65](https://github.com/OpenSourcePhysics/tracker/issues/65) — Snapshot graph -> abysmal image quality | Open | Small snapshot stretched; source-maximize workaround exists. Render native output at requested resolution. | [MoTrack #81](https://github.com/mgradwohl/MoTrack/issues/81) |
| [Tracker #66](https://github.com/OpenSourcePhysics/tracker/issues/66) — "Set time to zero" does not properly work with non-english language | Open | Locale-dependent time-zero initialization; reproduce without opening the time dialog first. | [MoTrack #67](https://github.com/mgradwohl/MoTrack/issues/67) |
| [Tracker #67](https://github.com/OpenSourcePhysics/tracker/issues/67) — Tracker connects to internet despite check for upgrades being disabled | Open | Maintainer confirms counting/connectivity requests persist; deliberate native privacy/offline correction. | [MoTrack #86](https://github.com/mgradwohl/MoTrack/issues/86) |
| [Tracker #68](https://github.com/OpenSourcePhysics/tracker/issues/68) — Can't install on macOS 26.x | Closed | Closed, fixed in 6.3.3 and reporter-confirmed; avoid obsolete macOS registration commands. | [MoTrack #85](https://github.com/mgradwohl/MoTrack/issues/85) |
| [Tracker #69](https://github.com/OpenSourcePhysics/tracker/issues/69) — Fail to install Tracker v6.3.2 under macOS Tahoe 26.1 | Closed | Maintainer reports a 6.3.3 fix; reporter confirms downloading it, not installation success. Grouped macOS package regression. | [MoTrack #85](https://github.com/mgradwohl/MoTrack/issues/85) |
| [Tracker #70](https://github.com/OpenSourcePhysics/tracker/issues/70) — Allow installing on Windows without admin | Open | Accept standard-user Windows installation; silent elevated install does not satisfy it. | [MoTrack #83](https://github.com/mgradwohl/MoTrack/issues/83) |
| [Tracker #75](https://github.com/OpenSourcePhysics/tracker/issues/75) — [Feature request] adding uncertainty values to tracks | Open | Defer uncertainty propagation design; user estimates, covariance and calibration/timing systematics need explicit scientific treatment. | See disposition; no new implementation issue. |
