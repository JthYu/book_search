"""Normalize chapters and generate one UTF-8 token per line for the C index."""

import argparse
from pathlib import Path

import jieba
from opencc import OpenCC

ROOT = Path(__file__).resolve().parents[1]


def read_stopwords(path):
    return {
        line.strip()
        for line in path.read_text(encoding="utf-8-sig").splitlines()
        if line.strip() and not line.lstrip().startswith("#")
    }


def preprocess(source, count, output, stopwords_path, userdict_path):
    converter = OpenCC("t2s")
    tokenizer = jieba.Tokenizer()
    if userdict_path is not None:
        tokenizer.load_userdict(str(userdict_path))
    stopwords = read_stopwords(stopwords_path)

    normalized_dir = output / "normalized"
    tokens_dir = output / "tokens"
    normalized_dir.mkdir(parents=True, exist_ok=True)
    tokens_dir.mkdir(parents=True, exist_ok=True)

    for doc_id in range(1, count + 1):
        filename = f"{doc_id:03d}.txt"
        normalized = converter.convert(
            (source / filename).read_text(encoding="utf-8-sig")
        )
        (normalized_dir / filename).write_text(normalized, encoding="utf-8")
        with (tokens_dir / filename).open("w", encoding="utf-8", newline="\n") as output_file:
            for token in tokenizer.cut(normalized, HMM=False):
                if token.isalnum() and token not in stopwords:
                    output_file.write(token + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("count", type=int)
    parser.add_argument("output", type=Path)
    parser.add_argument("--stopwords", type=Path, default=ROOT / "config/stopwords.txt")
    parser.add_argument("--userdict", type=Path, default=ROOT / "config/userdict.txt")
    args = parser.parse_args()
    if args.count < 1:
        parser.error("count must be positive")
    preprocess(args.source, args.count, args.output, args.stopwords, args.userdict)


if __name__ == "__main__":
    main()
