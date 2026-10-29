# Run report

`anyps5-report.json` is a structured summary of one run. [`run-report.schema.json`](../run-report.schema.json) is the single source of truth for its shape; the sample reports in [`run-report-fixtures/`](../run-report-fixtures/) all validate against it. This is version 1. Adding a field keeps version 1; renaming or removing a field, or changing what one means, is version 2.

The file stays on your machine. Nothing is uploaded unless you share it.

## Switching it off

The report is written by default. Turn it off with the launch flag `--aps5-report=off` or the environment variable `APS5_REPORT=0`. The flag wins over the environment variable, and the environment variable wins over the default (on). There is no ini setting. The flag needs a game converted with a new enough relinker; the environment variable also works with older conversions. The runtime reads the flag from the command line and does not remove it, so the game can see `--aps5-report=...` in its arguments. When the game relaunches itself, the choice is passed on through `APS5_REPORT`.

## Where it goes

`anyps5-report.json` is written next to the game's executable. If that folder can't be written, it goes to the AnyPS5 data folder under `reports/<run id>/` and the runtime prints where. `run.id` is that same id: 32 lowercase hex characters, random per run.

While a run is in progress, `anyps5-report.<run id>.ckpt` and `.lock` files sit in the same folder; a run killed from outside can leave them there until the next start recovers it. A checkpoint is refreshed every five seconds, so a recovered report's `seconds` can be up to five seconds short.

If a run ends without writing its report (power loss, forced kill, a crash the runtime can't handle), the next start writes a report for it, marked `recovered` and `incomplete`, with `exit: "killed"`. That means the run ended uncleanly for an unknown cause. It is not proof that anything killed the process.

## What `anyps5.modified` means

`false` means the build's git tree had no change to tracked files, no untracked files that git does not ignore, and no change inside a submodule. `true` means any of those. `null` means the build did not come from a git checkout. A mixed set of libraries from different builds is not detected.

## Unknown is null

A value the runtime cannot know is `null`, never a guess. This applies to `anyps5.*`, every `game.*` and `system.*` field, the whole `system.gpu` object (null if the run stopped before the GPU was set up), `run.exitCode`, `stop.source`, and instruction details. Read null as "unknown", not as false or zero.

`game.titleId` is copied only when it matches `PPSA` or `CUSA` style (four capital letters and five digits); otherwise it is null.

## Run

| Field | Meaning |
|---|---|
| `stage` | The furthest milestone the runtime observed. `unknown` is the starting value. The runtime never claims `converts`: conversion happens in the converter. |
| `loads` | The runtime reached the game's entry point. This does not prove every import resolved, because imports can load later. |
| `starts` | Game code was entered on its thread. |
| `first-draw` | A real game draw or dispatch was submitted to the GPU. |
| `presents` | The GPU accepted a game frame for display. This does not prove the frame was visible or not black. |
| `milestones` | Seconds since start. Milestones not reached are left out. |
| `exit` | `clean`, `crash`, `abort` or `killed`. A non-zero exit code from the game is not automatically a crash. |
| `exitCode` | The process exit code when known, otherwise null. |
| `averageFps`, `fpsWindowSeconds` | Frames per second measured from the first presented frame to the last, and the length of that window. Both are null with fewer than two presented frames. |
| `recovered`, `incomplete` | Both true on a report written at the next start for a run that ended uncleanly. |

## Stop

`stop.kind` is `none`, `missing-function`, `unsupported-shader`, `unsupported-format`, `crash`, `assert` or `other`. `none` goes with a clean exit and only with it.

`stop.reason` is the end path, separate from the kind: `kind` says what stopped the run, `reason` says how the run ended.

| Reason | Meaning |
|---|---|
| `guest-exit` | The game called `exit`, `_exit`, `_Exit` or asked the system to quit or relaunch. |
| `quit-requested` | The window system asked the game to quit: closing the window, or a terminal interrupt that becomes the same event. The two cannot be told apart. |
| `runtime-stop` | The runtime ended the run on an error it reported itself. The kind says which: `missing-function`, `unsupported-shader`, `unsupported-format` or `other`. |
| `crash` | The run ended through the crash or abort handler. The kind is `crash`, or `assert` when the assertion helper ran, or another kind the runtime recorded before it died. |
| `unclean-unknown` | Recovered report: the run ended without a report, for an unknown reason. |
| `other` | An end path the runtime did not classify. |

The pairs the schema enforces: `kind: none`, a clean exit and a reason of `guest-exit` or `quit-requested` go together, and only together. `crash` and `assert` kinds have reason `crash`. `runtime-stop` has one of the four kinds listed above. `unclean-unknown` goes with a recovered report, kind `other`, `exit: killed` and no exit code.

`stop.message` is built from a fixed template, never a raw error string, and is at most 300 characters. `stop.source` is a source-tree path with a line number, or null when unknown (native crashes, for example).

## Which milestones a system records

| System | `loaded`, `started` | `firstDraw`, `firstPresent` | `stage`, `framesPresented`, frame rate |
|---|---|---|---|
| Linux | never recorded | recorded | recorded |
| Windows | never recorded | recorded | recorded |
| macOS | recorded | recorded | recorded |

`coverage.milestones` is true on every system; it means the stage, the milestones the system records and the frame counts were collected. Read a missing `loaded` or `started` on Linux or Windows as "this system does not record it", never as "not reached": a report with `stage: first-draw` and no `loaded` is a run that drew. Only a stage lower than the one a milestone belongs to says it was not reached.

## What the runtime counts

- `milestones.firstDraw` is set when a batch that carries a recorded guest draw or dispatch has been accepted by the GPU queue. A guest draw counts in either kind of batch the runtime submits; a guest compute dispatch counts only when it is recorded into the runtime's main batch (a dispatch that is submitted and waited for on its own does not set the milestone). `firstPresent` and `framesPresented` count guest display buffers that the swapchain queue accepted (a suboptimal one too); internal clears, test pixels and rejected presents are not counted. `loaded` and `started` come from the entry bridge of the converted executable; only the macOS bridge reports them so far, so on other systems those two are left out.
- `missing.functions` counts calls to a function the runtime does not implement. The library is the folder the stub is compiled in; a call from older code that does not say where it is made has a null library. The name is the exported name without the runtime's suffix; a name that does not fit the schema is not listed and is counted in `truncated.functions`.
- `shaders.compiled` and `failed` count compilations where they happen (once per variant); reuse of a variant, in memory or from the disk cache, counts as a cache hit.
- `shaders.unsupported` lists instructions the recompiler could not translate, by encoding family and numeric opcode (the name is null). It also lists instructions met in shaders that were prepared and never used, so an entry does not by itself mean the run stopped on it; the stop says that.
- `shaders.imageFormats` counts guest format lookups, internal ones included, with the use shown as `other`, plus the formats the runtime rejected. It measures how often a format was looked up, not how many images used it. `shaders.descriptors` counts the bindings of shaders as they were prepared for a draw; the count measures use, not distinct shaders.
- `stop` names a typed failure only when it ended the run: a shader instruction the recompiler could not translate and the driver reported as fatal, or an unimplemented function or unsupported image format whose error message is the one that ended the run. A failure the game caught and went on from never names a later stop; any other abort stays `other`.
- `truncated` counts entries (a function, an instruction, a format, a binding kind) that were dropped, once each however many calls they made. When a hook had to give up an observation because the collector was busy, the matching count is at least 1 and the report is marked `incomplete`.
- Nothing in this list is collected, counted or scanned when the report is off.

## Lists, coverage and truncation

- `missing.functions[]`: `library` can be null. `calls` counts calls that reached the missing function. The first one usually ends the run, so 1 is common; this is not a list of everything unimplemented.
- `shaders.compiled` and `failed` count real compilations; cache hits are not compilations (`cacheHits` is optional). `unsupported[]` has `encoding`, `opcode` (name) and `modifiers`, each null when the failure can't be pinned to one guest instruction. An unknown numeric opcode keeps its number in `opcodeValue` with a null `opcode`.
- `shaders.imageFormats[]` and `descriptors[]` have `status` (`accepted`, `rejected` or `unknown`), `usage` and `count`. `count` counts checks, not pixels. Accepted does not prove every use works.
  - Image format `usage`: `sampled`, `storage`, `color-target`, `depth-target`, `transfer`, `other`.
  - Descriptor `usage` is the access: `read`, `write`, `atomic`, `other`.
- `coverage` says which sections the runtime collected for this run. A list can be empty because nothing happened or because this build doesn't collect it. With a section's coverage off, don't read its empty list as "nothing missing".
- `truncated` counts the entries dropped from each capped list, so a capped list never looks complete.

Lists are sorted by count, highest first. They hold at most 200 entries (`missing.functions`, `shaders.unsupported`) or 100 (`imageFormats`, `descriptors`); strings are at most 300 characters. The whole file is at most 65,536 bytes of UTF-8. When it would be larger, the runtime drops the lowest-ranked list entries first, then optional descriptions, and never the build, game or stop details.

## Privacy

Only known, structured values are copied. Free-form text the runtime can't vouch for is left out (null). The file holds no usernames, home paths, hostnames, IP or MAC addresses, serial numbers, account IDs, save data or screenshots. Paths are relative to the AnyPS5 source tree.
