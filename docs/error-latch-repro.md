# Draft upstream issue: one errored block latches its error onto every later g-code line

Ready to file against grblHAL core once reproduced on a second driver (this
report is from the ClearCore/SAME53 port, core `GRBL_VERSION 1.1f`, build
`20260726`, vendored submodule). Not yet filed — filing is the owner's call.

## Symptom

After at least one successfully executed g-code block, a single block that
returns an error makes **every subsequent g-code line answer that same error
code**, indefinitely — across telnet disconnect/reconnect — until any
`$`-command executes. `$G` is enough to clear it. Realtime commands and
`$`-commands are unaffected while latched; `<...>` status reports keep
flowing and `$G` shows clean modal state throughout.

## Reproduction (telnet, port 23; any transport should do)

```
G94          → ok
G64          → error:20      (any rejected code seeds it)
G94          → error:20      ← the same command that just ok'd
G80          → error:20
G1 X1 F100   → error:20      (motion also refused)
$G           → [GC:...] ok
G94          → ok            ← cleared
```

Notes from bisection on the bench:

- The latch **replays the seed's error code**: seeding with a G86 that
  answered `error:28` made every later line answer `error:28`, not 20 —
  which suggests a stored status being re-reported rather than re-parsing.
- Seeding with an error as the FIRST line of a session (no prior ok'd
  block) did NOT latch in one trial; error-after-ok latches reliably.
- Reproduced identically on four builds: N_TOOLS 32 + NGC_EXPRESSIONS_ENABLE,
  each flag alone, and a stock defaults build — so it is not tied to the
  tool table, expressions, or any local plugin.
- Survives stream reconnects (state is in the core/protocol, not a session).
- A power cycle also clears it (trivially).

## Why it goes unnoticed

Most senders poll `$G` or `$#` routinely, clearing the latch within a
report interval; a streamed job halts on its first error anyway. It bites
exactly the workflows that send bare g-code lines back to back — and it
produced a documented false "G28 unsupported" conclusion in this project's
history (an error'd line followed by G28 probes) before being isolated.

## Where to look (not yet root-caused)

The $-command clearing and the code replay point away from `gc_state`
(which `$G` reads without writing) and toward per-line status handling in
`protocol.c` / the stream layer. `gc_state.skip_blocks` was ruled out (it
returns Status_OK, not the seed error).
