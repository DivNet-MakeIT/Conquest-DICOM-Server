# Storage Commitment for dgate: overview of this fork (branch sc-outgoing)

This fork adds storage commitment (incoming and outgoing) to dgate, on top of upstream master
`f8c0057` (1.5.0g4). This page is the entry point for a review of branch `sc-outgoing`, the default
branch of the fork. The details are in two documents:

- [StorageCommitment.md](StorageCommitment.md): behaviour, keys, states, logging, limits and the
  commit map ("the main document" below, sections are referred to as §n);
- [Building-Windows.md](Building-Windows.md): Windows builds (Visual Studio, mingw-w64) and
  Windows-specific behaviour.

All AE titles and addresses in the documents are examples. All tests used synthetic data only.

---

## 1. Three variants

| | Variant A | Variant B | Variant C (this branch) |
|---|---|---|---|
| Branch | `sc-incoming` | `sc-outgoing-tls` | `sc-outgoing` (default branch) |
| Tag | `sc-incoming-v1.1` (`sc-incoming-v1`: the same code without the readme addendum and the binary) | `sc-outgoing-tls-rc2` | `sc-outgoing-v1` |
| What | storage commitment SCP only: dgate answers N-ACTION requests for data it received | incoming + outgoing storage commitment + verify-before-delete + DICOM TLS (strict and legacy profile) | incoming + outgoing storage commitment + verify-before-delete, without DICOM TLS |
| Base | `f8c0057` (1.5.0g4) | `f8c0057` (1.5.0g4) | `f8c0057` (1.5.0g4) |
| Startup line | `Build variant: SC-in ...` | `Build variant: SC-in SC-out TLS ...` (built with OpenSSL) | `Build variant: SC-in SC-out (storage commitment for incoming and outgoing data)` |

- The first two commits are the same on all three branches (`3183c6e`, `edadb73`).
- The older public branch `storage-commitment` is the earlier version of these two commits, on
  upstream `41575e8`. `sc-incoming` is the same two commits on 1.5.0g4, plus the build variant
  line, the upstream Windows build fix, a readme addendum and a Windows binary.
- `sc-outgoing` has exactly the storage commitment of `sc-outgoing-tls`: incoming and outgoing,
  re-send (`R`, `Rn`, `RI`), verify-before-delete (`sc_verify_now:`, `sc_verify_and_delete:`), all
  robustness fixes, the Windows fixes and the GUI source that keeps the keys on save. It has no
  DICOM TLS: no OpenSSL code, no TLS listeners, no TLS keys.
- Choose A for a site that only needs dgate to confirm what it holds, B when DICOM TLS is needed,
  C for everything else.

`DGATE_VERSION` stays `1.5.0g4`. The variant is printed on its own line after the usual startup
line and is available in Lua as the global `buildvariant`.

## 2. Design principle: everything is opt-in

**With every new setting at its default, dgate behaves as 1.5.0g4** (main document §1.2):

- `EnableStorageCommitment=0`: the storage commitment SOP class is refused, as before.
- `OutgoingStorageCommitment=0`: no worker thread, no new tables, no files, no database writes;
  the send paths do one integer test.
- An `acrnema.map` without a 5th column parses and behaves as before; `write_amap` writes it back
  byte-identical apart from its time line.
- No extra listeners and no new build dependency. Upstream build files (`maklinux`, `amake.bat`)
  build this branch as they are.
- dgate never deletes images on its own. Deletion stays with the operator's scripts, and the new
  `sc_verify_and_delete:` only deletes when asked to, after a fresh verification.

## 3. Features

| Feature | Variant | Where |
|---|---|---|
| Storage commitment SCP (incoming): re-enables the original `StorageCommitmentSupport` behind a switch; checks a database record **and** a non-empty file per instance; refuses an N-ACTION from a calling AE not in `acrnema.map` (0x0124); sends the report on the same or a new association (role negotiation) | A, B, C | §2 |
| `acrnema.map` 5th column with per-node options; map access under one lock, reload swapped in at once; `write_amap` keeps the column | B, C | §3.1 |
| Nodes marked `T` (need DICOM TLS) are refused at every connect site, never reached in plain text | C (B without OpenSSL) | §3.1 |
| Outgoing storage commitment (SCU): sends to `SC` nodes are noted, a worker thread asks per (destination, study), keeps the state per object in the database, retries, times out, logs | B, C | §3.2–3.4, §3.7 |
| Re-send of objects the destination reports missing (`R`, `Rn`, `RI`), never when the sent copy may differ from the archive copy | B, C | §3.5 |
| Fail-closed mechanisms: content generations, durable "commitment unsafe" sentinel file, revalidation before a positive answer | B, C | §3.6 |
| "Study confirmed" check (advisory), server commands, Lua function and hook | B, C | §3.8 |
| Verify-before-delete: `sc_verify_now:` asks the archive again and compares the local files with what was sent and committed; `sc_verify_and_delete:` verifies and deletes with the local writes of the study excluded | B, C | §3.9 |
| Robustness fixes found on the way: bounded ini cache copies, one-time ini lock, bounded connects, release and sends under a deadline | B, C | §3.3, §3.9, §6 |
| Windows: Visual Studio 2015+ build fix, mingw-w64 cross build | B, C | Building-Windows.md |
| DICOM TLS (strict and legacy profile, node certificate pinning) | B only | `sc-outgoing-tls` |

## 4. Configuration reference (short)

### 4.1 `acrnema.map` 5th column

Only the first token after the compression column is parsed; case does not matter; `-` means no
options. A token that is not entirely option letters (for example a number used by a Lua script)
gives no options, so existing user data in that column keeps working (§3.1).

| Token | Meaning |
|---|---|
| `T` | the node needs DICOM TLS; this dgate has no TLS, so every association to it is refused |
| `L` | only with `T` (legacy TLS profile); refused the same way |
| `SC` | outgoing storage commitment for data sent to this node |
| `R` | re-send missing objects, limit `OutgoingSCResendMax` |
| `R<n>` | re-send at most n times (1..999); `R0` = no re-send |
| `RI` | re-send until committed (not stopped by `OutgoingSCDeadline`) |

`T`, `L` and `SC` may come in any order; `R...` comes last. `write_amap` writes the canonical form
`T L SC R...`.

| Example | Meaning |
|---|---|
| `SC` | storage commitment |
| `SCR` | storage commitment, re-send up to `OutgoingSCResendMax` times |
| `SCR5` | storage commitment, re-send at most 5 times |
| `SCRI` | storage commitment, re-send until committed |

```
/* AE        IP / host     port   compression   options */
PACS1        192.0.2.10    104    un                        as before
ARCHIVE      192.0.2.20    104    un            SC
ARCHIVE3     192.0.2.22    104    un            SCR5
```

**Nodes marked `T`.** `T` and `L` are still parsed and written back, so a map can be shared with
`sc-outgoing-tls`. This branch has no TLS, so every outgoing association to a node with `T` (or
`TL`) is refused, never sent in plain text: forwards (also with a `dest` clause whose called AE has
`T`), C-MOVE sub-associations, Lua `dicomstore`/`dicomecho`/`dicomquery`/`dicommove`/`dicomget`/
`dicomprint`, virtual server, grab, echo, the storage commitment event on a new association, the
outgoing storage commitment worker and re-sends, and `sc_verify_now:` (`0 TLS not available for
this node`). Loading the map prints `*** acrnema.map: <AE>: T (DICOM TLS) but this dgate has no
TLS: every association to <AE> is refused` for each such entry. Incoming associations from a
calling AE with `T` are accepted on the normal port, as before (§3.1).

### 4.2 New `dicom.ini` keys (`[sscscp]` unless noted)

Storage commitment (§2, §3.2):

| Key | Default | Short meaning |
|---|---|---|
| `EnableStorageCommitment` | `0` | answer incoming N-ACTION requests (SCP) |
| `OutgoingStorageCommitment` | `0` | master switch for outgoing SC; the map's `SC` flags take effect |
| `OutgoingSCCheckIP` | `1` | a report must come from the numeric map address of the AE |
| `OutgoingSCQuietTime` | `30` | seconds after the last send of a (destination, study) before asking |
| `OutgoingSCMaxBatch` | `500` | objects per N-ACTION |
| `OutgoingSCSameAssocWait` | `5` | seconds to wait for the report on the request association |
| `OutgoingSCReportTimeout` | `3600` | no report within this time = failed attempt |
| `OutgoingSCRetries` | `5` | re-asks after a failed attempt |
| `OutgoingSCRetryInterval` | `60,300,900,3600` | delays before re-asks; the last value repeats |
| `OutgoingSCDeadline` | `86400` | after first send + deadline the object is TIMEOUT (not for `RI`) |
| `OutgoingSCScanInterval` | `15` | worker loop period |
| `OutgoingSCKeepDays` | `7` | purge COMMITTED rows and closed transactions |
| `OutgoingSCResendMissing` | `1` | 0 off, 1 transient failures, 2 also objects without a report |
| `OutgoingSCResendMax` | `3` | limit for plain `R` |
| `OutgoingSCResendDelay` | `300` | delay before a re-send (doubles after failures) |
| `OutgoingSCConnectTimeout` | `30` | worker connections: one deadline for connect and association setup |
| `OutgoingSCVerifyMax` | `4` | concurrent verifications |
| `OutgoingSCDeleteWriteWait` | `60` | how long a write of a study waits for its verified delete before it is refused |
| `SCUnsafeFile` | `sc_unsafe.flag` | sentinel file; must be on persistent storage |
| `[lua] OutgoingSCStudyConfirmed` | empty | Lua run when a report makes a study confirmed |

Keys starting with `SCTest` exist for the test harness only; they default to 0 (§3.2).

### 4.3 New server commands and Lua

Server commands (all under `checkaccess(ca_script)`, §3.8):

| Command | Purpose |
|---|---|
| `sc_queue:` | noted sends not yet taken by the worker |
| `sc_status:` | per destination: objects per state, open transactions, re-send counts, sentinel |
| `sc_confirmed:<StudyUID>[,<AE>...]` | 1 when every local object of the study is committed (advisory) |
| `sc_confirmed_list:<since>[,<AE>...]` | studies fully confirmed now (advisory) |
| `sc_cancel:<AE>[,<StudyUID>]` | cancel open work for a node or study |
| `sc_unsafe:` / `sc_unsafe_clear:` | show / reconcile and clear the sentinel (`sc_overflow_clear:` is an alias; `sc_overflow:` shows the overflow state) |
| `sc_verify_now:<StudyUID>,<AE>[,<timeout>]` | fresh verification: `1` or `0 <reason>` |
| `sc_verify_and_delete:<StudyUID>,<AE>[,<timeout>]` | verify, then delete locally with writes of the study excluded |

Lua:

- `scconfirmed(studyuid [, ae, ...])` → `true` / `false` / `nil` (advisory);
- `scverifynow(studyuid, ae [, timeout])` → `true`, or `false, reason`;
- `scverifyanddelete(studyuid, ae [, timeout])` → the same, deleting on success;
- global `buildvariant`;
- `get_amap(i)` returns the 5th column as a 5th value when the line has one; `put_amap` takes an
  optional 6th item; `get_amap`/`get_amaps` server commands pass it as a 13th format argument.
- Example, not enabled anywhere: `lua/sc_delete_if_verified.lua`.

**Advisory vs. delete.** `sc_confirmed:`, `scconfirmed()` and the hook are for monitoring. Delete
only with `sc_verify_and_delete:` / `scverifyanddelete`, or right after `sc_verify_now:` answered
`1` (§3.9).

## 5. Building

**Linux.** `maklinux` is unchanged and builds this branch (`Build variant: SC-in SC-out`). There
is no other build option.

**Windows, Visual Studio** (Building-Windows.md §2): `src/dgate/amake.bat` as upstream. With Visual
Studio 2015 and later, add `legacy_stdio_definitions.lib` (as the comment in `src/Amake.Bat` says)
and point `AsetCompiler64.bat` at the Visual Studio prompt's compiler.

**Windows, mingw-w64 cross build** (Building-Windows.md §3): `build/windows-mingw` has a Dockerfile
and `build-mingw.sh` (`build-mingw.sh /src /out`) for a build from a Linux or macOS host (`-O0`, as
`maklinux`).

**Windows binaries.** On this branch, `install64/dgate64.exe` is added by the last commit of the
branch, "Windows binaries: dgate64.exe", whose message has the sha256 sum and the build commands.
It needs `lua5.1.dll` from `install64` next to it (the upstream file, unchanged).
`install32/dgate.exe` and the prebuilt GUI `ConquestDICOMServer.exe` are upstream's and not rebuilt
(32-bit builds were not tried; the GUI, Building-Windows.md §6).

## 6. Test coverage

The test harness is not part of this repository: Docker based, synthetic data only (generated UIDs,
synthetic patients and pixel data), DCMTK and pynetdicom as peers.

**This branch** was tested on Linux with:

- the storage commitment regression suites (incoming, map, outgoing, validation, re-send,
  confirmed, fail-closed);
- the verify-before-delete suite, including writes, imports, replacements and regen during a
  verified delete;
- the worker time limit and file stamp suite;
- the ini cache concurrency stress test;
- a test of the refusal of nodes with `T`: no association to them, never plain text; storage
  commitment requests and `sc_verify_now:` send nothing.

**On `sc-outgoing-tls`**, whose storage commitment code this branch has unchanged, the same suites
ran on sqlite, MariaDB and PostgreSQL builds (x86_64 and arm64), together with the Windows 11 tests
(Visual Studio 2022 and mingw-w64, x64 and native ARM64: start, C-ECHO, C-STORE, incoming SC,
outgoing SC by forward and C-MOVE, `sc_confirmed:`, `sc_verify_now:`, deadline against a peer that
stops reading) and a multi-host flow test (synthetic studies from 1 to 200 objects, also 4 × 40 MB;
storage commitment end to end, verified delete, failure injection with a destination that drops
packets and a router killed during a transfer). Details: main document §5 and Building-Windows.md
§1, §5.

## 7. Known limitations (summary)

From main document §6, §3.9 and Building-Windows.md §5:

- **No DICOM TLS.** A node marked `T` in `acrnema.map` cannot be reached from this branch. Use
  `sc-outgoing-tls` where TLS is needed.
- **Windows:** no nanosecond file times, so no file stamps: `sc_verify_now:` answers
  `0 changed since send` and `sc_verify_and_delete:` never deletes. There is no same-association
  wait (no watchdog with winsock 1): the result of a request always comes on a new association, so
  the archive must be able to connect back to dgate. The worker closes its request associations
  without A-RELEASE.
- **Windows GUI:** it does not expose the new keys, and its "Save config" (`Serverdriver.pas`)
  rewrites a map line with 4 columns, which can drop the 5th column of that line. The prebuilt GUI
  also writes the old `acrnema.map` header comment, without the description of the 5th column.
- **Verified delete:** the write admissions are per process: a `dgate -r` or a modification run in
  another dgate process is not excluded. A change of a file outside dgate is only detected by the
  fingerprints. Two record-only writes (a virtual-compression `v` AE, the virtual-query cache) are
  not excluded but are detected after the delete. The delete itself is not a transaction.
- Outgoing SC covers forwards, C-MOVE and Lua `dicomstore`, not C-GET or `TestForward`.
- Incoming SC reports the state when the N-ACTION arrives; the requester's N-EVENT-REPORT-RSP
  status is not evaluated (as in the original code).
- Not implemented: per-node SC timing keys.
- Databases: SQLite, MariaDB/MySQL and PostgreSQL tested; ODBC not tested; the built-in dBase driver
  has no outgoing SC.
- An AE title can be spoofed; a commitment result is only as trustworthy as the network, even with
  the AE + IP + exact-set checks.
- Not tried: 32-bit Windows builds; the Intel 12.1 / Visual Studio 2010 toolchain named in
  `AsetCompiler64.bat`.

## 8. Observations on upstream code

These were found while working on the series. Items 1, 5 and 9 are left as they are; the others
are fixed or worked around here.

1. **`AllowOnlyMappedRemoteAE`, `AllowOnlyMappedLocalAE` and `CheckIPofRemoteAE` have no effect.**
   `flpdu.hpp` declares `virtual BOOL ShouldIAcceptRemoteMapped(BYTE *)` (and `...LocalMapped`);
   `ExtendedPDU_Service` in `nkiqrsop.hpp` defines `ShouldIAcceptRemoteMapped(BYTE *A, int ip)`.
   The signatures differ, so the derived function hides the virtual one instead of overriding it,
   and `flpdu.cxx` calls the base version, which always accepts. Not changed here; the SC refusal
   of unmapped AEs does its own map lookup (§6, item 1).
2. **gpps ini cache:** a cache hit was copied with an unbounded `strcpy` (up to 1023 characters,
   whatever the caller's buffer). Fixed: bounded copy under the lock; values that do not fit the
   cache arrays are not cached; results for values that fit are byte-identical.
3. **Static initialisation order on Windows:** the global `ExtendedPDU_Service` objects in
   `dgate.cpp` read `dicom.ini` in their constructors, before the static objects of `gpps.cpp`
   (included last in `total.cpp`) are constructed. A static lock object there crashed every
   Windows build at startup (Linux tolerates a zeroed mutex). The lock is now made once at its
   first use. Worth knowing for any future static in `gpps.cpp`.
4. **`odbci.cpp` `#define _errno errno`** breaks Visual Studio 2015 and later (Universal CRT,
   `errno` is `(*_errno())`) and mingw-w64; upstream `8e77e06` does not compile there. Removed.
5. **mingw-w64 g++ `-O2`:** a build of upstream `8e77e06` crashes at the first C-ECHO (in
   `CEchoRSP::Write`); MSVC `/O2` does not. Not investigated; the cross build uses `-O0`, as
   `maklinux`.
6. **`amake.bat`** ends with `del build32\*.*` and `del build64\*.*`, which prompt for
   confirmation and stop an unattended build.
7. **Hard-coded MySQL include path** `C:\mysql-5.0.22-win32\include\mysql.h` in `odbci.hpp`; the
   mingw build needs a shim for it and for `<sys\stat.h>` on a case-sensitive file system.
8. Visual Studio 2015+ also needs `legacy_stdio_definitions.lib` for `odbccp32.lib`; the 32-bit
   build needs `lua5.1.32.lib` for luasocket.
9. The plain `Socket::SendBinary` returns TRUE after a partial `send` (left as it is; the new
   deadline-bounded sends have full write loops).
10. Fixed in passing: `write_amap` dropped columns after the 4th; `put_amap` with a 6th item wrote
    `"un,xx"` into the compression field; the incoming SC event on a new association was sent
    without its dataset.

## 9. Commit map (branch `sc-outgoing`)

In order on top of `f8c0057` (hashes: `git log f8c0057..sc-outgoing`):

1. `3183c6e` Storage commitment SCP behind dicom.ini EnableStorageCommitment (default 0)
2. `edadb73` Storage commitment: refuse N-ACTION from a calling AE not in acrnema.map
3. odbci.cpp: no #define _errno errno (Visual Studio 2015+ and mingw-w64)
4. Outgoing storage commitment (SCU), re-send and verify-before-delete, without DICOM TLS
5. Windows build: mingw-w64 cross build
6. GUI: Save configuration keeps the storage commitment keys
7. Docs: storage commitment without TLS
8. Readme: addendum for this branch
9. Windows binaries: dgate64.exe

Commit 4 holds, in one commit, the storage commitment parts of the series on `sc-outgoing-tls`.
Main document §7.2 lists the commits of `sc-outgoing-tls` it comes from, with their hashes; each of
those commit messages has WHAT / WHY / HOW TESTED / NOT COVERED sections.

## 10. How to review

A suggested order, from the smallest and least risky change to the largest:

1. **Incoming storage commitment** (`3183c6e`, `edadb73`): small and self-contained; re-enables the
   original SCP code with checks. Can be taken on its own (branch `sc-incoming`).
2. **Upstream fixes** that stand alone: the ini cache (in commit 4) and `odbci.cpp` (commit 3).
3. **`acrnema.map`**: the lock and the 5th column (§3.1); everything else depends on it.
4. **Outgoing storage commitment** (commit 4): the largest part, mostly new code in `dgate.cpp`
   behind `OutgoingStorageCommitment`; read main document §3.3–3.6 first. The history of each part,
   step by step and with review fixes, is on `sc-outgoing-tls` (main document §7.2).
5. **Verify-before-delete** (§3.9 first): the only path that deletes.
6. **Windows build** (commit 5), the GUI source (commit 6) and Building-Windows.md.

The DICOM TLS code that is only in variant B is `git diff sc-outgoing sc-outgoing-tls`.
