#!/usr/bin/env python3
"""Evaluate a model on held-out sentences by simulating typing.

Methods, all as keystrokes per character (KSPC; the space after each word
counts as a character and costs one key):
  multitap      each letter pressed as often as its position on the key
  t9-classic    full key sequence, exact matches ranked by unigram only,
                one extra press per rank ('*' = next word)
  t9-context    the same, ranked by the full engine (trigram/bigram context)
  full          the engine with completions and next-word prediction: after
                0..n keys the target may be picked from the first BAR
                candidates (scrolling costs one press per position, accepting
                one press, which also inserts the space)

Words not in the lexicon are excluded and reported as the OOV rate.
"""

import argparse
import sys

import build
import lmformat as F
import tilib

BAR = 3
MULTITAP = {"2": "abc", "3": "def", "4": "ghi", "5": "jkl", "6": "mno", "7": "pqrs",
            "8": "tuv", "9": "wxyz"}


def multitap_presses(word):
    n = 0
    for ch in word.lower():
        k = F.key_of(ch) if ch in F._ALPHABET else None
        letters = MULTITAP.get(k, "")
        base = F.unicodedata.normalize("NFD", ch)[0]
        n += letters.index(base) + 1 if base in letters else 2
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("model")
    ap.add_argument("heldout")
    ap.add_argument("--lang", required=True)
    ap.add_argument("--sentences", type=int, default=2000)
    ap.add_argument("--penalty", type=float, help="completion penalty in bits")
    ap.add_argument("--learn", action="store_true",
                    help="let the user model learn while typing")
    args = ap.parse_args()

    m = tilib.Model(args.model, with_user=args.learn)
    if args.penalty is not None:
        m.set_penalty(args.penalty)
    alphabet = F.ALPHABETS[args.lang]

    sentences = [l.strip() for l in open(args.heldout, encoding="utf-8") if l.strip()]
    sentences = sentences[:args.sentences]

    words = oov = chars = 0
    keys = {"multitap": 0, "t9-classic": 0, "t9-context": 0, "full": 0}
    top1_classic = top1_context = top1_shown = 0
    predicted = 0  # words picked before typing any key

    for s in sentences:
        for starts, seg in build.tokenize(s, alphabet):
            p2, p1 = (-1, m.BOS) if starts else (-1, -1)
            for w in seg:
                wid = m.find(w)
                if wid < 0:
                    oov += 1
                    p2, p1 = p1, -1
                    continue
                ks = m.keys(wid)
                n = len(ks)
                words += 1
                chars += n + 1
                keys["multitap"] += multitap_presses(w) + 1

                lo, ex, hi = m.range(ks)
                exact = list(range(lo, ex))
                by_uni = sorted(exact, key=m.unigram)
                rank = by_uni.index(wid)
                keys["t9-classic"] += n + rank + 1
                top1_classic += rank == 0

                cands = m.predict(p2, p1, ks, 16)
                # What a user sees after typing all keys (first candidate,
                # which may be a completion), committed with the space key.
                top1_shown += bool(cands) and cands[0][0] == wid
                ex_ids = [i for i, e, c in cands if e]
                rank = ex_ids.index(wid) if wid in ex_ids else len(ex_ids)
                keys["t9-context"] += n + rank + 1
                top1_context += rank == 0

                best = n + rank + 1
                for j in range(0, n + 1):
                    if j >= best:
                        break
                    bar = [i for i, e, c in m.predict(p2, p1, ks[:j], BAR)]
                    if wid in bar:
                        pos = bar.index(wid)
                        if j + pos + 1 < best:
                            best = j + pos + 1
                            if j == 0:
                                predicted += 1
                keys["full"] += best

                if args.learn:
                    m.learn(p1, w)
                p2, p1 = p1, wid

    print(f"{args.model}: {words} words, OOV {100 * oov / max(words + oov, 1):.1f} %")
    for k, v in keys.items():
        print(f"  {k:11s} KSPC {v / chars:.3f}")
    print(f"  top-1 exact: classic {100 * top1_classic / words:.1f} %, "
          f"context {100 * top1_context / words:.1f} %, "
          f"first shown (incl. completions) {100 * top1_shown / words:.1f} %")
    print(f"  words predicted before the first key: {100 * predicted / words:.1f} %")


if __name__ == "__main__":
    sys.exit(main())
