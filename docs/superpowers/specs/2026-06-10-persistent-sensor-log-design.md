# Persistent Sensor Log — Design

Date: 2026-06-10
Status: Approved (design)

## Problem

The demo streams BME280 temperatures over UART and periodically reads readings
back from the flash file system. The readback intermittently emitted implausible
values (e.g. `0xA224BC43` = -15,746,508 C), in bursts of five.

## Root cause (confirmed by hardware capture + arithmetic + source)

The implausible values are **not** from the BME280 / I2C path. Evidence:

- **No `I2C:to` timeout tags** in 140 s+ of `listen.py` capture (`I2C_DEBUG=1`).
- **Arithmetic bound:** `i2c_read_reg` returns `uint8_t`, so `adc_T` is a 20-bit
  value and, with the (proven-good) calibration, `bme280_read_temp` is bounded to
  roughly [-150, +200] C. It cannot produce a billion-magnitude value.
- **Burst timing:** the five "bad" values cluster within ~1.4 s, while live reads
  are ~23.6 s apart (the `for(volatile i<10000000)` delay). The five come from the
  inner readback `for`-loop (no delay), not per-read sensor failures.
- **Five identical values per burst:** the same untouched stack slot read 5 times.

The garbage originates in the readback loop (`main.cpp:39-48`):

1. `fs_create` created only `file_id 0` ("sensor.bin"), but `main` writes/reads
   `file_id = seq % MAX_FILES` (mostly 1..31), which have no directory entry, so
   `fs_read` fails.
2. `fs_read`'s return value is ignored (`main.cpp:42`).
3. `r` is uninitialized (`main.cpp:41`), so failed reads print stale stack — a
   value that is nondeterministic (garbage in one run, a leftover temp in another).

`listen.py:85`'s hardcoded "almost certainly a failed I2C read" note is a guess,
not a measurement, and pointed at the wrong subsystem.

## Goal

Use the file system as a real, reboot-persistent datastore: each reading is stored
in its own file (`file_id = seq % MAX_FILES`), the last 32 readings persist across
power cycles, and a periodic readback prints every slot that holds valid data —
with no implausible values ever emitted.

## Design

One file per slot, round-robin overwrite, 32 slots total.

### Changes

1. **`main`: create 32 files at startup.** Loop `fs_create("s00")`..`fs_create("s31")`
   so file_ids 0..31 each have a directory entry. Idempotent across reboots
   (`fs_create` returns the existing id by name match), so this only writes on the
   first boot after a format.

2. **`main`: keep `fs_write(seq % MAX_FILES, &reading, sizeof(reading))`.** Now
   succeeds for every slot because the directory entry exists.

3. **`main`: readback reads all 32 slots.** Keep the existing periodic trigger
   (`seq % 5 == 0`), but replace the 5-slot window (`main.cpp:39-48`) with a loop
   over `file_id = 0..MAX_FILES-1`. For each slot: initialize `r`, call `fs_read`,
   and **only print `r.temp` when `fs_read` returns 0**.

4. **`fs_create`: initialize `firstpage_addr = 0xFFFFFFFF`** (correctness fix). A
   created-but-never-written file currently has `firstpage_addr = 0`, which is a
   real flash address (the superblock); `fs_read` then scans sector 0 and returns 0
   (success) with the buffer untouched. Setting the `0xFFFFFFFF` sentinel makes
   `fs_read` return -1 for unwritten slots (it already checks for that sentinel at
   `fs.cpp:254`), so the return-code check in change 3 is sufficient.

### Data flow

```
bme280_read_temp() -> reading{seq, temp}
   -> fs_write(seq % 32)        # persists to that slot's file
   -> UART (live value)
every Nth iteration:
   for fid in 0..31:
       init r
       if fs_read(fid, &r) == 0:  # 0 only when real data was copied
           UART (r.temp)
```

### Error handling

- `fs_read` returns -1 for unwritten/missing slots (after change 4) -> skipped.
- `r` is initialized before each read, so a skipped slot never emits stale stack.
- `fs_write` may return -1 when flash is full (see Limitations); the live stream
  continues regardless.

## Limitations (accepted, out of scope)

1. **No sector reclamation.** Overwriting a slot orphans the previous sector (stays
   `SECTOR_ACTIVE`, never freed). Flash fills monotonically; after ~2044 writes
   `fs_write` returns -1 and logging stops. Garbage collection is a separate FS
   feature, not in this change.
2. **First-format boot erases sector 0 thirty-two times** (one per `fs_create`).
   One-time only; subsequent boots are read-only directory scans.
3. **`seq` is not persisted** (RAM counter, resets to 0 on reboot). Stored `seq`
   values are not globally unique across sessions. Acceptable for the demo.

## Verification / success criteria

Hardware-in-the-loop via `tools/listen.py`:

1. **No implausible values.** Over a capture spanning a full 32-slot fill (>32
   readings), `listen.py` flags **zero** values as implausible.
2. **Persistence.** Power-cycle the board mid-run; the readback shows the prior
   session's readings (before new writes overwrite those slots).

Optional (if a host build is added in the plan): a RAM-backed-flash unit test
asserting (a) `fs_read` of a created-but-unwritten file returns -1, and (b)
`fs_write`/`fs_read` round-trip for a non-zero `file_id`.
