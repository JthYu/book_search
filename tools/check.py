"""Verify the stage 2 regression cases and stage 3 word-query fixtures."""

import argparse
from pathlib import Path
import subprocess

from preprocess import ROOT, preprocess


def verify(executable, directory, count, prefix, word_mode):
    queries = ROOT / "tests" / f"{prefix}_queries.txt"
    expected = (ROOT / "tests" / f"{prefix}_expected.txt").read_text(
        encoding="utf-8-sig"
    ).splitlines()
    command = [str(executable.resolve()), str(directory), str(count), str(queries)]
    if word_mode:
        command.append("--words")
    output = subprocess.check_output(command, encoding="utf-8")
    actual = [line for line in output.splitlines() if " -> " in line]
    if actual != expected:
        for reference, found in zip(expected, actual):
            if reference != found:
                raise SystemExit(f"{prefix}: expected {reference}, received {found}")
        raise SystemExit(f"{prefix}: expected {len(expected)} results, received {len(actual)}")
    print(f"{prefix}: {len(expected)} / {len(expected)} passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    args = parser.parse_args()
    fixture_build = ROOT / "build" / "fixtures"
    preprocess(
        ROOT / "tests" / "fixtures", 4, fixture_build,
        ROOT / "config" / "stopwords.txt", ROOT / "config" / "userdict.txt",
    )
    tokens = set((fixture_build / "tokens" / "004.txt").read_text(
        encoding="utf-8"
    ).splitlines())
    if "的" in tokens or "了" in tokens:
        raise SystemExit("stage3: stopword filter failed")
    verify(args.executable, ROOT / "data" / "hongloumeng", 120, "stage2", False)
    verify(args.executable, fixture_build / "tokens", 4, "stage3", True)


if __name__ == "__main__":
    main()
