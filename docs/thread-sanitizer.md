# ThreadSanitizer on the Linux bench

The opt-in bench build instruments the host runtime with Clang's ThreadSanitizer (TSan). The generated game C is not
instrumented: a race involving an access from that code can be missed. Vulkan drivers and other prebuilt libraries are
not necessarily instrumented either. A clean report is not proof that the game has no races.

Keep this build out of releases. `NX2SARIA_BENCH_TSAN=1` requires an output folder named `g25-tsan`; the build marks it
and refuses to use it for an ordinary build. It compiles the runtime rather than linking the prebuilt runtime. The host
object keys and link key include the sanitizer flags, and old unsanitized host object keys are not adopted. The generated
game's compile commands stay unchanged. Use a separate build cache and do not copy the instrumented objects into a kit.

Use the bench's serialized branch-build script with the environment variable set and put the whole script in a memory
limited scope: its shader preparation and configuration probe can also start the executable. The bench script takes the
shared build lock. Make sure no game is running first, check free disk space and use a scratch directory for reports.
For example, after setting `BENCH` to the bench root, `CHECKOUT` to this checkout and `SCRATCH` to an existing scratch
directory:

```sh
pgrep -x ocarina   # stop here if it prints a process
systemd-run --user --scope -p MemoryMax=18G -p MemorySwapMax=0 \
  env NX2SARIA_BENCH_TSAN=1 NX2SARIA_PGO=off SDL_VIDEODRIVER=offscreen \
  SDL_AUDIO_DRIVER=dummy NX2SARIA_AUDIO=none \
  TSAN_OPTIONS="halt_on_error=0:history_size=3:second_deadlock_stack=1:log_path=$SCRATCH/tsan:suppressions=$CHECKOUT/tools/tsan.supp" \
  "$BENCH/xbuild/build-branch.sh" g25-tsan "$CHECKOUT"
```

The script builds the committed checkout, not uncommitted changes. Confirm the scope actually has `memory.max` set to
18 GiB and `memory.swap.max` to zero on the bench; do not run the game if the limit cannot be enforced. Use the same
memory limit and the shared lock for each separate run. Keep a copy of the game's ini and compare it afterward. Run
offscreen with dummy audio; do not use Xvfb. For a repeatable scene use `tools/run_game.py --boot-map` with a copied
profile and backed-up save as described in [boot-to-map.md](boot-to-map.md). Keep game logs and TSan reports in the
scratch directory, and verify that `ocarina` has exited.

Before treating an empty report as evidence, run the instrumented executable with `NX2SARIA_TSAN_PROBE=1` in a separate
short probe and verify that its TSan report names `tsan_probe_writer` and a data race. The switch is compiled only into
the sanitizer build. Leave it unset during game runs. TSan writes a file per process using `log_path`; preserve all files,
including any from re-execution. `halt_on_error=0` allows more reports, while an explicit wall-clock timeout keeps the
first run short. A TSan process may use substantially more memory than the ordinary game.

Classify each distinct report from its stacks and synchronization: a real race, a documented benign race, or a false
positive with evidence. Fix real races in separately reviewable changes. Add entries to `tools/tsan.supp` only for
investigated benign reports or false positives and explain each entry there. Hardware or driver synchronization may not
be visible to TSan; do not suppress an unexplained report merely because it involves Vulkan.

## Before each large release

Besides a TSan run of a few scenes, the bench runs the replay probes. They need no game files or GPU; they build the
real `vk_usnap.c` and `vk_memory.c` with stand-ins for the rest of the runtime:

```sh
python tools/build_shaders.py   # vk_memory.c includes the shader headers; or pass --spv-include <kit>/runtime
python tests/replay_hazard_check.py --vma-include toolchain/vma --vulkan-include toolchain/vulkan-headers/include \
  --work "$SCRATCH/replay-hazard" --tsan
python tests/usnap_concurrency_check.py --vma-include toolchain/vma --vulkan-include toolchain/vulkan-headers/include \
  --work "$SCRATCH/usnap-concurrency" --tsan
```

Each prints one line per case and exits 0 when every case is as expected. `vkmem lifetime: protocol held; controls
reproduced the failures` is the expected last line of the lifetime probe: a control that no longer fails means the probe
lost its reach, not that the code got safer.

## Replay hazards TSan cannot see

Two orders in the replay are wrong or right whatever the memory model says, so a clean TSan report says nothing about
them. The probes force them with barriers.

**Uniform snapshot drops** (`vk_usnap.c` `vk_usnap_drop`). A draw that writes guest memory as storage drops the uniform
snapshot regions it touches from its place in the stream (`vk_exec_stream_pos`), and later bindings of that memory read
the live bytes, which hold its result when the GPU runs them.
- A later chunk's drop is not seen by an earlier chunk's lookup, even when it ran first: the earlier draw binds the
  snapshot, as of the submit.
- An earlier chunk's drop that has not run yet when a later chunk (or, with `replay_pipe`, a later group or the
  submitting thread past the group) looks the range up is not seen either: the later draw binds the snapshot instead of
  the earlier draw's result. This is left open. Its precondition, a GPU write recorded by a chunk into a copied range,
  is counted, and the first four each name the range: `vk_usnap: a replay chunk's GPU write into ... dropped a copied
  uniform range` and `vk_usnap: GPU writes recorded by replay chunks that dropped copied uniform ranges: N so far` every
  3600 submissions. None happened in Kokiri Forest, Hyrule Field by night and by day, or Kakariko Village
  (2026-11-20, about 75,000 submissions, 80,000 drops by chunks), with the default workers and with `replay_pipe = 2`:
  every drop into a copied range came from the submitting thread, which a group's chunks never run beside in the stream
  (an inline uniform update ends a group; a copy drains the groups). A fix (an earlier chunk's possible writer making
  the lookup wait for its turn) waits for a log that shows the count above zero.

**Pool records and VRAM mirrors** (`vk_memory.c`). No lifetime protocol beyond the current one is needed:
- A pool the game finalizes while the replay records keeps its record until the submission being recorded completes:
  a resolve marks the pool used, and `pool_fini` then waits for that submission (`pool_wait_add`). In the runs above
  and two with four teleports each, 2,675 of 8,411 finalizes came while a submission was being recorded (the replay is
  on the submitting thread by default), and none of the records was used after its finalize began.
- A mirror is evicted only when no submission synced it for `MIRROR_IDLE` submissions, so the unlocked fast path of
  `vk_mem_use` (which needs the current submission's sync) never takes a victim, and the slow path runs under the
  `use_lock` that `mirror_create`'s eviction holds while chunks record.
- `tests/vkmem_lifetime_check.c` shows both: the protocol holds, and taking out the used mark gives a use after free,
  taking out the idle rule and the lock gives uses without a buffer.
- Eviction itself is not reached in the game on the bench. With the budget taken as 1200 and 768 MiB
  (`NX2SARIA_VRAM_BUDGET_MB`) and four teleports (`NX2SARIA_DBG_TEST` `warp`: Kokiri Forest, Hyrule Field, the Lost
  Woods and back), over 1,000 new mirrors did not fit and each asked for an eviction first, and none found a victim:
  at an area change the game finalizes the area's pools (3,348 and 4,636 finalizes), so their mirrors go with the
  pool, and the mirrors left are used every frame. A pool that lives on unused for `MIRROR_IDLE` submissions on a card
  short of video memory would be the case; only the probe covers it.
