#!/usr/bin/env python3
"""Build a predictive text language model (.lm) for lib/textinput.

Input: plain sentence files (one sentence per line, or Tatoeba/Leipzig TSV
where the sentence is the last column). Output: a compact binary model with
lexicon, unigrams, and pruned bigrams/trigrams with backoff, sized to a byte
budget. The format is described in lmformat.py and read by
lib/textinput/src/model.c.

Example:
  build.py --lang de --vocab 20000 --budget 180000 -o ../../lm/de.lm \\
      corpora/deu_sentences.tsv corpora/deu-de_web-public_2019_300K-sentences.txt
"""

import argparse
import collections
import math
import random
import re
import sys
import unicodedata

import numpy as np

import lmformat as F

WORD_RE = re.compile(r"[^\W\d_]+(?:['’-][^\W\d_]+)*|\d+|[.!?]+|\S")
SENT_END = re.compile(r"^[.!?]+$")


def read_sentences(paths, limit=None):
    for path in paths:
        n = 0
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.rstrip("\n")
                if "\t" in line:
                    line = line.rsplit("\t", 1)[1]
                line = unicodedata.normalize("NFC", line).replace("’", "'")
                if line:
                    yield line
                    n += 1
                    if limit and n >= limit:
                        break


def case_class(w):
    if w.islower():
        return 0
    if w[0].isupper() and (len(w) == 1 or w[1:].islower()):
        return 1
    if w.isupper() and len(w) > 1:
        return 2
    return -1  # mixed case: not representable


def tokenize(sentence, alphabet):
    """Split into segments of words. A segment ends at numbers, unusable
    tokens and sentence ends. Returns [(starts_sentence, [words])]."""
    segments = []
    cur = []
    at_start = True
    seg_start = True

    def flush(next_is_start):
        nonlocal cur, seg_start
        if cur:
            segments.append((seg_start, cur))
        cur = []
        seg_start = next_is_start

    for tok in WORD_RE.findall(sentence):
        if SENT_END.match(tok):
            flush(True)
            at_start = True
            continue
        if not tok[0].isalpha():
            # Commas, quotes, brackets: transparent. Numbers and other
            # symbols: context break.
            if tok in ",;:\"'()[]«»„“”‚‘-–—/":
                continue
            flush(False)
            continue
        if case_class(tok) < 0 or len(tok) > F.MAX_WORD_LEN or \
                any(c not in alphabet for c in tok.lower()):
            flush(False)
            continue
        cur.append(tok)
        at_start = False
    flush(True)
    return segments


# Web debris that is not worth a lexicon slot.
JUNK = {"http", "https", "www", "html", "htm", "php", "aspx", "jpg", "pdf"}
# Single letters are typed with multitap; only real one-letter words are kept.
SINGLE_LETTER_WORDS = {"a", "I"}


def usable(w):
    if w.lower() in JUNK:
        return False
    return len(w) > 1 or w in SINGLE_LETTER_WORDS


def build_vocab(segments_iter, vocab_size):
    forms = collections.Counter()           # all occurrences per form
    noninitial = collections.Counter()      # occurrences not at sentence start
    for starts, words in segments_iter:
        for i, w in enumerate(words):
            forms[w] += 1
            if not (starts and i == 0):
                noninitial[w] += 1

    # Truecasing: a sentence-initial token counts as its most frequent
    # non-initial form with the same lowercase spelling.
    by_lower = collections.defaultdict(list)
    for w, c in noninitial.items():
        by_lower[w.lower()].append((c, w))
    best_form = {lw: max(lst)[1] for lw, lst in by_lower.items()}

    def truecase(w, initial):
        if initial:
            return best_form.get(w.lower(), w)
        return w

    counts = collections.Counter()
    for w, c in noninitial.items():
        counts[w] += c
    for w, c in forms.items():
        init = c - noninitial.get(w, 0)
        if init:
            counts[truecase(w, True)] += init

    # Drop rare case variants (< 5 % of the spelling) and pick the top words.
    total_lower = collections.Counter()
    for w, c in counts.items():
        total_lower[w.lower()] += c
    cands = [(c, w) for w, c in counts.items()
             if c >= 0.05 * total_lower[w.lower()] and c >= 2 and usable(w)]
    cands.sort(reverse=True)
    vocab = [w for c, w in cands[:vocab_size]]
    return vocab, truecase


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("corpus", nargs="+")
    ap.add_argument("--lang", required=True, choices=sorted(F.ALPHABETS))
    ap.add_argument("--vocab", type=int, default=20000)
    ap.add_argument("--budget", type=int, default=180000, help="model size in bytes")
    ap.add_argument("--trigram-share", type=float, default=0.3,
                    help="share of the n-gram budget for trigrams")
    ap.add_argument("--heldout", type=float, default=0.02,
                    help="share of sentences kept out for evaluation")
    ap.add_argument("--heldout-out", help="write held-out sentences here")
    ap.add_argument("--limit", type=int, help="max sentences per corpus file")
    ap.add_argument("--rare-words", default="Tom,Mary,Maria",
                    help="words over-represented in the corpus (Tatoeba's example "
                         "names); sentences containing them are downsampled")
    ap.add_argument("--rare-keep", type=float, default=0.05,
                    help="share of the sentences with rare words that are kept")
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()

    alphabet = F.ALPHABETS[args.lang]
    rnd = random.Random(1)

    print("reading corpus ...", file=sys.stderr)
    rare = re.compile(r"\b(" + "|".join(map(re.escape, args.rare_words.split(","))) + r")\b") \
        if args.rare_words else None
    train, heldout = [], []
    dropped = 0
    for s in read_sentences(args.corpus, args.limit):
        if rare and rare.search(s) and rnd.random() >= args.rare_keep:
            dropped += 1
            continue
        (heldout if rnd.random() < args.heldout else train).append(s)
    if args.heldout_out:
        with open(args.heldout_out, "w", encoding="utf-8") as f:
            f.write("\n".join(heldout) + "\n")
    print(f"  {len(train)} training, {len(heldout)} held-out sentences, "
          f"{dropped} dropped (rare words)", file=sys.stderr)

    segments = []
    for s in train:
        segments.extend(tokenize(s, alphabet))
    vocab, truecase = build_vocab(segments, args.vocab)
    print(f"  vocabulary: {len(vocab)} words", file=sys.stderr)

    model = F.ModelBuilder(args.lang, vocab)
    V = model.V

    # Token stream: word ids, BOS = V at sentence starts, -1 = break.
    stream = []
    for starts, words in segments:
        if starts:
            stream.append(V)
        for i, w in enumerate(words):
            if starts and i == 0:
                w = truecase(w, True)
            stream.append(model.id_of.get(w, -1))
        stream.append(-1)
    a = np.array(stream, dtype=np.int64)
    print(f"  {int((a >= 0).sum() - (a == V).sum())} word tokens", file=sys.stderr)

    model.count(a)
    model.estimate()
    data = model.encode(args.budget, args.trigram_share)
    with open(args.output, "wb") as f:
        f.write(data)
    print(f"wrote {args.output}: {len(data)} bytes", file=sys.stderr)
    model.report()


if __name__ == "__main__":
    main()
