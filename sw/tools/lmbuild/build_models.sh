#!/bin/sh
# Download the corpora (once) and rebuild lm/de.lm and lm/en.lm, then
# evaluate both on their held-out sentences. Needs python3 with numpy, gcc.
#
# Corpora (about 170 MB) are cached in ${CORPUS_DIR:-~/.cache/cmdz5-corpora}.
set -e

here=$(cd "$(dirname "$0")" && pwd)
out="$here/../../lm"
c=${CORPUS_DIR:-$HOME/.cache/cmdz5-corpora}
mkdir -p "$c" "$here/build"

fetch() {
	[ -f "$c/$2" ] || curl -sSfL -o "$c/$2" "$1"
}
fetch https://downloads.tatoeba.org/exports/per_language/deu/deu_sentences.tsv.bz2 deu_sentences.tsv.bz2
fetch https://downloads.tatoeba.org/exports/per_language/eng/eng_sentences.tsv.bz2 eng_sentences.tsv.bz2
fetch https://downloads.wortschatz-leipzig.de/corpora/deu-de_web-public_2019_300K.tar.gz deu-de_web-public_2019_300K.tar.gz
fetch https://downloads.wortschatz-leipzig.de/corpora/eng-com_web-public_2018_300K.tar.gz eng-com_web-public_2018_300K.tar.gz

for f in deu eng; do
	[ -f "$c/${f}_sentences.tsv" ] || bunzip2 -k "$c/${f}_sentences.tsv.bz2"
done
for n in deu-de_web-public_2019_300K eng-com_web-public_2018_300K; do
	[ -f "$c/$n/$n-sentences.txt" ] || tar xzf "$c/$n.tar.gz" -C "$c" "$n/$n-sentences.txt"
done

cd "$here"
python3 build.py --lang de --heldout-out build/de-heldout.txt -o "$out/de.lm" \
	"$c/deu_sentences.tsv" "$c/deu-de_web-public_2019_300K/deu-de_web-public_2019_300K-sentences.txt"
python3 build.py --lang en --heldout-out build/en-heldout.txt -o "$out/en.lm" \
	"$c/eng_sentences.tsv" "$c/eng-com_web-public_2018_300K/eng-com_web-public_2018_300K-sentences.txt"
python3 evaluate.py --lang de "$out/de.lm" build/de-heldout.txt
python3 evaluate.py --lang en "$out/en.lm" build/en-heldout.txt
