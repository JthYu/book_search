"""Compare both CLI modes with independently computed document sets."""

import argparse
import ast
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory


def results(executable, chapters, count, query_path, skip):
    command = [str(executable.resolve()), str(chapters), str(count), str(query_path), "--words"]
    if skip:
        command.append("--skip")
    output = subprocess.check_output(command, encoding="utf-8")
    answers = {}
    for line in output.splitlines():
        if " -> " in line:
            expression, docs = line.split(" -> ", 1)
            answers[expression] = ast.literal_eval(docs)
        elif line.startswith("Mode:"):
            print(line)
    return answers


def verify(executable):
    count = 5000
    common = set(range(1, 4097))
    cluster = set(range(3073, count + 1))
    uneven = {doc for doc in common if doc % 13 < 4}
    even = set(range(2, count + 1, 2))
    odd = set(range(1, count + 1, 2))
    rare = {16, 256, 1024, 4096, 4097, 5000}
    terms = {
        "common": common, "cluster": cluster, "uneven": uneven,
        "even": even, "odd": odd, "rare": rare,
    }
    cases = {
        "common AND cluster": common & cluster,
        "cluster AND common": common & cluster,
        "common AND uneven": common & uneven,
        "common AND rare": common & rare,
        "rare AND common": common & rare,
        "even AND odd": set(),
        "common AND common": common,
        "missing AND common": set(),
        "common OR cluster": common | cluster,
        "common NOT cluster": common - cluster,
        "(common AND cluster) OR (rare NOT common)": (common & cluster) | (rare - common),
        "(common OR rare) AND cluster": (common | rare) & cluster,
        "common AND (cluster OR uneven)": common & (cluster | uneven),
    }
    with TemporaryDirectory() as temporary:
        chapter_dir = Path(temporary) / "chapters"
        chapter_dir.mkdir()
        for doc in range(1, count + 1):
            tokens = [term for term, docs in terms.items() if doc in docs]
            (chapter_dir / f"{doc:03d}.txt").write_text("\n".join(tokens), encoding="utf-8")
        queries = Path(temporary) / "queries.txt"
        queries.write_text("\n".join(cases), encoding="utf-8")

        for skip in (False, True):
            actual = results(executable, chapter_dir, count, queries, skip)
            if set(actual) != set(cases):
                raise SystemExit("Missing or unexpected queries in output.")
            for expression, expected in cases.items():
                if actual[expression] != sorted(expected):
                    raise SystemExit(f"Wrong document IDs for {expression!r} with skip={skip}.")
    print(f"stage4: {len(cases)} / {len(cases)} passed in both modes on {count} synthetic documents")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    verify(parser.parse_args().executable)


if __name__ == "__main__":
    main()
