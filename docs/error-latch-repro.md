# Resolved: the "error latch" is grblHAL's post-error sync hold (compat level 0)

What looked like a bug — one errored block making every later g-code line
repeat the same error, until `$G` cleared it — is a designed grblHAL
mechanism, root-caused to `protocol.c` (~line 266 in this vendored core):

```c
#if COMPATIBILITY_LEVEL == 0
    else if(gc_state.last_error == Status_OK || gc_state.last_error == Status_GcodeToolChangePending) {
#else
    else {
#endif
        if((gc_state.last_error = gc_execute_block(line)) != Status_OK)
```

At `COMPATIBILITY_LEVEL 0` (this build), a g-code line executes only while
`gc_state.last_error` is clean. After an errored block, every further g-code
line SKIPS execution and falls through to `report.status_message(last_error)`
— which is why the stored code replays verbatim, across reconnects. Three
things reset it, all verified on the bench:

- an **empty line** (`protocol.c`: "Empty line. For syncing purposes.") —
  `G94 ok · G64 error:20 · G94 error:20 · <blank> ok · G94 ok`
- any **`$` command** (its status overwrites `last_error`)
- a reset/power cycle.

The purpose is stream-abort integrity: a sender that ignores an error must
not have the rest of a now-invalid program silently executed under it — the
hold forces an acknowledgement. Classic-grbl-compatible builds
(`COMPATIBILITY_LEVEL >= 1`) take the `#else` branch and never hold, which
is why the behaviour surprises people arriving from grbl.

## Consequences for this project

- **haasSender complies already**: it sends `$G` after every manual command
  and, since the fidelity branch, after any streamed-job error halt. Both act
  as the sync acknowledgement.
- **Bare-terminal users (telnet/serial) must sync after an error** — send an
  empty line — before more g-code. Now noted in HAASSENDER-BENCH.md.
- `history/g28-false-alarm.md` in the sender repo is fully explained: the
  "impossible" G28 errors were this hold replaying a previous line's error,
  and the later successes followed `$`-commands.

## Upstream

Not a defect report any more. If anything is worth raising upstream it is a
documentation note: the compat-0 post-error hold and its empty-line sync are
easy to mistake for a wedged parser from a terminal. Owner's call.
