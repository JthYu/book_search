"""Compare character and word search against literal substring ground truth."""

import argparse
import ast
from pathlib import Path
import subprocess
import tempfile

TERMS = (
    "红楼梦", "贾宝玉", "林黛玉", "薛宝钗", "大观园", "刘姥姥",
    "潇湘馆", "蘅芜苑", "金陵十二钗",
)


def run_queries(executable, directory, count, queries, word_mode):
    with tempfile.TemporaryDirectory() as temporary:
        query_file = Path(temporary) / "queries.txt"
        query_file.write_text("\n".join(queries) + "\n", encoding="utf-8")
        command = [str(executable.resolve()), str(directory), str(count), str(query_file)]
        if word_mode:
            command.append("--words")
        output = subprocess.check_output(command, encoding="utf-8")
    lines = output.splitlines()
    print(lines[0])
    print(lines[1])
    return [
        set(ast.literal_eval(line.split(" -> ", 1)[1]))
        for line in lines
        if " -> " in line
    ]


def rates(found, truth):
    correct = len(found & truth)
    precision = f"{correct / len(found):.1%}" if found else "—"
    recall = f"{correct / len(truth):.1%}" if truth else "—"
    return f"{precision} / {recall}"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument("build_directory", type=Path)
    parser.add_argument("count", type=int)
    args = parser.parse_args()
    normalized = args.build_directory / "normalized"
    chapters = {
        doc_id: (normalized / f"{doc_id:03d}.txt").read_text(encoding="utf-8")
        for doc_id in range(1, args.count + 1)
    }
    print("Character baseline")
    char_results = run_queries(
        args.executable, normalized, args.count,
        [" AND ".join(term) for term in TERMS], False,
    )
    print("Word index")
    word_results = run_queries(
        args.executable, args.build_directory / "tokens", args.count, TERMS, True,
    )
    print()
    print("| 查询词 | 真值章数 | 单字命中 | 单字精确率 / 召回率 | 分词命中 | 分词精确率 / 召回率 |")
    print("|---|---:|---:|---:|---:|---:|")
    for term, chars, words in zip(TERMS, char_results, word_results, strict=True):
        truth = {doc_id for doc_id, chapter in chapters.items() if term in chapter}
        print(
            f"| {term} | {len(truth)} | {len(chars)} | {rates(chars, truth)} | "
            f"{len(words)} | {rates(words, truth)} |"
        )
        if term == "红楼梦":
            print(f"<!-- 分词命中的章号 {sorted(words)} -->")


if __name__ == "__main__":
    main()
