# Fuzzing

Coverage-guided fuzzers (libFuzzer) for the two places where atophttpd turns
untrusted bytes into data structures:

| Harness            | Target                                                        |
|--------------------|---------------------------------------------------------------|
| `fuzz_http_args`   | the HTTP query-string parser (`http_args.c`), incl. the in-place `%2C` -> `,` rewrite |
| `fuzz_rawlog`      | the atop binary raw-log parser (`rawlog.c`): header, per-sample records and zlib-compressed `sstat`/`tstat` blobs |

Both are built with clang and compiled with AddressSanitizer + UndefinedBehaviorSanitizer,
so any memory error, undefined behaviour, crash or runaway allocation fails the run and
leaves a reproducer in `fuzz/crashes/`.

## Running

```sh
make fuzz                      # both targets, 30s each (also a CI gate)
make fuzz FUZZ_SECONDS=300     # longer session
make fuzz FUZZ=rawlog          # a single target
tests/fuzz.sh rawlog 600       # the script directly: <target> <seconds>
```

`clang` is required (`sudo apt-get install clang`). The script builds the atop
submodule to generate a valid raw-log, which it drops into `fuzz/corpus/rawlog/`
as a seed -- essential, since the raw-log header carries a magic, exact struct
sizes and the running atop version that a blind mutator cannot synthesise.

## Corpus

`fuzz/corpus/<target>/` holds the inputs. Human-written seeds (`seed_*`) are
committed; libFuzzer's own SHA1-named additions and the generated raw-log seed
are git-ignored.

## Reproducing a crash

```sh
./fuzz/fuzz_rawlog fuzz/crashes/crash-<hash>
```
