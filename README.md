# merope

Out-of-core CSV engine. Infers dialect, physical types and column meanings from
a file it has never seen, then answers natural-language questions about it in
parallel. C++20, Windows, zero third-party dependencies.

```
merope profile data\sales.csv                  # what it worked out, writes nothing
merope schema  data\sales.csv --confirm        # cache the schema beside the file
merope query   data\sales.csv "Show total amount by country in 2025"
merope serve   --data data                     # same pipeline, browser front end
```

## Build

MSVC v143 (VS 2022), x64, C++20. Windows-only: Winsock serves the UI, WinHTTP
calls the model, both ship with the OS.

```
git clone https://github.com/neear0/merope.git
cd merope
msbuild merope.vcxproj /p:Configuration=Release /p:Platform=x64
x64\Release\merope.exe selftest        # 209 checks, no network, no dataset
```

- Configurations: `Debug`, `Release`, `Sanitize` (`/fsanitize=address`).
- Builds at `/W4 /WX`.
- No CMake, no Makefile, no package manifest. JSON parser, expression parser,
  thread pool, HTTP server and HTTPS client are all in `merope/src`.

## Pipeline

```
question
  → planner     model emits a declarative JSON plan, never code
  → validator   columns exist, types fit, ops allowed, result bounded
  → compiler    projection pushdown, slot assignment, expression typing
  → execute     one partition per worker, one chunk at a time, local partials
  → reduce      partials merged once
  → result + execution report
```

A rejected plan never reaches the engine. `query` prints the logical plan, the
validator notes and the physical plan before any result.

## Schema inference

| Stage | Method |
| --- | --- |
| Encoding | BOM, then UTF-8 validation, then CP1250 vs ISO-8859-2 by scoring high bytes against letters real Central European text uses. UTF-16 is detected and refused — it cannot be streamed byte-wise. |
| Delimiter | `, ; \t \|` scored over ≤60 lines by field-count consistency. |
| Quoting | Re-split with quotes honoured; records whether any quoted field contains a newline. |
| Separators | Decimal/thousands voted on by digit-group positions. Ambiguous shapes (`1.234`) abstain. |
| Sample | ≤64 MiB: reservoir over a full scan. Larger: 200 head rows + 64 random offsets snapped to line boundaries, 256 rows each. Target 10,000 rows, fixed seed. |
| Profile | Two passes — nulls/cardinality/lengths/examples, then typed extremes once the type is known. |
| Inference | Measured shape × header label. Disagreement returns `UNKNOWN` with the conflict stated, not a guess. |

Only a full scan gives exact counts and extremes. Everything else is labelled
`(sample estimate)` wherever shown. `--confirm` writes
`<name>.csv.merope-schema.json` beside the dataset; `--no-cache` ignores it.

## Types

| Type | Storage |
| --- | --- |
| `int64` | `std::int64_t` |
| `float64` | `double` |
| `boolean` | `int64`, 0/1 |
| `date` | `int64`, days since 1970-01-01 |
| `datetime` | `int64`, seconds since epoch, UTC |
| `decimal` | `int64` scaled by 10⁴ |
| `utf8` | `std::string` |
| `categorical` | `std::string`, low cardinality |
| `unknown` | — |

- **Money is `decimal`.** `SUM` over 4M rows is exact to the cent. More fraction
  digits than the scale is rejected, not rounded.
- **`AVG` carries `(sum, count)`** — never a mean of means. Asserted
  bit-identical across 1, 13 and 16 partitions.
- **Integer sums are range-checked.** Overflow is reported unreliable, not wrapped.
- **Division never truncates.** Always float; `x / 0` is NULL, not infinity.
- **Nulls are two-tier.** `null n/a nan \n` are missing anywhere. `na nil none
  - -- ?` are missing only in non-text columns — `NA` is Namibia, `None` is a
  status, and treating them as missing deletes whole groups from a `GROUP BY`.

## Engine

| | |
| --- | --- |
| Chunk | 32,768 rows. `column_block_t` = null-flag vector + exactly one of `ints` / `reals` / `texts`. |
| Memory | One chunk per worker. Read, evaluate, reduce, drop. Nothing accumulates per row. |
| Partitions | Equal byte ranges over `[0, file_size)`; each reader snaps forward to the next newline. Count capped at `file_size / 4 MiB`. |
| Disabled when | The sniffer saw a newline inside a quoted field — from an arbitrary offset it is indistinguishable from a record terminator. |
| Bad rows | `--policy skip` (count), `quarantine` (retain capped sample), `fail` (throw on first). |

## Model boundary

Gemini, OpenAI-chat and Anthropic wire formats. `--api-base` covers anything
speaking the OpenAI shape (Ollama, LM Studio, vLLM, OpenRouter).

```
merope models                                                    # ask the provider
merope query data\sales.csv "..." --provider anthropic --model claude-sonnet-5
merope query data\sales.csv "..." --provider ollama --api-base http://127.0.0.1:11434/v1
```

- **Leaves the process:** the profile (types, null/distinct counts, a few example
  values) and the question. Never a row. `--show-prompt` prints it verbatim.
- **Comes back untrusted:** unknown column names dropped and counted, confidence
  clamped to the heuristic floor, unanswered columns keep their heuristic
  proposal, and the plan still faces the validator.
- **Prompt-injection stance:** a header or value reading like an instruction is
  data; the model is told to report it, not obey it.
- **Mock is the default.** Heuristics + conventional-name table + keyword
  parsing. `bench` uses it exclusively — no network call in a timing loop.

Resolution order per run: CLI → env (`MEROPE_AI_PROVIDER`, `MEROPE_AI_MODEL`,
`MEROPE_AI_BASE`) → `merope.ai.json` → mock. Every command prints what it chose.

Key sources: `merope.ai.json` (copy `merope.ai.example.json`) or `MEROPE_API_KEY`
/ `GEMINI_API_KEY` / `OPENAI_API_KEY` / `ANTHROPIC_API_KEY` / `GOOGLE_API_KEY`.
Never a flag — command lines land in shell history. Never returned by an
endpoint, written to the sidecar, or printed in full.

## Validator

Grammar: comparisons, `AND`/`OR`/`NOT`, `IN`, `BETWEEN`, `IS NULL`, `+ - * /`,
and `year month day hour minute lower upper length abs`. No arbitrary code path.

- Columns must exist; operand types must be comparable; aggregates must suit
  their column type.
- Literals are coerced to the subject type where exact, rejected where not.
- No limit → one applied, with a warning. Several → smallest wins.
- Recursion is depth-bounded: a nested predicate would otherwise overflow the
  stack, and that is not a catchable exception.

## Commands

| Command | Does |
| --- | --- |
| `profile <csv>` | Sniff, sample, profile, infer. Writes nothing. |
| `schema <csv> [--confirm]` | Show schema; `--confirm` caches it. |
| `query <csv> "<question>"` | Plan, validate, execute, report. |
| `plan <csv> <plan.json>` | Run a plan directly, no model. |
| `gen <csv>` | Synthetic dataset with known exact answers. |
| `serve` | Web UI on loopback. |
| `bench` | Run benchmark suites, write CSV. |
| `models` | Ask the provider what this key may use. |
| `selftest` | 209 checks. |

`--workers N` `--partitions N` `--chunk-rows N` `--sample-rows N` `--seed S`
`--policy skip|quarantine|fail` `--baseline` `--no-cache` `--no-ai`
`--show-prompt` `--port N` `--data DIR` `--no-open` `--no-kill` `--provider P`
`--model M` `--api-base URL` `--ai-config PATH` `--ai-timeout N`

`gen` prints the exact totals it wrote, so answers can be checked against the
data rather than against another run of the engine.

## HTTP API

| Endpoint | Purpose |
| --- | --- |
| `GET /api/datasets` | List CSVs under the served directory. |
| `GET /api/session` | Capabilities, active model, shutdown token. |
| `GET /api/models` | Models this key may use. |
| `POST /api/inspect` | Sniff, sample, profile, infer. |
| `POST /api/confirm` | Save the accepted schema. |
| `POST /api/query` | Question → plan → validate → execute. |
| `POST /api/plan` | Run a plan directly. |
| `POST /api/generate` | Write a synthetic dataset. |
| `POST /api/shutdown` | Stop the server. Needs `X-Merope-Token`. |

**Security.** The API opens files by path, so it is not a network service:

- binds `127.0.0.1` only, never `0.0.0.0`;
- dataset paths canonicalised and confined to `--data`, compared component by
  component — neither `..` nor a name-prefix sibling gets through;
- non-local `Host` refused, which blunts DNS rebinding;
- `/api/shutdown` also needs the session token: a cross-origin page can post
  blind, and stopping a process needs no reply to be useful. No CORS headers, so
  that page can neither read the token nor set its header;
- numeric fields clamped before the cast to `size_t`, so a negative value cannot
  wrap into an unbounded allocation.

First four are covered by `selftest`. Outbound TLS, proxy and cert store are
WinHTTP's; the key travels in a header, not a URL a proxy would log.

## Web UI

`serve` opens `http://127.0.0.1:7433/`: pick a dataset, check what was inferred,
ask, read. Page is embedded in the executable — no asset directory, nothing
fetched from the network including fonts. Authored at `merope/src/web/ui.html`;
`tools\embed_ui.ps1` regenerates the embedded copy (chunked only because MSVC
caps a string literal at 16 KB).

- Each column shown beside the values it was judged on, with type, confidence
  and reason. `UNKNOWN` is marked, not hidden.
- The chain halts visibly where the validator refuses, downstream stages greyed
  as *not reached*, with the model's raw JSON and the reason.
- Kill button posts to `/api/shutdown`, then polls `/api/session` until the
  fetch fails at the network layer — the reply is only intent, the silence is
  the proof. `--no-kill` removes the endpoint.
- Results are linkable: `#sales.csv?q=Show total amount by country in 2025`.

## Benchmarks

```
merope bench
merope bench --suite threads --repeats 5
merope bench --suite scaling --sizes 1,5,10,25 --keep
```

Fixed plan, not a question, so no network call sits in the timing loop:
`year(timestamp)` computed, filtered to 2025, grouped by country, summed over
amount, sorted. Median of `--repeats`. Each run writes a timestamped directory
under `bench/` with per-suite CSVs and a `manifest.txt` recording seed,
provider, compiler, CPU, cores and memory.

4,000,000 rows, 171 MB, `sum(amount) by country` filtered to 2025:

| Workers | Time | Throughput | Peak memory |
| ---: | ---: | ---: | ---: |
| 1 | 3.90 s | 44 MB/s | 7.4 MB |
| 2 | 2.10 s | 82 MB/s | 11.4 MB |
| 4 | 1.33 s | 128 MB/s | 19.6 MB |
| 8 | 0.75 s | 230 MB/s | 34.5 MB |
| 16 | 0.69 s | 247 MB/s | 63.0 MB |

5.6× on 16 workers. Peak memory is sampled on a background thread and the
process-wide counter only rises, so it bounds the flat-memory claim rather than
demonstrating it exactly.

## Layout

```
merope/src/
  core/       type system, value parsing, JSON
  dataset/    encoding, sniffer, record reader, sampler, generator
  schema/     profiler, heuristics, unified schema and sidecar
  plan/       expression parser, plan model, validator, compiler
  engine/     columnar chunks, evaluator, processing engine, report
  parallel/   thread pool, partitioner
  ai/         provider interface, mock, HTTPS client, remote adapter
  app/        pipeline shared by both front ends
  web/        HTTP server, REST API, UI
merope/tests/ self_test.cpp
tools/        embed_ui.ps1
```

`snake_case` names, `c_` classes, `_t` structs, `k_` constants. RAII, no naked
`new`/`delete`. No column is addressed by hardcoded name or constant index —
everything resolves through `physical_index` on the confirmed schema.

## License

MIT. See [LICENSE](LICENSE).
