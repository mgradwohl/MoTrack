"""Collect and combine LLVM line coverage for MoTrack production sources."""

import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def collect(build, output, llvm):
    build = build.resolve()
    for profile in build.glob("motrack-*.profraw"):
        profile.unlink()
    env = {**os.environ, "LLVM_PROFILE_FILE": (build / "motrack-%p.profraw").as_posix()}
    subprocess.run(["ctest", "--test-dir", build, "--output-on-failure"], env=env, check=True)
    profiles = sorted(build.glob("motrack-*.profraw"))
    if not profiles:
        raise SystemExit("LLVM did not produce coverage profiles.")
    data = build / "motrack.profdata"
    subprocess.run([llvm / "llvm-profdata", "merge", "-sparse", *profiles, "-o", data], check=True)
    app, *tests = (build / "coverage-objects.txt").read_text(encoding="utf-8").splitlines()
    command = [llvm / "llvm-cov", "export", "-format=lcov", f"-instr-profile={data}", app]
    for binary in tests:
        command.extend(["-object", binary])
    lines = {}
    filename = None
    for record in subprocess.check_output(command).decode("utf-8").splitlines():
        if record.startswith("SF:"):
            source = Path(record[3:]).resolve()
            filename = source.relative_to(ROOT).as_posix() if source.is_relative_to(ROOT / "src") else None
        elif record.startswith("DA:") and filename is not None:
            number, count, *_ = record[3:].split(",")
            source_lines = lines.setdefault(filename, {})
            source_lines[number] = source_lines.get(number, False) or int(count) > 0
    expected = {p.relative_to(ROOT).as_posix() for p in (ROOT / "src").rglob("*.cpp")}
    if missing := expected.difference(lines):
        raise SystemExit(f"Missing coverage records for: {', '.join(sorted(missing))}")
    output.write_text(json.dumps(lines, sort_keys=True), encoding="utf-8")


def check(reports):
    merged = {}
    for report in reports:
        for filename, lines in json.loads(report.read_text(encoding="utf-8")).items():
            for number, hit in lines.items():
                key = (filename, number)
                merged[key] = merged.get(key, False) or hit
    if not merged:
        raise SystemExit("No production lines were measured.")
    covered, total = sum(merged.values()), len(merged)
    print(f"Line coverage {100 * covered / total:.2f}% ({covered}/{total})")
    if 100 * covered < 90 * total:
        raise SystemExit("Line coverage is below 90%.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    collector = commands.add_parser("collect")
    collector.add_argument("build", type=Path)
    collector.add_argument("output", type=Path)
    collector.add_argument("--llvm-bin", type=Path, required=True)
    checker = commands.add_parser("check")
    checker.add_argument("reports", type=Path, nargs="+")
    args = parser.parse_args()
    if args.command == "collect":
        collect(args.build, args.output, args.llvm_bin)
    else:
        check(args.reports)
