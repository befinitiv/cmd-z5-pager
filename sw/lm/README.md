Language models for predictive text, read by `lib/textinput`. Built with `tools/lmbuild/build_models.sh`; see the predictive text section in `../README.md` for the format and the evaluation.

| File | Language | Words | Bigrams | Trigrams | Size |
|---|---|---|---|---|---|
| `de.lm` | German | 20 000 | 28 483 | 9 763 | 176 KB |
| `en.lm` | English | 20 000 | 29 838 | 10 086 | 176 KB |

# Sources and licences

The models are derived from these corpora and inherit their attribution requirements:

* **Tatoeba** (https://tatoeba.org), German and English sentences, licensed under CC BY 2.0 FR. Sentences containing the example names Tom, Mary and Maria were downsampled to 5 %.
* **Leipzig Corpora Collection**, `deu-de_web-public_2019_300K` and `eng-com_web-public_2018_300K`, licensed under CC BY. Please cite: D. Goldhahn, T. Eckart, U. Quasthoff: *Building Large Monolingual Dictionaries at the Leipzig Corpora Collection: From 100 to 200 Languages.* Proceedings of LREC 2012.
