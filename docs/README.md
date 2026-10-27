# Storage Commitment and DICOM TLS for dgate: overview of this fork

This fork adds storage commitment (incoming and outgoing) and DICOM TLS to dgate, on top of
upstream master `0676009` (1.5.0g4). This page is the entry point for a review. The details are in
two documents:

- [StorageCommitment-and-TLS.md](StorageCommitment-and-TLS.md): behaviour, keys, states, logging,
  limits and the full commit map ("the main document" below, sections are referred to as §n);
- [Building-Windows.md](Building-Windows.md): Windows builds (Visual Studio, optional TLS build,
  mingw-w64) and Windows-specific behaviour.

All AE titles and addresses in the documents are examples. All tests used synthetic data only.

---

## 1. Two variants

| | Variant A | Variant B |
|---|---|---|
| Branch | `sc-incoming` ("incoming") | `sc-outgoing-tls` ("sending and TLS") |
| Tag | `sc-incoming-v1.1` (`sc-incoming-v1`: the same code without the readme addendum and the binary) | `sc-outgoing-tls-rc2` |
| Commits on `0676009` | 3 code commits, plus the readme addendum, the upstream Windows build fix `294b633` (as `ca61fd1`) and a Windows binary | the whole series (see §9 of this page) |
| Contents | storage commitment SCP only: dgate answers N-ACTION requests for data it received | A + outgoing storage commitment (SCU) + DICOM TLS (strict and legacy profile) + verify-before-delete |
| Startup line | `Build variant: SC-in ...` | `Build variant: SC-in SC-out ...` (without OpenSSL) or `Build variant: SC-in SC-out TLS ...` |

The first two commits are the same on both branches (`5e60eb3`, `cef67a7`). The third commit of
variant A (`557f0cc`, the startup line `SC-in`) has its own counterpart in variant B (`e499039`),
so the branches diverge after the second commit. Variant A is the smaller change, for a site that
only needs dgate to confirm what it holds. Variant B is the one to review for everything else.

`DGATE_VERSION` stays `1.5.0g4`. The variant is printed on its own line after the usual startup
line and is available in Lua as the global `buildvariant`.

## 2. Design principle: everything is opt-in

**With every new setting at its default, dgate behaves as 1.5.0g4** (main document §1.2):

- `EnableStorageCommitment=0`: the storage commitment SOP class is refused, as before.
- `OutgoingStorageCommitment=0`: no worker thread, no new tables, no files, no database writes;
  the send paths do one integer test.
- An `acrnema.map` without a 5th column parses and behaves as before; `write_amap` writes it back
  byte-identical apart from its time line.
- `TLSPort` and `TLSLegacyPort` empty: no extra listeners. Without `T` in the map, no connection
  uses TLS. A build without `-DHAVE_OPENSSL` has no TLS code at all; plain socket paths are
  unchanged in both builds.
- dgate never deletes images on its own. Deletion stays with the operator's scripts, and the new
  `sc_verify_and_delete:` only deletes when asked to, after a fresh verification.
- Upstream build files are not changed for Linux: the TLS flags are not added to `maklinux`. On
  Windows, `amake.bat` runs the extra TLS build only when `OPENSSLDIR` is set.

## 3. Features

| Feature | Variant | Where |
|---|---|---|
| Storage commitment SCP (incoming): re-enables the original `StorageCommitmentSupport` behind a switch; checks a database record **and** a non-empty file per instance; refuses an N-ACTION from a calling AE not in `acrnema.map` (0x0124); sends the report on the same or a new association (role negotiation) | A, B | §2 |
| `acrnema.map` 5th column with per-node options; map access under one lock, reload swapped in at once; `write_amap` keeps the column | B | §3.1 |
| Outgoing storage commitment (SCU): sends to `SC` nodes are noted, a worker thread asks per (destination, study), keeps the state per object in the database, retries, times out, logs | B | §3.2–3.4, §3.7 |
| Re-send of objects the destination reports missing (`R`, `Rn`, `RI`), never when the sent copy may differ from the archive copy | B | §3.5 |
| Fail-closed mechanisms: content generations, durable "commitment unsafe" sentinel file, revalidation before a positive answer | B | §3.6 |
| "Study confirmed" check (advisory), server commands, Lua function and hook | B | §3.8 |
| Verify-before-delete: `sc_verify_now:` asks the archive again and compares the local files with what was sent and committed; `sc_verify_and_delete:` verifies and deletes with the local writes of the study excluded | B | §3.9 |
| DICOM TLS, strict profile (PS3.15 B.12, BCP 195 / RFC 9325): outgoing to `T` nodes, incoming on `TLSPort`; never a plain fallback; no session resumption, no renegotiation | B | §4, §4.1, §4.3 |
| Node certificate pinning (`TLSNodeCertsFile`, SHA-256 fingerprints, fail closed) | B | §4.2 |
| Legacy TLS profile for OsiriX-class stations (`TL` outgoing, `TLSLegacyPort` incoming): TLS 1.0–1.2, AES-CBC/SHA-1 only, post-handshake key and signature policy, downgrade protection | B | §4.3 |
| One log line per TLS connection (protocol, cipher, profile, pin result) | B | §4.3 |
| Robustness fixes found on the way: bounded ini cache copies, one-time ini lock, bounded connects, release and sends under a deadline | B | §3.3, §3.9, §7 |
| Windows: Visual Studio 2015+ build fix, optional TLS build in `amake.bat`, mingw-w64 cross build | B | Building-Windows.md |

## 4. Configuration reference (short)

### 4.1 `acrnema.map` 5th column

Only the first token after the compression column is parsed; case does not matter; `-` means no
options. A token that is not entirely option letters (for example a number used by a Lua script)
gives no options, so existing user data in that column keeps working (§3.1).

| Token | Meaning |
|---|---|
| `T` | DICOM TLS to and from this node, strict profile |
| `L` | with `T` only: legacy TLS profile (OsiriX-class nodes) |
| `SC` | outgoing storage commitment for data sent to this node |
| `R` | re-send missing objects, limit `OutgoingSCResendMax` |
| `R<n>` | re-send at most n times (1..999); `R0` = no re-send |
| `RI` | re-send until committed (not stopped by `OutgoingSCDeadline`) |

`T`, `L` and `SC` may come in any order; `R...` comes last. `write_amap` writes the canonical form
`T L SC R...`.

| Example | Meaning |
|---|---|
| `SC` | plain DICOM, storage commitment |
| `TSC` | strict TLS, storage commitment |
| `TLSC` | legacy TLS, storage commitment |
| `SCR5` | plain, storage commitment, re-send at most 5 times |
| `TSCRI` | strict TLS, storage commitment, re-send until committed |
| `TLSCR5` | legacy TLS, storage commitment, re-send at most 5 times |

```
/* AE        IP / host     port   compression   options */
PACS1        192.0.2.10    104    un                        as before
ARCHIVE      192.0.2.20    104    un            SC
ARCHIVE2     192.0.2.21    2762   j2            TSC
WORKSTATION1 192.0.2.40    11112  un            TL
```

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
| `OutgoingSCConnectTimeout` | `30` | worker connections: one deadline for connect, TLS handshake and association setup |
| `OutgoingSCVerifyMax` | `4` | concurrent verifications |
| `OutgoingSCDeleteWriteWait` | `60` | how long a write of a study waits for its verified delete before it is refused |
| `SCUnsafeFile` | `sc_unsafe.flag` | sentinel file; must be on persistent storage |
| `[lua] OutgoingSCStudyConfirmed` | empty | Lua run when a report makes a study confirmed |

Keys starting with `SCTest` exist for the test harness only; they default to 0 (§3.2).

TLS (§4.1):

| Key | Default | Short meaning |
|---|---|---|
| `TLSCertificate` | empty | own certificate (PEM, chain allowed), also used as client certificate |
| `TLSPrivateKey` | the certificate file | its key |
| `TLSCAFile` | empty | trusted CAs or peer certificates |
| `TLSVerifyPeer` | `require` | `require` / `verify` / `ignore` |
| `TLSMinVersion` | `1.2` | `1.2` or `1.3` (strict profile) |
| `TLSCipherList` | built-in | TLS 1.2 suites; can only narrow the built-in list |
| `TLSVerifyHostname` | `0` | 1 = SAN must match the map host or IP (outgoing) |
| `TLSAcceptSelfSigned` | `0` | 1 = forgive only the "self-signed" errors |
| `TLSNodeCertsFile` | empty | per-node SHA-256 fingerprints (e.g. `tlscerts.map`) |
| `TLSPort` | empty | strict TLS listener |
| `TLSEnforceForMappedNodes` | `1` | a `T` node is rejected on the plain port when a TLS port is set |
| `TLSLegacyPort` | empty | legacy-profile TLS listener |

### 4.3 New server commands and Lua

Server commands (all under `checkaccess(ca_script)`, §3.8, §4.2):

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
| `read_tlscerts:` | reload `TLSNodeCertsFile` |

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

**Linux.** `maklinux` is unchanged and builds variant A or B without TLS
(`Build variant: SC-in SC-out`). For the TLS build, add `-DHAVE_OPENSSL` to the compile flags and
`-lssl -lcrypto` to the link (OpenSSL 1.1.1 or later). Nothing else differs (§1.2, §4.4).

**Windows, Visual Studio** (Building-Windows.md §2):

- `src/dgate/amake.bat` as upstream. With Visual Studio 2015 and later, add
  `legacy_stdio_definitions.lib` (as the comment in `src/Amake.Bat` says) and point
  `AsetCompiler64.bat` at the Visual Studio prompt's compiler.
- Optional TLS build: set `OPENSSLDIR` (Shining Light OpenSSL layout, static `/MT` libraries)
  before `amake.bat`; it then also runs `src/AmakeTLS.Bat` and builds `dgate64tls.exe`.
  Without `OPENSSLDIR` nothing extra runs.

**Windows, mingw-w64 cross build** (Building-Windows.md §3): `build/windows-mingw` has a Dockerfile
and `build-mingw.sh` for plain and TLS builds from a Linux or macOS host (`-O0`, as `maklinux`).

**Windows binaries.** On this branch, `install64/dgate64.exe` (plain) and
`install64/dgate64tls.exe` (TLS, OpenSSL linked statically) are built with Visual Studio 2022 from
the series up to `16ee3f2`; the later commits change no dgate source. They are added by the last
commit of the branch, "Windows binaries: dgate64.exe (plain) and dgate64tls.exe (OpenSSL,
static) ...", whose message has the sha256 sums and the build commands. Variant A has its own
plain `install64/dgate64.exe`, built from its own source (commit "Windows binaries: dgate64.exe
(plain) ..." on `sc-incoming`). Both need `lua5.1.dll` from `install64` next to them (the
upstream file, unchanged). `install32/dgate.exe` and the prebuilt GUI `ConquestDICOMServer.exe`
are upstream's and not rebuilt (32-bit builds were not tried; the GUI, Building-Windows.md §6).

## 6. Test coverage

The test harness is not part of this repository: Docker based, synthetic data only (generated UIDs,
synthetic patients and pixel data), DCMTK and pynetdicom as peers.

**Linux suites** (x86_64 and arm64 containers; sqlite, MariaDB and PostgreSQL builds, plain and
TLS; every commit keeps the compile warning count at the baseline). Last full run on the end of the
series before the Windows commits:

| Suite | Result |
|---|---|
| storage commitment regression (incoming, map, outgoing, validation, re-send, confirmed, fail-closed; sqlite, MariaDB, PostgreSQL) | all pass |
| verify-before-delete, incl. writes, imports, replacements and regen during a verified delete | 146/146 on each of the 3 databases, plus 7/7 over TLS (strict and legacy) |
| worker time limits and file stamps | 57/57 on each of the 3 databases |
| TLS: outgoing, incoming, pinning, review fixes, ini cache stress | 40/40, 64/64, 29/29, 65/65, 33/33, 9/9 + 9/9 |
| legacy TLS profile | 110/110 |
| map option parse/format unit test | 58/58 |
| TLS hardening (resumption, renegotiation, cipher list, unloaded pin file, converter forwards) | 70/70 |

The later verify-before-delete cases (writes, imports, replacements and regen during a verified
delete, deadlines) were also run against the build before their fix, as negative controls. The
main document §6 lists the areas covered.

**Windows 11 x64** (Building-Windows.md §1, §5): Visual Studio 2022 with the repository's own
`amake.bat`, and mingw-w64, plain and TLS: start, C-ECHO, C-STORE, incoming SC, outgoing SC to a
Linux dgate by forward and C-MOVE, `sc_confirmed:`, `sc_verify_now:`, strict TLS both ways, legacy
listener, `T` node refused on the plain port, deadline against a peer that stops reading. Plain
11/11, TLS 16/16, stalled peer 5/5. A native ARM64 build (plain) also passes 11/11. The upstream
prebuilt `dgate64.exe` was run as a control.

**Multi-host flow test** (three machines: a variant A source, a variant B router with TLS and
MariaDB on x86_64, and an arm64 machine with two variant B archives, TLS and plain; synthetic studies from 1 to 200 objects, also
4 × 40 MB):

- strict TLS and plain storage commitment end to end (C-MOVE → forward → COMMITTED →
  `sc_confirmed` 1) within seconds;
- legacy TLS 1.0 peer both directions; `T` without `L` refused both ways; never a plain fallback;
- `sc_verify_now:` / `sc_verify_and_delete:`: deleted after a verified commit; not deleted after a
  local overwrite, with the archive stopped, or with the archive frozen after the N-ACTION;
- failure injection: packets to the archive's TLS port dropped while another destination keeps
  committing (one connect limit per pass, no stall); router killed with SIGKILL during a
  200-object transfer, then sentinel and reconciliation: no untouched committed object was
  demoted (0 of 611), the interrupted study stayed unconfirmed until it was sent again.

The flow test found one defect (a verification exceeding its own timeout when the archive freezes
after the N-ACTION); it was fixed (`6f7974d`) and is covered by the local suite. Later builds, up to `7f3b228`, were
deployed to the same machines as a smoke test (TLS storage commitment and a verified delete); the
Windows commits after it were tested on Windows and with the Linux suites.

**OsiriX MD (legacy profile).** Verified: router → OsiriX over plain DICOM (the study arrived
complete). Measured: OsiriX MD negotiates only TLS 1.0/1.2 with AES-CBC suites, so it needs the
legacy profile; with its built-in identity (self-signed RSA 1024) the router refuses the
connection, as designed (§4.3, interoperability note). The router's legacy listener was checked
from the workstation that runs OsiriX with a TLS client (TLS 1.2, DHE-RSA-AES256-SHA, client certificate
requested). **Pending:** a full OsiriX-over-TLS transfer in either direction, after OsiriX is given
a 2048-bit identity.

## 7. Known limitations (summary)

From main document §7, §3.9, §4.5 and Building-Windows.md §5:

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
- Not implemented: per-node SC timing keys; binding a certificate to an AE title (fingerprints are
  used instead).
- Databases: SQLite, MariaDB/MySQL and PostgreSQL tested; ODBC not tested; the built-in dBase driver
  has no outgoing SC.
- An AE title can be spoofed; for `SC` nodes without TLS a commitment result is only as
  trustworthy as the network. Pinning (`TLSNodeCertsFile`) is the real binding.
- Not tried: 32-bit Windows builds; TLS on native ARM64 Windows; the Intel 12.1 / Visual Studio
  2010 toolchain named in `AsetCompiler64.bat`.

## 8. Observations on upstream code

These were found while working on the series. Items 1, 5 and 9 are left as they are; the others
are fixed or worked around here.

1. **`AllowOnlyMappedRemoteAE`, `AllowOnlyMappedLocalAE` and `CheckIPofRemoteAE` have no effect.**
   `flpdu.hpp` declares `virtual BOOL ShouldIAcceptRemoteMapped(BYTE *)` (and `...LocalMapped`);
   `ExtendedPDU_Service` in `nkiqrsop.hpp` defines `ShouldIAcceptRemoteMapped(BYTE *A, int ip)`.
   The signatures differ, so the derived function hides the virtual one instead of overriding it,
   and `flpdu.cxx` calls the base version, which always accepts. Not changed here; the new
   `ShouldIAcceptRemoteTransport` hook uses a matching signature, and the SC refusal of unmapped
   AEs does its own map lookup (§7, item 1).
2. **gpps ini cache:** a cache hit was copied with an unbounded `strcpy` (up to 1023 characters,
   whatever the caller's buffer). Fixed: bounded copy under the lock; values that do not fit the
   cache arrays are not cached; results for values that fit are byte-identical (`e477062`,
   `cba1ee6`, `3cdcb7b`).
3. **Static initialisation order on Windows:** the global `ExtendedPDU_Service` objects in
   `dgate.cpp` read `dicom.ini` in their constructors, before the static objects of `gpps.cpp`
   (included last in `total.cpp`) are constructed. A static lock object there crashed every
   Windows build at startup (Linux tolerates a zeroed mutex). The lock is now made once at its
   first use (`23284c6`, `059817d`). Worth knowing for any future static in `gpps.cpp`.
4. **`odbci.cpp` `#define _errno errno`** breaks Visual Studio 2015 and later (Universal CRT,
   `errno` is `(*_errno())`) and mingw-w64; upstream `8e77e06` does not compile there. Removed
   (`294b633`).
5. **mingw-w64 g++ `-O2`:** a build of upstream `8e77e06` crashes at the first C-ECHO (in
   `CEchoRSP::Write`); MSVC `/O2` does not. Not investigated; the cross build uses `-O0`, as
   `maklinux`.
6. **`amake.bat`** ends with `del build32\*.*` and `del build64\*.*`, which prompt for
   confirmation and stop an unattended build.
7. **Hard-coded MySQL include path** `C:\mysql-5.0.22-win32\include\mysql.h` in `odbci.hpp`; the
   mingw build needs a shim for it and for `<sys\stat.h>` on a case-sensitive file system.
8. Visual Studio 2015+ also needs `legacy_stdio_definitions.lib` for `odbccp32.lib`; the 32-bit
   build needs `lua5.1.32.lib` for luasocket.
9. The plain `Socket::SendBinary` returns TRUE after a partial `send` (left as it is; the TLS path
   and the new deadline-bounded sends have full write loops).
10. Fixed in passing: `write_amap` dropped columns after the 4th; `put_amap` with a 6th item wrote
    `"un,xx"` into the compression field; the incoming SC event on a new association was sent
    without its dataset.

## 9. Commit map (branch `sc-outgoing-tls`, grouped)

Every code commit message has WHAT / WHY / HOW TESTED / NOT COVERED sections. The chronological map is
in main document §8.

**Incoming storage commitment (variant A)**
`5e60eb3` SCP behind `EnableStorageCommitment` ·
`cef67a7` refuse N-ACTION from an AE not in `acrnema.map` ·
variant A only: `557f0cc` build variant line (SC-in)

**`acrnema.map`**
`0a9bcf2` access under a lock, atomic reload ·
`901d1f3` optional 5th column (T, SC, R, Rn, RI) ·
`5adc439` header describes the 5th column

**Outgoing storage commitment: core**
`381b377` part 1: tables, N-EVENT-REPORT ·
`7d1bdde` part 2a: note sends ·
`edcd50d` part 2: worker, N-ACTION, retries ·
`9e2706d` part 3c: "study confirmed", commands, Lua, hook ·
`1a8130a` part 3b: re-send (R, Rn, RI) ·
`62b86ab` no re-send of a copy that may differ

**Outgoing storage commitment: review fixes and fail-closed state**
`0584bc2` `7443a0d` `99597b1` `47c3463` `c7104fa` `84adb5b` `d98e003` `49f6f77` `f2b0eb5`
`a59c37a` `d730e0e` `bb1c66d` `82c7639` `3bbc672` `5a981da` `ed2b484` `78e06dd`

**Outgoing storage commitment: worker limits, file stamps, logging**
`fbf1229` `0e647df` `ea450d9` `ef1e57e` `4d2ad38` `93fa22b` `1c9217a` `3e51e96` `b7d5912`

**DICOM TLS: strict profile, pinning, transport**
`9b61a2a` outgoing TLS ·
`8d29149` incoming `TLSPort` ·
`f7236f7` `TLSAcceptSelfSigned` ·
`4d23427` `TLSNodeCertsFile`, `read_tlscerts:` ·
review fixes `0214762` `5b7cc69` `1d0a9e8` `6053deb` `2adbce2` `0d12737` `4f5a01e` `018ff80`
`b54ec5e` ·
hardening `d5b4557` `186f467` `db1fab3` `a1a51bf` `96afa52` ·
converter forwards `c3fe55d` `74dd8f4` `5cac87e`

**DICOM TLS: legacy profile and logging**
`962ac9c` outgoing `TL` ·
`6390a8e` incoming `TLSLegacyPort` ·
`918f37b` certificate level and post-handshake policy ·
`82f7b4e` one log line per TLS connection

**Verify-before-delete**
`435fb15` `sc_verify_now:` and `scverifynow` ·
`48c2229` local content = committed content (step 7) ·
`0e5d63e` a send that may change the content certifies nothing ·
`cb3d4b1` check after step 7; `sc_verify_and_delete:` ·
`6f7974d` keeps its timeout when the archive stops answering ·
`7413f28` no write of a study admitted while its verified delete runs ·
`2dfeda9` modpatid and merge admit both studies ·
`ec06415` refused import keeps its file ·
`5d2c802` regen under admission ·
`b24c9bd` replacements admit the worklist's study ·
`9b9123e` regen admits the file's study too

**Robustness (sockets, ini cache, build identification)**
`e477062` `cba1ee6` `3cdcb7b` ini cache ·
`e408e76` `e3fb82c` `6609adf` deadline for plain connect and sends ·
`e499039` build variant line ·
`dd233de` remove development-process markers from comments

**Windows**
`23284c6` `059817d` ini lock made at first use, exactly once ·
`294b633` no `#define _errno errno` ·
`d7c9881` optional TLS build in `amake.bat`, mingw-w64 cross build ·
`16ee3f2` quote OpenSSL library paths ·
`8fbf2fe` GUI: "Save configuration" keeps the SC and TLS keys, `dgatesop.lst` lists storage
commitment (GUI source and lua installers) ·
"Windows binaries: dgate64.exe (plain) and dgate64tls.exe (OpenSSL, static) ..." (last commit of
the branch) adds the prebuilt executables

**Documentation**
`26b0a1a` `d7d0678` `be60fcc` `45defab` `08bb743` `c47c0ac` `7f3b228` main document ·
`d7c9881` Building-Windows.md ·
`bb327cd` Windows GUI (Building-Windows.md §6) ·
`58d3853` this page ·
`27c17b9` readme addendum ·
"Docs: final commit map and cross-references"

## 10. How to review

A suggested order, from the smallest and least risky change to the largest:

1. **Variant A** (`5e60eb3`, `cef67a7`): small and self-contained; re-enables the original SCP
   code with checks. Can be taken on its own.
2. **Upstream fixes** that stand alone: the ini cache (`e477062`, `cba1ee6`, `3cdcb7b`, `23284c6`,
   `059817d`), `294b633`, and the `write_amap` / `put_amap` fixes in `901d1f3`.
3. **`acrnema.map`** (`0a9bcf2`, `901d1f3`): the lock and the 5th column; everything else depends
   on it.
4. **DICOM TLS strict profile** (`9b61a2a`, `8d29149`, then the TLS review fixes): almost all of it
   is in `dicomlib/socket.cxx` and the connect sites; plain paths are kept as they were.
5. **Legacy profile** (`962ac9c`, `6390a8e`, `918f37b`, `82f7b4e`): only needed for OsiriX-class
   nodes.
6. **Outgoing storage commitment** (parts 1–3, then the review fixes): the largest part, mostly new
   code in `dgate.cpp` behind `OutgoingStorageCommitment`; read main document §3.3–3.6 first.
7. **Verify-before-delete** (§3.9 first, then `435fb15` onwards): the only path that deletes.
8. **Windows build** (`d7c9881`, `16ee3f2`), the GUI source (`8fbf2fe`) and Building-Windows.md.

If separate pull requests per area are easier to review than one branch, the series can be split
along these lines.
