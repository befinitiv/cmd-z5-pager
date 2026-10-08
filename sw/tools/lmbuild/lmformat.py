"""Binary format of the predictive text models (.lm) and the model builder.

All integers are little-endian. Probabilities are stored as costs, i.e.
-log2(p), in units of 1/64 bit (COST_SCALE).

File layout:
  header     magic "CZLM", u16 version, u16 section count, char lang[4],
             u32 V (words; context id V = sentence start), u8 alphabet size,
             u8 lexicon block size, u8 max word length, u8 reserved,
             u16 alphabet[64] (codepoints), u8 keys[64] (ASCII key per symbol)
  directory  per section: u32 id, u32 offset, u32 size
  sections   LEX, UNI, BI, TRI, TOP (see the encode_* functions)

Lexicon: words sorted by (key sequence, lowercase spelling, case), so all
words sharing a key prefix form one contiguous id range, starting with the
exact matches. Front-coded in blocks of BLOCK words. Per word: length of the
prefix shared with the previous word (not stored for the first word of a
block), the remaining lowercase symbols ended by EOW, and the case class
(lower, Capitalized, UPPER). All three use canonical Huffman codes whose code
lengths are stored in the section.

Unigrams: 6-bit costs in steps of UNI_STEP bits, packed MSB first.

Bigram/trigram tables ("posting tables"): for each present context key (a
word id, or for trigrams the global index of a bigram entry) a backoff weight
and the sorted successor ids with quantised costs. Gaps are Rice coded;
anchors every ANCHOR_EVERY contexts allow random access.
"""

import math
import struct
import sys
import unicodedata

import numpy as np

VERSION = 1
MAGIC = b"CZLM"
COST_SCALE = 64
MAX_WORD_LEN = 31
BLOCK = 32
UNI_BITS = 6
UNI_STEP = 0.5
MAX_CODE_LEN = 15
ANCHOR_EVERY = 16
QBITS = 4
TOPN = 48

SEC_LEX, SEC_UNI, SEC_BI, SEC_TRI, SEC_TOP = 1, 2, 3, 4, 5

# Lowercase symbols; uppercase is a per-word case class. At most 63 symbols.
_ALPHABET = ("abcdefghijklmnopqrstuvwxyz" "äöüß"
             "àáâãåæçèéêëìíîïñòóôõøùúûýÿœ" "'-")
ALPHABETS = {"de": _ALPHABET, "en": _ALPHABET}
assert len(_ALPHABET) <= 63

_ITU = {c: k for k, letters in
        {"2": "abc", "3": "def", "4": "ghi", "5": "jkl", "6": "mno",
         "7": "pqrs", "8": "tuv", "9": "wxyz"}.items() for c in letters}


def key_of(ch):
    """Phone key (ASCII digit) of a lowercase symbol."""
    if ch in "'-":
        return "1"
    special = {"ß": "s", "æ": "a", "œ": "o", "ø": "o"}
    base = special.get(ch) or unicodedata.normalize("NFD", ch)[0]
    return _ITU[base]


def keyseq(word):
    return "".join(key_of(c) for c in word.lower())


def case_of(word):
    if word.islower():
        return 0
    if word[0].isupper() and word[1:].islower() or len(word) == 1:
        return 1
    return 2


class BitWriter:
    def __init__(self):
        self.buf = bytearray()
        self.acc = 0
        self.nacc = 0
        self.nbits = 0

    def write(self, value, nbits):
        assert 0 <= value < (1 << nbits) or nbits == 0, (value, nbits)
        self.acc = (self.acc << nbits) | value
        self.nacc += nbits
        self.nbits += nbits
        while self.nacc >= 8:
            self.nacc -= 8
            self.buf.append((self.acc >> self.nacc) & 0xFF)
        self.acc &= (1 << self.nacc) - 1

    def rice(self, value, k):
        q = value >> k
        # q ones, a zero, then k low bits.
        while q >= 32:
            self.write(0xFFFFFFFF, 32)
            q -= 32
        self.write(((1 << q) - 1) << 1, q + 1)
        if k:
            self.write(value & ((1 << k) - 1), k)

    def gamma(self, value):
        """Elias gamma code of value >= 1."""
        n = value.bit_length()
        self.write(0, n - 1)
        self.write(value, n)

    def bytes(self):
        out = bytearray(self.buf)
        if self.nacc:
            out.append((self.acc << (8 - self.nacc)) & 0xFF)
        return bytes(out)


def huffman_lengths(counts, nsym):
    """Code lengths (<= MAX_CODE_LEN) of a Huffman code for symbols 0..nsym-1."""
    import heapq
    freq = [counts.get(i, 0) for i in range(nsym)]
    bias = 0
    while True:
        heap = [(f + bias, i, (i,)) for i, f in enumerate(freq) if f > 0]
        lengths = [0] * nsym
        if len(heap) == 1:
            lengths[heap[0][1]] = 1
            return lengths
        heapq.heapify(heap)
        n = nsym
        while len(heap) > 1:
            fa, _, a = heapq.heappop(heap)
            fb, _, b = heapq.heappop(heap)
            for i in a + b:
                lengths[i] += 1
            heapq.heappush(heap, (fa + fb, n, a + b))
            n += 1
        if max(lengths) <= MAX_CODE_LEN:
            return lengths
        bias = bias * 2 + 1  # flatten the distribution until it fits


def canonical_codes(lengths):
    codes = [None] * len(lengths)
    code = 0
    for length in range(1, MAX_CODE_LEN + 1):
        for sym, l in enumerate(lengths):
            if l == length:
                codes[sym] = (code, length)
                code += 1
        code <<= 1
    return codes


def best_rice_k(gaps):
    gaps = np.asarray(gaps, dtype=np.int64)
    if len(gaps) == 0:
        return 0
    best = None
    for k in range(0, 24):
        bits = int((gaps >> k).sum()) + (k + 1) * len(gaps)
        if best is None or bits < best[0]:
            best = (bits, k)
    return best[1]


def lloyd(values, levels):
    """1-D k-means codebook (sorted centres) for the given values."""
    v = np.sort(np.asarray(values, dtype=np.float64))
    if len(v) == 0:
        return np.zeros(levels)
    centres = np.quantile(v, (np.arange(levels) + 0.5) / levels)
    for _ in range(30):
        mids = (centres[1:] + centres[:-1]) / 2
        idx = np.searchsorted(mids, v)
        for i in range(levels):
            sel = v[idx == i]
            if len(sel):
                centres[i] = sel.mean()
        centres.sort()
    return centres


def quantise(values, centres):
    mids = (centres[1:] + centres[:-1]) / 2
    return np.searchsorted(mids, np.asarray(values))


class ModelBuilder:
    def __init__(self, lang, vocab):
        self.lang = lang
        self.alphabet = ALPHABETS[lang]
        self.sym = {c: i for i, c in enumerate(self.alphabet)}
        order = sorted(vocab, key=lambda w: (keyseq(w), w.lower(), case_of(w)))
        self.words = order
        self.V = len(order)
        self.id_of = {w: i for i, w in enumerate(order)}
        self.stats = {}

    # Counting ---------------------------------------------------------------

    def count(self, a):
        V = self.V
        B = V + 1
        words = a[(a >= 0) & (a < V)]
        self.uni = np.bincount(words, minlength=V).astype(np.int64)
        self.N = int(self.uni.sum())

        x, y = a[:-1], a[1:]
        ok = (x >= 0) & (y >= 0) & (y < V)
        codes, counts = np.unique(x[ok] * B + y[ok], return_counts=True)
        self.bi_ctx, self.bi_w, self.bi_c = codes // B, codes % B, counts

        x, y, z = a[:-2], a[1:-1], a[2:]
        ok = (x >= 0) & (y >= 0) & (y < V) & (z >= 0) & (z < V)
        codes, counts = np.unique((x[ok] * B + y[ok]) * B + z[ok], return_counts=True)
        self.tri_u = codes // (B * B)
        self.tri_v = (codes // B) % B
        self.tri_w = codes % B
        self.tri_c = counts
        print(f"  {len(self.bi_c)} bigram types, {len(self.tri_c)} trigram types",
              file=sys.stderr)

    @staticmethod
    def discount(counts):
        n1 = int((counts == 1).sum())
        n2 = int((counts == 2).sum())
        return n1 / (n1 + 2 * n2) if n1 + n2 else 0.5

    def estimate(self):
        V = self.V
        self.p_uni = (self.uni + 0.5) / (self.N + 0.5 * V)
        self.uni_cost = -np.log2(self.p_uni)

        # Bigram: absolute discounting over context totals.
        self.D2 = self.discount(self.bi_c)
        ctx_tot = np.bincount(self.bi_ctx, weights=self.bi_c, minlength=V + 1)
        self.bi_p = (self.bi_c - self.D2) / ctx_tot[self.bi_ctx]
        ratio = np.log2(np.maximum(self.bi_p, 1e-12)) - np.log2(self.p_uni[self.bi_w])
        # Pruning score: weighted change of log probability against the unigram.
        self.bi_score = self.bi_c * np.abs(ratio)
        self.bi_score[self.bi_c < 2] = 0

        self.D3 = self.discount(self.tri_c)
        B = V + 1
        tctx = self.tri_u * B + self.tri_v
        uctx, inv = np.unique(tctx, return_inverse=True)
        tot = np.bincount(inv, weights=self.tri_c)
        self.tri_p = (self.tri_c - self.D3) / tot[inv]

    # Selection under a budget -------------------------------------------------

    def select_bigrams(self, k):
        order = np.argsort(-self.bi_score, kind="stable")
        keep = order[:k]
        keep = keep[self.bi_score[keep] > 0]
        keep = keep[np.lexsort((self.bi_w[keep], self.bi_ctx[keep]))]
        ctx, w, p = self.bi_ctx[keep], self.bi_w[keep], self.bi_p[keep]
        # Backoff weights per context.
        V = self.V
        sum_p = np.bincount(ctx, weights=p, minlength=V + 1)
        sum_u = np.bincount(ctx, weights=self.p_uni[w], minlength=V + 1)
        alpha = np.ones(V + 1)
        has = sum_p > 0
        alpha[has] = np.maximum(1 - sum_p[has], 1e-6) / np.maximum(1 - sum_u[has], 1e-6)
        return ctx, w, -np.log2(p), alpha

    def bigram_cost_bo(self, ctx, w, bi):
        """Cost of P(w | ctx) under the pruned bigram model."""
        bctx, bw, bcost, alpha = bi
        V = self.V
        B = V + 1
        codes = bctx * B + bw
        q = ctx * B + w
        pos = np.searchsorted(codes, q)
        pos_c = np.minimum(pos, len(codes) - 1)
        found = (pos < len(codes)) & (codes[pos_c] == q)
        cost = -np.log2(alpha[ctx]) + self.uni_cost[w]
        cost[found] = bcost[pos_c[found]]
        return cost

    def prepare_trigrams(self, bi):
        """Candidate trigrams and their pruning scores for a chosen bigram set."""
        bctx, bw, bcost, alpha = bi
        V = self.V
        B = V + 1
        # Context (u, v) must be a stored bigram; its key is the bigram index.
        codes = bctx * B + bw
        q = self.tri_u * B + self.tri_v
        pos = np.searchsorted(codes, q)
        pos_c = np.minimum(pos, max(len(codes) - 1, 0))
        ok = (pos < len(codes)) & (codes[pos_c] == q) & (self.tri_c >= 2)
        idx = np.nonzero(ok)[0]
        key = pos_c[idx]
        w = self.tri_w[idx]
        p3 = self.tri_p[idx]
        lower = self.bigram_cost_bo(self.tri_v[idx], w, bi)
        score = self.tri_c[idx] * np.abs(-np.log2(np.maximum(p3, 1e-12)) - lower)
        self._tri = (key, w, p3, lower, np.argsort(-score, kind="stable"), len(codes))

    def select_trigrams(self, k):
        key, w, p3, lower, ranked, nkeys = self._tri
        order = ranked[:k]
        order = order[np.lexsort((w[order], key[order]))]
        key, w, p3, lower = key[order], w[order], p3[order], lower[order]
        sum_p = np.bincount(key, weights=p3, minlength=nkeys)
        sum_l = np.bincount(key, weights=np.exp2(-lower), minlength=nkeys)
        alpha = np.ones(nkeys)
        has = sum_p > 0
        alpha[has] = np.maximum(1 - sum_p[has], 1e-6) / np.maximum(1 - sum_l[has], 1e-6)
        return key, w, -np.log2(p3), alpha

    # Encoding -----------------------------------------------------------------

    def encode_lexicon(self):
        eow = len(self.alphabet)
        # Pass 1: symbol statistics.
        items = []
        prev = ""
        for i, w in enumerate(self.words):
            lw = w.lower()
            pre = 0
            if i % BLOCK:
                while pre < min(len(lw), len(prev)) and lw[pre] == prev[pre]:
                    pre += 1
            items.append((i % BLOCK == 0, pre, [self.sym[c] for c in lw[pre:]] + [eow],
                          case_of(w)))
            prev = lw
        from collections import Counter
        pre_c = Counter(p for first, p, _, _ in items if not first)
        sym_c = Counter(s for _, _, syms, _ in items for s in syms)
        case_c = Counter(c for _, _, _, c in items)
        pre_len = huffman_lengths(pre_c, 32)
        sym_len = huffman_lengths(sym_c, 64)
        case_len = huffman_lengths(case_c, 4)
        pre_code, sym_code, case_code = (canonical_codes(pre_len), canonical_codes(sym_len),
                                         canonical_codes(case_len))

        # Pass 2: encode.
        bw = BitWriter()
        offsets = []
        for first, pre, syms, case in items:
            if first:
                offsets.append(bw.nbits)
            else:
                bw.write(*pre_code[pre])
            for s in syms:
                bw.write(*sym_code[s])
            bw.write(*case_code[case])
        head = struct.pack("<I", len(offsets)) + bytes(pre_len) + bytes(sym_len) + bytes(case_len)
        head += b"".join(struct.pack("<I", o) for o in offsets)
        return head + bw.bytes()

    def encode_unigrams(self):
        q = np.clip(np.round(self.uni_cost / UNI_STEP), 0, (1 << UNI_BITS) - 1).astype(int)
        bw = BitWriter()
        for v in q:
            bw.write(int(v), UNI_BITS)
        return bw.bytes()

    def encode_top(self):
        top = np.argsort(-self.uni, kind="stable")[:TOPN]
        return struct.pack("<H", len(top)) + b"".join(struct.pack("<H", int(t)) for t in top)

    @staticmethod
    def encode_postings(keys, ids, costs, alpha, nkeys):
        """keys/ids sorted; costs in bits; alpha: backoff weight per key."""
        out = bytearray()
        if len(keys) == 0:
            return struct.pack("<IIBBBB", 0, 0, 0, 0, ANCHOR_EVERY, QBITS) + \
                bytes(2 * 2 * (1 << QBITS)) + struct.pack("<II", 0, 0)
        levels = 1 << QBITS
        cost_cb = lloyd(costs, levels)
        ukeys, starts = np.unique(keys, return_index=True)
        alpha_cost = -np.log2(alpha[ukeys])
        alpha_cb = lloyd(alpha_cost, levels)
        cq = quantise(costs, cost_cb)
        aq = quantise(alpha_cost, alpha_cb)

        key_gaps = np.diff(np.concatenate(([-1], ukeys))) - 1
        ends = np.append(starts[1:], len(keys))
        id_gaps = np.empty(len(ids), dtype=np.int64)
        for s, e in zip(starts, ends):
            id_gaps[s:e] = np.diff(np.concatenate(([-1], ids[s:e]))) - 1
        kk = best_rice_k(key_gaps)
        ki = best_rice_k(id_gaps)

        bw = BitWriter()
        anchors = []
        for r, (key, s, e) in enumerate(zip(ukeys, starts, ends)):
            if r % ANCHOR_EVERY == 0:
                anchors.append((int(key), bw.nbits, int(s)))
            else:
                bw.rice(int(key_gaps[r]), kk)
            bw.write(int(aq[r]), QBITS)
            bw.gamma(int(e - s))
            for j in range(s, e):
                bw.rice(int(id_gaps[j]), ki)
                bw.write(int(cq[j]), QBITS)

        out += struct.pack("<IIBBBB", len(ukeys), len(keys), kk, ki, ANCHOR_EVERY, QBITS)
        out += b"".join(struct.pack("<h", int(round(c * COST_SCALE))) for c in cost_cb)
        out += b"".join(struct.pack("<h", int(round(c * COST_SCALE))) for c in alpha_cb)
        out += struct.pack("<I", len(anchors))
        out += b"".join(struct.pack("<III", *a) for a in anchors)
        data = bw.bytes()
        out += struct.pack("<I", bw.nbits) + data
        return bytes(out)

    def header(self, sections):
        alph = [ord(c) for c in self.alphabet] + [0] * (64 - len(self.alphabet))
        keys = [ord(key_of(c)) for c in self.alphabet] + [0] * (64 - len(self.alphabet))
        head = MAGIC + struct.pack("<HH4sIBBBB", VERSION, len(sections),
                                   self.lang.encode(), self.V, len(self.alphabet),
                                   BLOCK, MAX_WORD_LEN, 0)
        head += struct.pack("<64H", *alph) + bytes(keys)
        dir_size = 12 * len(sections)
        off = len(head) + dir_size
        directory = b""
        body = b""
        for sid, data in sections:
            pad = (-off) % 4
            body += bytes(pad)
            off += pad
            directory += struct.pack("<III", sid, off, len(data))
            body += data
            off += len(data)
        return head + directory + body

    def encode(self, budget, tri_share):
        lex = self.encode_lexicon()
        uni = self.encode_unigrams()
        top = self.encode_top()
        fixed = len(lex) + len(uni) + len(top) + 400
        remaining = budget - fixed
        if remaining <= 0:
            sys.exit(f"budget too small: lexicon and unigrams need {fixed} bytes")
        bi_budget = int(remaining * (1 - tri_share))

        def fit(build, limit, maxk):
            """Largest k (within 1 %) whose encoding fits into limit bytes."""
            best = build(0)
            lo, hi = 0, maxk + 1
            while hi - lo > max(50, lo // 100):
                mid = (lo + hi) // 2
                sel, data = build(mid)
                if len(data) <= limit:
                    best = (sel, data)
                    lo = mid
                else:
                    hi = mid
            return best

        V = self.V

        def build_bi(k):
            sel = self.select_bigrams(k)
            ctx, w, cost, alpha = sel
            return sel, self.encode_postings(ctx, w, cost, alpha, V + 1)

        bi, bi_data = fit(build_bi, bi_budget, int((self.bi_score > 0).sum()))
        tri_budget = budget - fixed - len(bi_data)

        self.prepare_trigrams(bi)

        def build_tri(k):
            sel = self.select_trigrams(k)
            key, w, cost, alpha = sel
            return sel, self.encode_postings(key, w, cost, alpha, len(bi[0]))

        tri, tri_data = fit(build_tri, tri_budget, len(self._tri[0]))
        sections = [(SEC_LEX, lex), (SEC_UNI, uni), (SEC_BI, bi_data), (SEC_TRI, tri_data),
                    (SEC_TOP, top)]
        self.stats = {
            "lexicon": len(lex), "unigrams": len(uni), "bigrams": len(bi_data),
            "trigrams": len(tri_data), "n_bigrams": len(bi[0]), "n_trigrams": len(tri[0]),
            "D2": self.D2, "D3": self.D3,
        }
        return self.header(sections)

    def report(self):
        s = self.stats
        print(f"  lexicon {s['lexicon']} B ({self.V} words, "
              f"{8 * s['lexicon'] / self.V:.1f} bits/word)", file=sys.stderr)
        print(f"  unigrams {s['unigrams']} B", file=sys.stderr)
        print(f"  bigrams {s['bigrams']} B ({s['n_bigrams']} entries, "
              f"{8 * s['bigrams'] / max(s['n_bigrams'], 1):.1f} bits/entry), "
              f"D={s['D2']:.2f}", file=sys.stderr)
        print(f"  trigrams {s['trigrams']} B ({s['n_trigrams']} entries, "
              f"{8 * s['trigrams'] / max(s['n_trigrams'], 1):.1f} bits/entry), "
              f"D={s['D3']:.2f}", file=sys.stderr)
