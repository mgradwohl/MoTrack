# MoTrack

Read and follow [AGENTS.md](../AGENTS.md), the canonical shared agent guide, and
[CONTRIBUTING.md](../CONTRIBUTING.md) for commands and C++ conventions.
Apply the [review checklist](../docs/code-review.md) to proposed changes.

MoTrack is a C++23 / Qt 6 desktop application for Windows, macOS, and Linux. It uses FFmpeg for video
and libarchive for Tracker experiments. The project model and scientific analysis stay independent
of GUI and decoder APIs. TaskSmack's SDL/ImGui layers and Windows/Linux-only assumptions do not apply.

Keep shared workflow, authorization, and cleanup rules in `AGENTS.md`; this file is only an entry point.
