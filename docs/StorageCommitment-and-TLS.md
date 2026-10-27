# Storage Commitment (incoming and outgoing) and DICOM TLS in dgate

This document covers changes to dgate on top of upstream master `f8c0057` (1.5.0g4). It is written for
someone who knows the Conquest code base. Each statement here has been checked against the code at
the end of this series: outgoing storage commitment with verify-before-delete, the legacy TLS
profile and the worker time limits.

All AE titles, addresses and file names below are examples (RFC 5737 addresses `192.0.2.x`).

---

## 1. Overview

### 1.1 Build variants

| Variant | Contents | `DGATE_BUILD_VARIANT` |
|---|---|---|
| **A** | Storage commitment SCP only (incoming), commits `3183c6e`, `edadb73` | `SC-in` |
| **B** | A + outgoing storage commitment (SCU) + DICOM TLS | `SC-in SC-out` (built without OpenSSL), `SC-in SC-out TLS` (built with `-DHAVE_OPENSSL`) |

`DGATE_VERSION` is still `1.5.0g4`. dgate prints the variant at startup on a separate line, after
the usual `DGATE (...) is running as threaded server` line:

```
Build variant: SC-in (storage commitment for incoming data)                                   [A]
Build variant: SC-in SC-out (storage commitment for incoming and outgoing data, no TLS in this build)
Build variant: SC-in SC-out TLS (storage commitment for incoming and outgoing data, DICOM TLS)
```

Lua scripts can read the same string in the global `buildvariant`, next to `version`.

### 1.2 Principle: everything is opt-in

**With every new setting at its default, dgate behaves exactly as the previous version.**

- `EnableStorageCommitment=0`: the storage commitment SOP class is refused at association time,
  as before.
- `OutgoingStorageCommitment=0`: no worker thread, no new tables, no files, no database writes.
  The send paths do one integer test and nothing more.
- An `acrnema.map` without a 5th column parses and behaves as before, and `write_amap` writes it
  back byte-identical apart from its time line.
- `TLSPort` and `TLSLegacyPort` are empty, so there are no extra listeners. Without `T` in the
  map, no connection uses TLS.
- A build without `HAVE_OPENSSL` has no TLS code at all. Plain socket paths are unchanged in both
  builds.
- The changes never delete images. Deletion stays with the operator's scripts (see 3.9).

The TLS build flags (`-DHAVE_OPENSSL -lssl -lcrypto`, OpenSSL ≥ 1.1.1) are **not** added to
`maklinux` or other upstream build files. That is left to the maintainer. Windows (MSVC, Schannel or
OpenSSL DLL packaging) was not built or tested.

---

## 2. Incoming storage commitment (dgate as SCP)

This re-enables the original `StorageCommitmentSupport`, which has been blocked out since
20090513. It is now made safe and sits behind a switch.

| Key (`[sscscp]`) | Default | Meaning |
|---|---|---|
| `EnableStorageCommitment` | `0` | 1 = answer Storage Commitment Push Model (1.2.840.10008.1.20.1) N-ACTION requests |

`linux/dgatesop.lst*` now lists the class. While both `EnableStorageCommitment` and
`OutgoingStorageCommitment` are 0, the class is blanked from the thread's SOP list, so the
association refuses it as before.

**What is checked for each Referenced SOP Instance (0008,1155) in 0008,1199**
- There must be a database record (`GetFileName`) **and** a non-empty file on disk
  (`FindPhysicalDevice` + `DFileSize`).
- An instance that fails either check goes to the Failed SOP Sequence (0008,1198) with
  FailureReason 0x0112, and the event becomes Event Type 2. Event Type 1 is sent only when every
  instance passed.

**Refusals**
- Calling AE not in `acrnema.map` (normal `GetACRNema` matching, including wildcards): N-ACTION-RSP
  **0x0124** (not authorised), no event, and a console line. Other services from that AE are not
  affected. Without this check, any SCU could probe for SOP Instance UIDs, and the return address
  for a new association can only come from the map anyway. This check is done by
  `StorageCommitmentSupport` itself for every N-ACTION it reads; it does not depend on the upstream
  keys `AllowOnlyMappedRemoteAE` / `CheckIPofRemoteAE`, which have no effect (section 7).
- Request without an instance list: **0x0120**.
- N-ACTION that cannot be read: no event.
- `OutgoingStorageCommitment=1` with `EnableStorageCommitment=0`: an incoming N-ACTION gets
  **0x0118** (no such SOP class). The class is then only accepted for N-EVENT-REPORTs (section 3).

**Report**
- The report goes on the same association after ~200 ms (the original `Sleep(200)` is kept). If
  the requester has released, it goes on a new association to the map address, which proposes the
  SCP role for the class.
- `PDU_Service::Connect` now keeps role items set by the caller, mirroring the acceptor side
  (mvh 20230608).
- Bug fixed: the old code sent the new-association event without its dataset, because `Write`
  empties the object. A copy is now written.
- The result is sent once on the same association and, failing that, once on a new one. It is not
  stored or retried. An unreachable requester gets no confirmation and keeps its images, which is
  the safe side.

---

## 3. Outgoing storage commitment (dgate as SCU)

After a successful C-STORE to a node marked `SC`, dgate asks that node for a storage commitment.
It keeps the state per object and per destination in the database, and retries, re-sends or times
out according to `dicom.ini`.

### 3.1 `acrnema.map` 5th column

```
/* AE        IP / host     port   compression   options */
PACS1        192.0.2.10    104    un                         as before
ARCHIVE      192.0.2.20    104    un            SC
ARCHIVE2     192.0.2.21    2762   j2            TSC
ARCHIVE3     192.0.2.22    104    un            SCR5
ARCHIVE4     192.0.2.23    2762   un            TSCRI
VIEWER       192.0.2.30    2762   un            T
WORKSTATION1 192.0.2.40    11112  un            TL
```

Only the first token after the compression column is parsed. Case does not matter, and `-` means
"no options".

| Token | Meaning |
|---|---|
| `T` | DICOM TLS to (and from) this node, strict profile (section 4) |
| `L` | with `T` only: legacy TLS profile for OsiriX-class nodes (4.3) |
| `SC` | outgoing storage commitment for data sent to this node |
| `R` | re-send missing objects, limit `OutgoingSCResendMax` |
| `R<n>` | re-send at most n times (1..999); `R0` = no re-send |
| `RI` | re-send until committed (not stopped by `OutgoingSCDeadline`) |

**Combining tokens**
- `T`, `L` and `SC` may appear in any order. `R…` comes last.
- Examples: `TSC` / `SCT`, `SCR`, `SCR5`, `TSCRI`, `TL`, `TLSC`, `TLSCR5`.

**Ignored tokens** (each gives a console warning when the map is loaded)
- A token that is not entirely option letters gives **no options** for that line. This keeps the
  Lua convention working: `lua/association.lua` documents the 5th column as user data, e.g. a
  C-MOVE thread count read through `find_amap(...)[5]`. A number is never taken as an option.
- `R` without `SC` is ignored, and so is `R` on a wildcard entry.
- `L` without `T` is ignored (`*** TLS: acrnema.map: <AE>: L needs T, L ignored (no TLS for this entry)`):
  the entry gets no TLS at all.

**How the map commands handle the column**
- The text after the compression column, up to 127 characters, is kept as read
  (`ACRNemaAddress.Options`), together with the parsed `Flags` (`ACR_TLS`, `ACR_SC`, `ACR_RESEND`,
  `ACR_TLSLEGACY`) and `ResendLimit`.
- `write_amap` writes the column back. Before this change, `write_amap` dropped everything after
  column 4.
- When the first token holds options, `write_amap` writes it upper case in the canonical order
  `T L SC R…` (e.g. `sctlr5` → `TLSCR5`, `SCT` → `TSC`). Every letter is kept, also one that is
  ignored (`L` without `T`, `R0`, `R` on a wildcard entry). The rest of the line follows unchanged,
  and a column that is not an option token is written as read.
- `put_amap` takes an optional 6th item. With 5 items, the existing options are kept.
- `get_amap` / `get_amaps` pass the column as a 13th format argument, after the existing 12.
- Lua `get_amap(i)` returns it as a 5th value, only when the line has one.
- `dgate -m` lists the column.
- Map access goes through one lock in `amap.cpp`. `read_amap` builds the new map on the side and
  swaps it in at once.
- `GetACRNemaEntryByAE` returns one whole entry (address, port, compression, flags) from one map
  version. Connect sites use it, so that the address and the transport always come from the same
  entry.

### 3.2 `dicom.ini` keys (`[sscscp]`)

All times are in seconds. A value that is not a number or is out of range falls back to the
default, with a console warning (printed only while the feature is on).

| Key | Default | Range | Meaning |
|---|---|---|---|
| `OutgoingStorageCommitment` | `0` | 0/1 | master switch; 1 = the map's `SC` flags take effect |
| `OutgoingSCCheckIP` | `1` | 0/1 | an N-EVENT-REPORT must come from the numeric IPv4 address of the calling AE in the map (no name lookup); 0 allows host names |
| `OutgoingSCQuietTime` | `30` | 0.. | wait after the last send of a (destination, study) group before asking |
| `OutgoingSCMaxBatch` | `500` | 1..20000 | objects per N-ACTION; a larger group gets several transactions |
| `OutgoingSCSameAssocWait` | `5` | 0..3600 | wait for the report on the N-ACTION association; 0 = release at once |
| `OutgoingSCReportTimeout` | `3600` | 1.. | no report within this time after an accepted N-ACTION = failed attempt |
| `OutgoingSCRetries` | `5` | 0.. | re-asks after a failed attempt (5 = at most 6 N-ACTIONs) |
| `OutgoingSCRetryInterval` | `60,300,900,3600` | list | delay before re-ask n; one value or a list, the last value repeats |
| `OutgoingSCDeadline` | `86400` | 1.. | after first send + deadline the object becomes TIMEOUT (not for `RI`) |
| `OutgoingSCScanInterval` | `15` | 1..3600 | worker loop period |
| `OutgoingSCKeepDays` | `7` | 1..36500 | purge COMMITTED rows (by CommittedAt) and closed transactions |
| `OutgoingSCResendMissing` | `1` | 0..2 | for nodes with `R` only: 0 = off everywhere; 1 = re-send objects reported failed with a transient reason (0x0112, 0x0110, 0x0213); 2 = also objects that got no report after all re-asks |
| `OutgoingSCResendMax` | `3` | 0..999 | limit for plain `R` |
| `OutgoingSCResendDelay` | `300` | 0.. | delay before a re-send; after failed re-sends it doubles up to the last `OutgoingSCRetryInterval` value |
| `SCUnsafeFile` | `sc_unsafe.flag` | name | sentinel file (3.6); a name without a path is in the dgate directory |
| `OutgoingSCVerifyMax` | `4` | 1..64 | concurrent `sc_verify_now` verifications; one more answers `0 busy` |
| `OutgoingSCConnectTimeout` | `30` | 1..`TCPIPTimeOut` | worker only (N-ACTION requests and re-sends): one deadline from the start of the connect; TCP connect, TLS handshake and association setup (A-ASSOCIATE-RQ, A-ASSOCIATE-AC read) together end within it; a default above `TCPIPTimeOut` is lowered to it. All other connections keep `TCPIPTimeOut`. |

In `[lua]`:

| Key | Default | Meaning |
|---|---|---|
| `OutgoingSCStudyConfirmed` | empty | Lua run when a received report makes a study confirmed for its sender (3.8) |

The test harness uses some test-only keys. All default to 0 and are not for production:
`SCTestDelayBeforeWrite`, `SCTestDelayAfterRead`, `SCTestQueueMax`, `SCTestFailDemote`,
`SCTestFailFetch`, `SCTestDelayBeforeRecord`, `SCTestNoFileStamp`, `SCTestDelayVerifyStep`,
`SCTestDelayBeforeDelete`.

**Production note on `SCUnsafeFile`.** The sentinel and its `.running` marker must be on persistent
storage that survives a restart of dgate (for example the dgate directory on a host volume), never
on the writable layer of a container that is recreated: a lost sentinel would let "study
confirmed" answer 1 again without a reconciliation, and a lost running marker hides an unclean stop.

Changing a key needs a restart. Lowering a limit does not interrupt work in progress.

**The peer must also know dgate.** The destination must have this dgate's AE, IP and port in its own
configuration (with `TSC`, the TLS port), or a report on a new association cannot arrive. Such
objects end in TIMEOUT and raise an alarm.

### 3.3 Flow

1. **Noting.** After a C-STORE succeeds, the UIDs (taken just before `Write`, after any script or
   recompression), the called AE, the address, port and TLS mode actually used, and the file stamp
   of the local file the send read (3.4) are put into a bounded in-memory queue. There is no
   database access in the send path. This happens in four
   places:
   - ExportConverter forward (including a `dest` clause);
   - ImportConverter forward;
   - the C-MOVE SCP in `nkiqrsop.cpp` (not C-GET);
   - Lua `dicomstore`.

   `TestForward` is excluded.
2. **Worker** (one thread, started only with the switch on). Every `OutgoingSCScanInterval`, it:
   1. records queued sends in `SCObject` (state NOTED);
   2. abandons OPEN transactions past `OutgoingSCReportTimeout`, then re-asks or times out;
   3. turns NOTED or REQUESTED objects past `OutgoingSCDeadline` into TIMEOUT (`RI`: OVERDUE alarm);
   4. re-sends eligible objects (3.5);
   5. after `OutgoingSCQuietTime`, sends an N-ACTION per (destination, study), in chunks of
      `OutgoingSCMaxBatch`, on a new association that offers only the storage commitment class.
      TLS is used if the node has `T`; a T node never gets a plain-text request.

   **Time limits of the worker's own connections.** The request and re-send connections end their
   TCP connect, TLS handshake and association setup within `OutgoingSCConnectTimeout` (also for
   plain sockets; other dgate connections are unchanged). It is one absolute deadline from the
   start of the connect, not a limit per step or per read: every wait is cut to it, and once it has
   passed every read and write of that connection fails at once, so a destination that accepts the
   TCP connection but never sends A-ASSOCIATE-AC, or sends it byte by byte, is cut off at the
   deadline. Their release waits at most
   `OutgoingSCSameAssocWait` seconds, clamped to 2..10 s, for A-RELEASE-RP; without a reply the
   association is aborted (`*** Storage commitment: no release reply from <AE> within <n> s
   (<association>): association aborted`). A destination that could not be reached in a pass
   (request or re-send) is skipped for the rest of that pass for requests and re-sends, so one
   destination that does not answer costs at most one connect limit per pass and the others go on.
3. **Write first, then send.** The `SCTransaction` and `SCTxMember` rows and the objects' state
   REQUESTED are committed before the N-ACTION goes out. A restart resumes from the tables.
4. **TransactionUID.** It is built from the server UID root plus ≥ 64 random bits (`/dev/urandom`,
   `rand_s` on Windows), checked to be ≤ 64 characters, and new for every request.
5. **Result.** The worker waits up to `OutgoingSCSameAssocWait` on the same association. Reads are
   bounded by the remaining wait, and a watchdog shuts down only its own `dup` of the descriptor.
   The result may also come later on a **new association**, which `SCEventReportSupport` handles in
   `ServerChild`.
6. **Validation before any state change.** All of the following must hold:
   - event type 1 or 2;
   - the affected instance is the well-known one;
   - the TxUID is known;
   - the calling AE is the transaction's destination, connecting from its map address
     (`OutgoingSCCheckIP`);
   - 0008,1199 ∪ 0008,1198 is **exactly** the member set, with no duplicates and no overlap;
   - each SOP Class matches what was recorded.

   A report that fails any check is answered with an error and changes nothing. A valid result is
   committed in one database transaction **before** the success response is sent.
7. **Precedence and generations.**
   - COMMITTED from any transaction wins over a late FAILED.
   - Each object has a content generation `Gen`, and a result only applies to the generation it
     asked about.
   - A local rewrite of the same SOPInstanceUID makes the rows CHANGED (Gen+1) at once, and any
     send that started before that is stale.
   - `Gen` never wraps: it stops at 2·10⁹, and a row at that value is never asked about or confirmed.

### 3.4 State model and tables

Tables are created when missing (`CreateTable`), only with the switch on. They are never dropped by
regen. Upstream tables are not changed.

| Table | Content |
|---|---|
| `SCObject` | one row per (CalledAE, SOPInstUID): State, TxUID, Attempts, Resends, ResendRes, FirstSent, LastSent, NextTry, CommittedAt, FailReason, DestIP, DestPort, DestTLS (0 plain, 1 `T`, 2 `TL` legacy profile), MapGen, NoResend, Gen, FileStamp |
| `SCTransaction` | one row per N-ACTION: TxUID, CalledAE, SentAt, ClosedAt, State, Result, Members; not changed once closed |
| `SCTxMember` | (TxUID, SOPInstUID, SOPClassUID, Gen), written before the N-ACTION; Outcome (1 committed / 2 failed), Reason |
| `SCMarker` | overflow marker of earlier development builds; only read once and taken over by the sentinel (3.6) |

**Object states**

| State | Meaning |
|---|---|
| `NOTED` | sent, not yet asked about |
| `REQUESTED` | in an open transaction |
| `COMMITTED` | the destination confirmed this generation |
| `FAILED` | reported failed (FailReason = 0008,1197 or a local reason: re-send limit, not local) |
| `TIMEOUT` | re-asks exhausted or deadline passed |
| `UNSUPPORTED` | the destination did not accept the SC class; images were still sent normally |
| `HELD` | the map address, port or TLS of the destination changed since the send (also a switch between `T` and `TL`); nothing is sent to the new address |
| `CANCELLED` | map entry lost its `SC`, or `sc_cancel:` |
| `CHANGED` | the object was stored again locally after it was sent; not confirmed until sent again |

**File stamp (`FileStamp`).** `v2:size:mtime.ns:ctime.ns:inode` of the local file, taken by
the send path right before it reads the file and checked right after it (no stamp when the file
changed meanwhile). The change time (ctime) is set by the system on every write and cannot be set
back by user tools, so a rewrite that restores the modification time (`touch -d`, `cp -p`,
`rsync -t`) still changes the stamp. The device number is not part of it: it can change when a
file system is mounted again (a container restart on overlay storage, device-mapper numbering after
a reboot) and would then make every stamp differ after exactly the unclean restart the
reconciliation runs after (the verification still compares the device within one call, in its
fingerprint). Stamps detect ordinary rewrites; they are not proof against
deliberate tampering by a privileged user (who can set the system clock or write to the block
device). A content digest of the sent bytes was considered and not implemented (the send path
reads the file through the DICOM parser; hashing would mean reading every file a second time). A
stamp of an earlier build (`size:mtime.ns:inode`, without `v2:`) counts as no stamp: step 7 of
`sc_verify_now` never matches it and the reconciliation uses the whole-second rule. Forwards (ExportConverter), C-MOVE and re-sends record it; the ImportConverter forward of
the received object, Lua sends and the older note wrappers record none. A send that may change the
content records none either and sets NoResend: a `script` clause, an ExportConverter command
before the forward that may change the object (only `write`, `append`, `copy`, `save to`, the `if…`
and `between` tests, `prefetch`, `preretrieve`, `system`, `mkdir`, `rm`, `testmode`, `nop`, `stop`,
`silentstop`, `return`, `defer` and other forwards count as unchanging; `lua`, `call`, `save frame`,
`save bmp/gif/jpg` and the patient/study/series/image commands do not), a lossy or otherwise
changing compression (everything except as-is,
uncompressed and the lossless JPEG, JPEG 2000 and JPEG-LS modes), and for C-MOVE a
RetrieveConverter0 / RetrieveResultConverter0, scrub, downsizing or frame extraction. The stamp then
never certifies content the archive did not get. Only strong stamps are kept:
a system with nanosecond file times (Linux, macOS), a non-zero inode and non-zero nanosecond parts
of both times;
otherwise (e.g. Windows, file systems with whole-second times) the stamp is empty. A table of an
earlier build gets the column at the first start (`adding column FileStamp to table SCObject`,
existing rows empty).

**Transaction states:** `OPEN`, `CLOSED`, and `ABANDONED` (replaced by a newer request). A late
report to an ABANDONED transaction is still validated against that transaction's own members.

**Retries and re-sends are separate.**
- A *retry* (re-ask) is a new N-ACTION with a new TxUID for the same objects. It is governed by
  `OutgoingSCRetries`, `OutgoingSCRetryInterval`, `OutgoingSCReportTimeout` and
  `OutgoingSCDeadline`.
- A *re-send* is a new C-STORE of the local archive copy (3.5). It is governed by `R`/`Rn`/`RI`,
  `OutgoingSCResendMissing`, `OutgoingSCResendMax` and `OutgoingSCResendDelay`.

### 3.5 Re-send of missing objects (`R`, `Rn`, `RI`)

**When an object is re-sent**
- The node has `SC` and `R…`, `OutgoingSCResendMissing > 0`, and the entry is not a wildcard. This
  is checked when the re-send is reserved and again just before the C-STORE.
- The object was reported FAILED with 0x0112, 0x0110 or 0x0213. In mode 2 it may also be an
  object that got no report after all re-asks (a "silent loss").
- Permanent reasons such as 0x0122 and 0x0119 are never re-sent.

**How it is re-sent**
- The source is the local archive copy, found by SOPInstanceUID. If it is not found locally, the
  object becomes FAILED (not local) with an alarm.
- It goes to the **recorded** destination (AE, IP, port, TLS) with the map's compression. If the
  map entry differs now, the object becomes HELD.
- One association is used per (AE, study) batch.

**Accounting**
- `ResendRes` is incremented in the database **before** each C-STORE. A crash or a lost response
  therefore still uses up the limit.
- After a successful C-STORE the object returns to NOTED with `Attempts=0`. The normal cycle then
  asks again with a new TxUID.
- A re-send does not pass through the noting queue.

**NoResend.** If the copy that was sent may differ from the archive copy, the object is never
re-sent automatically. This covers:
- a forward `script` clause, `RetrieveConverter0` / `RetrieveResultConverter0`, or a C-MOVE script;
- scrub (9999,0202), downsizing (9999,0200) or frame extraction (9999,0201);
- lossy, NKI, clip or downsize compression;
- Lua `dicomstore`.

**RI**
- Re-sends and re-asks continue until the object is committed, past `OutgoingSCDeadline` and in
  modes 1 and 2.
- An overdue object gets a daily console alarm (`OVERDUE`) and is counted in `sc_status:`.
- `RI` stops only for: a permanent failure reason, no local file, HELD, `R` removed from the map,
  the switch set to 0, or `sc_cancel:`.

### 3.6 Fail-closed mechanisms

**Durable sentinel `SCUnsafeFile`**
- The sentinel is the single authority for "a COMMITTED row may be wrong". It is a file, not a
  database row, so a database outage cannot lose it.
- It is created, with a reason line synced to disk and a console ALARM, when:
  - a local overwrite could not be invalidated;
  - a send was dropped (queue full, unusable values, destination no longer `SC`);
  - sends were still unrecorded at a stop;
  - the previous run did not stop cleanly (the `<SCUnsafeFile>.running` marker was still present);
  - the running marker cannot be written.
- **While the sentinel exists, no study is confirmed** (`sc_confirmed:`, `sc_confirmed_list:`,
  `scconfirmed`), and the hook does not fire.
- `sc_unsafe_clear:` removes it. The command is refused while the queue is not empty, while a local
  write is in progress, or while the database is unreachable.
- Before removing the file, `sc_unsafe_clear:` reconciles every object that has rows not in
  CHANGED. The object is left as it is only when its current local file is known to be the one sent:
  - when every such row has a `FileStamp` and the current file has one: all these stamps equal the
    stamp of the current file;
  - otherwise (a row without a stamp): every such row has a `LastSent` before the modification time
    of the current file (whole seconds; a file written in the same second as its send counts as
    changed);
  - no usable local file: always reconciled.

  Every row of a reconciled object becomes CHANGED (Gen + 1, TxUID cleared, NoResend 1), whatever
  its state (also FAILED, TIMEOUT, HELD, CANCELLED, because a late result could still make them
  COMMITTED); only rows already CHANGED stay as they are. A reconciled object always needs a new
  send. A database fetch error while the rows are read refuses the clear, and the sentinel stays.
- SIGTERM, SIGINT and `quit:` stop the server cleanly.

**Write-in-progress marks.** A local rewrite is marked from the moment its rows are demoted until
its file is written. Sends that are noted or recorded meanwhile are stale. After the write, the rows
are demoted again synchronously.

**Queue window.** "Study confirmed" is 0 while a send of the study is still in the in-memory queue.

**Revalidation.** Before a positive "study confirmed" is returned, the code checks that the queue,
the write marks, the invalidation counter and the sentinel have not changed since its queries
started. If anything changed, it asks again, at most 3 times, and then answers 0.

**Regen.** `dgate -r` and `regen:` do not demote rows. `regenfile:` still does.

### 3.7 Logging

Console (OperatorConsole) lines are written for:
- every N-ACTION and its outcome, with study, transaction, number of objects and the attempt;
- every received result, and every refused one with the reason;
- UNSUPPORTED (once per AE), HELD, CANCELLED and TIMEOUT;
- every re-send;
- the daily OVERDUE alarm;
- sentinel ALARMs;
- queue overflow (one line per 1000 drops);
- `sc_verify_now` (3.9): one line per N-ACTION, one per result received or refused, and one summary
  line per verification:

Worker lines (a = attempt, b = its limit: `OutgoingSCRetries` + 1 for requests; for re-sends
`a/b (Rn)` from the map, `a/b (R, OutgoingSCResendMax)` from dicom.ini, `a/inf (RI)`, a range
`a1-a2/b` when the objects of a batch differ):

```
Storage commitment request to <AE>: study <StudyUID>, transaction <TxUID>, <n> object(s), attempt a/b
Storage commitment request accepted by <AE> (transaction <TxUID>, study <StudyUID>, <n> object(s), attempt a/b)
*** Storage commitment request to <AE> failed (study <StudyUID>, transaction <TxUID>, <n> object(s), attempt a/b): asked again later | requests used up
*** Storage commitment: connect to <AE> (<ip> <port>) timed out after <s> s, attempt a/b (transaction <TxUID>)
*** Storage commitment: could not connect to <AE> (<ip> <port>) for transaction <TxUID>, attempt a/b
*** Storage commitment: no release reply from <AE> within <s> s (<association>): association aborted
Storage commitment result from <AE> for <TxUID>: <c> committed, 0 failed (study <StudyUID>)
*** Storage commitment result from <AE> for <TxUID>: <c> committed, <f> failed (study <StudyUID>)
Storage commitment re-send to <AE>: study <StudyUID>, <n> object(s), attempt a/b (Rn)
*** Storage commitment: connect to <AE> (<ip> <port>) timed out after <s> s, re-send attempt a/b (Rn)
Storage commitment: re-sent <SOPInstanceUID> to <AE> (attempt a/b (Rn))
*** Storage commitment ALARM: <AE> changed in acrnema.map (sent to <ip> <port>[ TLS| TLS legacy], now <ip> <port>[ TLS| TLS legacy])
```

Verify lines:

```
Storage commitment verify: request to <AE>, transaction <TxUID>, <n> object(s)
Storage commitment verify: result from <AE> for <TxUID> on the <request|new> association: <c> committed, <f> failed
*** Storage commitment verify: result from <AE> for <TxUID> on the <request|new> association refused (<reason>): the verification fails
*** Storage commitment verify: study <StudyUID>, AE <AE>: <k> of <n> local object(s) not the content last sent and committed (<a> without a COMMITTED row, <b> with another file stamp, <c> local file(s) without a stamp, <d> sent with the content transformed): <changed since send|content transformed at send>
Storage commitment verify: study <StudyUID>, AE <AE>, <n> object(s), <k> chunk(s), transaction(s) <TxUID …|->, <s.mmm> s: <1|0 reason>
```

Lines starting with `***` are warnings, failures or alarms: every storage commitment line with
TIMEOUT, ALARM, failed, could not, timed out, refused or aborted starts with `***`.

### 3.8 Server commands, Lua and hook

All commands are guarded by `checkaccess(ca_script)`.

| Command | Answer |
|---|---|
| `sc_queue:` | `<queued> <dropped>`: noted sends not yet taken by the worker |
| `sc_status:` | per destination: objects per state, age of the oldest NOTED/REQUESTED; open transactions; re-send counts (resends, reserved, resend_queued, resend_limit, not_local, overdue); sentinel |
| `sc_confirmed:<StudyUID>[,<AE>…]` | `1` when **every** local object of the study (image table) is COMMITTED for **all** given AEs; with no AE given, for every SC destination the study was sent to. Otherwise `0` (also on error, with a console line). Computed live; there is no stored marker. |
| `sc_confirmed_list:<since>[,<AE>…]` | temp file, one StudyUID per line: studies with an object COMMITTED at or after `since` (epoch) that are fully confirmed now |
| `sc_cancel:<AE>[,<StudyUID>]` | objects in NOTED, REQUESTED, FAILED, TIMEOUT, HELD or CHANGED become CANCELLED; answers the count, or -1. A later result can still commit them. |
| `sc_unsafe:` | `0`, or `1 <first reason line>` |
| `sc_unsafe_clear:` | reconcile and remove the sentinel (see 3.6); `sc_overflow_clear:` is an alias |
| `sc_overflow:` | `0`, `1 since <time> dropped <n>`, or `1 marked: <reason>` |
| `sc_verify_now:<StudyUID>,<AE>[,<timeout>]` | `1`, or `0 <reason>` (3.9) |
| `sc_verify_and_delete:<StudyUID>,<AE>[,<timeout>]` | `1` when verified and deleted locally, or `0 <reason>` (3.9) |

Lua functions:
- `scconfirmed(studyuid [, ae, …])` returns `true` or `false`, or `nil` on error. It returns
  `false` when `OutgoingStorageCommitment=0`.
- `scverifynow(studyuid, ae [, timeout])` returns `true`, or `false, reason`. Default timeout 60 s.
  Inside the server it calls the verification directly; outside (e.g. `dgate --dolua:`) it sends
  `sc_verify_now:` to the server, where results on a new association arrive (no answer from the
  server: `false, "no answer from the server"`). Bad arguments: `false, "usage"`.
- `scverifyanddelete(studyuid, ae [, timeout])`: the same, through `sc_verify_and_delete:`; `true`
  only when the study was verified and deleted locally.

Hook: `[lua] OutgoingSCStudyConfirmed`
- It runs after the N-EVENT-REPORT response has been sent, once for each study that this result made
  confirmed for the calling AE.
- It runs in the Lua context of that association, with the globals `SCStudy`, `SCAE` and
  `SCObjects` set.
- It does not run while the sentinel exists.

### 3.9 Deleting studies in routing mode

> **`sc_confirmed:`, `sc_confirmed_list:`, `scconfirmed()` and the `OutgoingSCStudyConfirmed`
> hook are ADVISORY.** Use them for monitoring and to schedule work. **Do not delete on them.**
>
> Delete a study locally with `sc_verify_and_delete` (Lua `scverifyanddelete`), which verifies and
> deletes in one call, or only when `sc_verify_now` (Lua `scverifynow`) returned `1` **immediately
> before** the deletion.

Successive reviews kept finding edge cases where the persistent state can be wrong: a crash, a
database outage, a stop at the wrong moment, races with local rewrites. The fail-closed mechanisms
in 3.6 make these cases rare and safe-side, but deletion needs a stronger guarantee.

**What `sc_verify_now` does**
1. Reads the study's local objects from the image table, with a fingerprint of each file (name,
   size, modification and change time, inode, device).
   - No objects: answers `0 empty`.
   - A write in progress: answers `0 writing`.
2. Requires the AE to be a non-wildcard map entry with `SC`; otherwise `0 not an SC node`. It uses
   TLS if the entry has `T`, with all TLS checks.
3. For each chunk of ≤ `OutgoingSCMaxBatch` objects, registers a new random TransactionUID in memory
   only, opens an association (storage commitment + Verification) and sends an N-ACTION for exactly
   those objects.
4. Waits for the result, either on that association or on a new one, which `SCEventReportSupport`
   routes to the waiting call. The result is validated as strictly as in 3.3. No result before the
   timeout gives `0 no result`.
5. Every object of every chunk must be committed (event type 1, empty 0008,1198). Otherwise the
   answer is `0 failed <n>` or `0 invalid result`.
6. Re-reads the local objects and fingerprints. Any difference, a new object, or a write in
   progress gives `0 changed`.
7. Checks that the local content is the content the archive committed: for every local object
   there must be a COMMITTED `SCObject` row of that AE with NoResend 0 and a non-empty `FileStamp`
   equal to the stamp of the local file as read in step 1. A COMMITTED row with NoResend set (the
   send may have changed the content, see the file stamp in 3.4) gives `0 content transformed at
   send`. Otherwise `0 changed since send`: no COMMITTED row (never sent there, rewritten through
   dgate so the row is CHANGED, sent again but not yet committed, or purged after
   `OutgoingSCKeepDays`), another stamp (rewritten on disk after the committed send), or no stamp
   (a system without nanosecond file times, a send path that records none). This step only reads
   the table.

8. Checks the local side once more as in step 6 (a write during step 7, which reads the table and
   not the files, gives `0 changed`).

The answer is `1` only after all eight steps pass.

Step 7 makes the verification depend on the advisory `SCObject` table for one question only, "is
the archived content the current local content", and only in one direction: it can turn a `1` into
`0`, never a `0` into `1`; every condition of steps 1 to 6 applies unchanged. In practice: verify
(and delete) within `OutgoingSCKeepDays` of the commit, and send a study again through a forward or
C-MOVE when it was rewritten locally or sent by a path without a stamp.

**Properties**
- Nothing is written to SCObject, SCTransaction or SCTxMember, and the sentinel is neither read nor
  set.
- Other answers (any doubt answers `0`):

| Answer | Cause |
|---|---|
| `0 disabled` | `OutgoingStorageCommitment=0`; nothing is sent |
| `0 usage` | StudyUID or AE not valid, timeout not a number in 1..600 |
| `0 busy` | `OutgoingSCVerifyMax` verifications already running |
| `0 database error` | the local objects or the committed rows could not be read, including a fetch error (5) |
| `0 changed since send` | step 7: a local file is not the content last sent to that AE and committed |
| `0 content transformed at send` | step 7: the committed send of an object may have had other content than the local file (script, lossy or changing compression, C-MOVE processing) |
| `0 no local file` / `0 invalid local object` | an object of the study has no readable file, or a bad or repeated UID |
| `0 TLS not available for this node` | the entry has `T` and this build has no TLS: nothing is sent in plain text |
| `0 no connection` | no association to the node (also a failed TLS handshake or certificate check) |
| `0 storage commitment not accepted` | the node refused the storage commitment context |
| `0 request not sent` / `0 no response to the request` / `0 request refused` | the N-ACTION could not be sent, got no N-ACTION-RSP, or got an error status |
| `0 internal error (…)` | no random TransactionUID, out of memory |

- Timeout: default 60 s, 1..600. It covers all chunks, and it is the deadline of every request
  association too (connect, TLS handshake, N-ACTION, its response, the same-association wait and
  the release, which is aborted when the archive does not reply): an archive that stops answering
  at any point gives `0 no result` at the timeout and frees the `OutgoingSCVerifyMax` slot.
- One console line per verification (3.7), plus one per request and per result.
- What the archive confirms is its custody of these SOP Instance UIDs (PS3.4 Annex J). The archive
  does not compare content; step 7 compares the local file with the one last sent and committed.
- **Content guarantee.** A `1` means: the archive holds these UIDs, and each one was last sent to it
  from exactly the local file as it is now, byte for byte or through a lossless transfer syntax
  conversion, and that send was committed. A send that may have changed the content (a script on
  the sent copy, a lossy or otherwise changing compression, the per-object processing of a C-MOVE)
  is never taken as the same content, also not through NoResend: such a study answers `0 content
  transformed at send`. If the archived copy is meant to differ (for example anonymised for a
  research archive), the local copy is not a copy of it, and deleting on the commitment is a policy
  decision outside this check.
- An object stored after the answer is not covered by `sc_verify_now`; `sc_verify_and_delete`
  closes that window (below).

**`sc_verify_and_delete:<StudyUID>,<AE>[,<timeout>]`** (Lua `scverifyanddelete`) runs the same
verification and then deletes the study locally, as `deletestudy:` does (files and database
records), with the local writes of the study excluded:
- A guard for the study is set before the verification reads it. Every local write that starts
  (`SaveToDisk`, the path of every C-STORE and every rewrite through dgate) counts on it; a write
  whose study is not known, or whose SOP Instance UID is missing or longer than 64 characters,
  counts on every guard.
- After the answer `1`, in one locked step: when a write of the study started since the guard was
  set, or one is in progress, nothing is deleted (`0 changed`); otherwise the guard starts to block.
  A write of the study that starts now waits until the delete is done (console line `write of
  <uid> (study <uid>) waited for the verified delete of its study`); it is stored afterwards, as a
  new object of the study.
- The study is deleted and read again: no local object may be left.
- Answers: `1` (verified and deleted), every `0` of `sc_verify_now`, `0 changed` (a write of the
  study started during the verification), `0 write during delete` (a write waited longer than 60 s
  and went on: alarm line, check the study), `0 not deleted completely` (objects left after the
  delete: alarm line). The same `checkaccess` as the other commands; with
  `OutgoingStorageCommitment=0` the answer is `0 disabled` and nothing is deleted.

Residual limits: a change of a local file that does not go through a dgate write (a file edited or
replaced on disk outside dgate, a move between devices) is not excluded; it is only detected by the
fingerprints of steps 1, 6 and 8, so it can still happen between step 8 and the delete. The delete
itself is `DeleteStudy` (one file and record after the other), not a transaction: when it fails
halfway, objects are left and the answer is `0 not deleted completely`.

Example (not enabled anywhere): `lua/sc_delete_if_verified.lua` defines
`sc_delete_if_verified(patid, study, ae [, timeout])` (default 120 s), which calls
`scverifyanddelete` and prints the reason when the study was not deleted. In short:

```lua
local ok, why = scverifyanddelete(study, 'ARCHIVE', 120)
if ok ~= true then print('not deleted: ' .. tostring(why)) end
```

---

## 4. DICOM TLS

TLS lives in the `Socket` class (`dicomlib/socket.cxx`). The PDU and DIMSE code is unchanged. Plain
sockets keep their old code paths exactly. TLS is built only with `-DHAVE_OPENSSL` (OpenSSL ≥ 1.1.1).

**Outgoing**
- Every connect site that resolves a destination through the map uses TLS when the entry has `T`.
  This covers:
  - forwards and C-MOVE sub-associations;
  - Lua `dicomstore`, `dicomecho`, `dicomquery`, `dicommove`, `dicomget` and `dicomprint`;
  - virtual server, grab and echo;
  - the storage commitment event and worker.
- The TCP connect is bounded in TLS mode.
- Converter forwards keep their association open between objects. A kept association is reused
  only when its TLS mode (plain, strict, legacy) is what the map asks for now and, with TLS on
  either side, it goes to the same address, port and called AE with the same certificate AE.
  Otherwise it is closed and made again (console: `forward association closed by TRANSPORT` or
  `DESTINATION`), e.g. after a map reload added `T` to a node with an open plain association.
  Plain to plain is compared as before (association level and SOP class only).
- A failed handshake or certificate check is a failed connection. **There is never a plain
  fallback.**

**Incoming**
- A second listener runs on `TLSPort`. Each connection gets its own context (socket, peer IP,
  transport), so the plain port's hand-off is not shared.
- `SSL_accept` runs in the child thread, with a time limit, before the association is parsed.
- The DICOM port `TCPPort` is unchanged. There is no STARTTLS.

### 4.1 Keys (`[sscscp]`)

| Key | Default | Meaning |
|---|---|---|
| `TLSCertificate` | empty | our certificate (PEM, chain allowed); also sent as client certificate; required for `TLSPort` |
| `TLSPrivateKey` | the `TLSCertificate` file | its key (PEM) |
| `TLSCAFile` | empty | trusted CAs or peer certificates (PEM); needed unless `TLSVerifyPeer=ignore` or `TLSAcceptSelfSigned=1` |
| `TLSVerifyPeer` | `require` | `require` / `verify` / `ignore`. Incoming: client certificate required / checked when sent / not asked for. Outgoing: `require` = `verify`. |
| `TLSMinVersion` | `1.2` | `1.2` or `1.3`; TLS ≤ 1.1 is never used (strict profile) |
| `TLSCipherList` | built-in list | TLS 1.2 suites; can only **narrow** the default (ECDHE with AES-GCM or CHACHA20-POLY1305, RFC 9325); TLS 1.3 uses OpenSSL defaults |
| `TLSVerifyHostname` | `0` | 1 = the certificate SAN must match the node's host or IP in the map (outgoing) |
| `TLSAcceptSelfSigned` | `0` | 1 = accept self-signed peer certificates in both directions. Only the "self-signed" errors are forgiven; expired, not yet valid, bad signature, wrong purpose and host mismatch still fail. Logged once per AE. |
| `TLSNodeCertsFile` | empty | per-node certificate fingerprints (4.2) |
| `TLSPort` | empty | TLS listener port, read like `TCPPort`; empty = none; equal to `TCPPort` = startup error and no listener; port in use = clear console line |
| `TLSEnforceForMappedNodes` | `1` | with `TLSPort` or `TLSLegacyPort` set: a calling AE with `T` in the map is rejected on the plain port (A-ASSOCIATE-RJ) before any data; either TLS port satisfies it |
| `TLSLegacyPort` | empty | third listener with the legacy profile (4.3), read like `TCPPort`; works without `TLSPort`; equal to `TCPPort` or `TLSPort` = startup error line, no legacy listener |

The certificate, CA, verify, min-version, cipher and self-signed keys are read when the first TLS
context is made, so changing them needs a restart. `TLSVerifyHostname` is read for each connection.

### 4.2 Node certificates (`tlscerts.map`)

```
# AE          SHA-256 of the DER certificate (64 hex digits, colons optional), at most two
ARCHIVE2      3f9a...e1
MODALITY1     ab:cd:...:01, 77e0...9c        # two during a certificate change
```

**File format**
- A name without a path is in the dgate directory, like `acrnema.map`.
- Comments are `#`, `//` and `/* */`. AE titles are compared without regard to case.

**When an AE is listed**
- Its certificate must match one of the listed fingerprints: outgoing, the server certificate of
  that AE; incoming, the client certificate of that calling AE.
- With `TLSVerifyPeer=ignore`, a listed incoming AE is refused, because there is no certificate to
  compare.

**Loading**
- The file is read at startup and by the server command `read_tlscerts:`. It is built on the side
  and swapped in under a lock.

**Failure handling (fail closed)**
- A line that cannot be read, or an AE listed twice, makes that AE "certificate invalid", and all
  its TLS connections are refused.
- A file that cannot be read, or a damaged file (unterminated comment, NUL byte), keeps the
  previously loaded table. At startup that table is empty, and the console says so.

### 4.3 Profiles

**Strict profile** (`T`, `TLSPort`): PS3.15 Annex B.12 with the BCP 195 / RFC 9325 recommendations.
- TLS 1.2 with ECDHE + AES-GCM or ChaCha20-Poly1305 only.
- TLS 1.3 with the OpenSSL defaults.
- Nothing older than 1.2.

**Legacy profile** (`TL` outgoing, `TLSLegacyPort` incoming).
It exists for OsiriX-class stations that implement the older DICOM TLS profile.
- It is opt-in per node and per port, and uses separate client and server contexts.
- Protocol: TLS 1.0 to 1.2.
- Cipher suites: AES-CBC with SHA-1 only. The order is DHE, then ECDHE, then RSA key exchange:
  `DHE-RSA-AES256-SHA:DHE-RSA-AES128-SHA:ECDHE-RSA-AES256-SHA:ECDHE-RSA-AES128-SHA:AES256-SHA:AES128-SHA`.
- Never NULL, RC4, EXPORT, DES or 3DES.
- OpenSSL security level 0 is set in the legacy context only, for the protocol. Certificates are
  still verified at auth level 1.
- After every legacy handshake, the connection is refused if:
  - the ephemeral DH key is < 2048 bits or the EC key < 224 bits;
  - any certificate key in the chain is RSA/DSA < 2048 or EC < 224;
  - any certificate is MD5-signed;
  - any certificate is SHA-1-signed, unless the node is in `TLSNodeCertsFile` or
    `TLSAcceptSelfSigned=1` (an accepted SHA-1 signature is logged).
- `TLSMinVersion` and `TLSCipherList` do not apply. The certificates, `TLSCAFile`, `TLSVerifyPeer`,
  `TLSAcceptSelfSigned`, `TLSVerifyHostname` and `TLSNodeCertsFile` apply as for `T`.
- **Downgrade protection:** a calling AE whose map entry has `T` without `L` is rejected on
  `TLSLegacyPort`. `T` without `L` to a legacy-only receiver fails; it is never retried with the
  legacy profile. `TLSEnforceForMappedNodes` is satisfied by either TLS port.

**TLS log lines (both profiles).** Every TLS connection that passes its checks gets one line in the
normal dgate log, outgoing after the handshake and incoming after the calling AE checks:

```
TLS: connected to <AE> (<ip>): <protocol> <cipher>, <strict|legacy> profile, node certificate <matches TLSNodeCertsFile|not in TLSNodeCertsFile>
TLS: incoming from <AE> (<ip>): <protocol> <cipher>, <strict|legacy> profile, node certificate <…>
```

e.g. `TLS: connected to OSIRIX1 (192.0.2.40): TLSv1 AES128-SHA, legacy profile, node certificate not
in TLSNodeCertsFile`. A refused connection has no such line, only its `*** TLS: …` refusal line with
the reason: handshake failure (OpenSSL's reason), node certificate mismatch, legacy policy
(`*** TLS: legacy connection with <AE> (<ip>) refused: <reason>`), a T node on the plain port, or a
T node without L on the legacy port (`*** TLS: association from <AE> (<ip>) on TLSLegacyPort
rejected (<protocol> <cipher>): <AE> has T without L in acrnema.map, use TLSPort`).

**Interoperability note (OsiriX MD)**
- As measured, OsiriX MD negotiates only TLS 1.0 / 1.2 with AES-CBC suites, so it needs the legacy
  profile.
- Its default identity is a 1024-bit RSA certificate, self-signed. It is refused in one of two ways,
  and it must be replaced by a 2048-bit identity in both cases:
  - not trusted (not in `TLSCAFile`, not pinned, `TLSAcceptSelfSigned=0`): the handshake fails in
    the certificate check, and the line reads `certificate verification failed: self-signed
    certificate`;
  - trusted or pinned (in `TLSCAFile` or `TLSNodeCertsFile`, or `TLSAcceptSelfSigned=1`): the key
    size check after the handshake refuses it with `peer certificate RSA 1024 bit < 2048 (OsiriX
    default certificate? choose a 2048-bit identity)`.

### 4.4 Build without OpenSSL

- A node with `T`: the connection is refused with a console line ("built without TLS support").
  **Nothing is sent in plain text.** In particular a T node gets no plain C-STORE, no plain
  N-ACTION (objects stay NOTED), and no plain re-send.
- `TLSPort` or `TLSLegacyPort` set: a console line, and no listener.
- `read_tlscerts:`: a console line.
- Everything else is identical to the previous version.

### 4.5 Limits

- An AE title can be spoofed. The real binding of a node to its identity is `TLSNodeCertsFile`
  (fingerprint pinning), optionally together with `TLSVerifyHostname` (outgoing).
- For `SC` nodes without TLS, a storage commitment result is only as trustworthy as the network,
  even with the AE + IP + exact-set checks.

---

## 5. Databases

The new tables are made through Conquest's `Database::CreateTable` / `CreateIndex` when missing. The
SC worker opens its own `Database` connection.

| Database | Notes |
|---|---|
| SQLite (built-in) | tested |
| MySQL / MariaDB | tested; AE titles are compared with `CalledAE = BINARY '…'`, because the default collation is case-insensitive |
| PostgreSQL | tested |
| ODBC | uses the same layer; not tested |
| built-in dBase | outgoing SC is not available (console line); incoming SC is unchanged |

- Every UID that goes into SQL is checked to be digits and dots, ≤ 64 characters. AE titles are
  checked to be printable, ≤ 16 characters, with no quote or backslash.
- Tables from earlier development builds get the `Gen` and `FileStamp` columns added
  (`ALTER TABLE … ADD`).
- A fetch error (`Database::FetchError`) is told apart from the end of the rows wherever the
  reconciliation, `sc_unsafe_clear:`, `sc_verify_now:` and the confirmed check read rows: the clear
  is refused, and the verification answers `0 database error`.

---

## 6. Testing

The test harness lives outside this repository (Docker, synthetic data only, generated UIDs, a
synthetic patient). Suites were run against sqlite, MariaDB and PostgreSQL builds, plain and TLS
(DCMTK `storescp`/`storescu`/`echoscu +tls` and pynetdicom as peers), on Linux x86_64 and arm64
containers. Every commit keeps the compile warning count at the baseline.

| Area | Covered |
|---|---|
| Incoming SC | all present → type 1; unknown or missing-file instance → type 2 / 0x0112; request without list → 0x0120; unmapped AE → 0x0124; report on a new association with role negotiation; switch 0 → context refused |
| Map | every option form, case, R limits (0/5/999/I, invalid 1000/5X), R without SC, R on wildcard, numeric user column, extra columns; `get_amaps`, Lua `get_amap`, `put_amap` 5/6 items, `write_amap`/`read_amap` round trip byte-identical for 4-column maps |
| Outgoing SC | report on the same and on a new association; silent archive → re-asks with new TxUIDs → TIMEOUT; partial failure; SC class refused → UNSUPPORTED (images arrive); node without SC and switch 0 → no traffic, no tables; QuietTime grouping; MaxBatch chunking; restart between N-ACTION and report; TxUID format and randomness; `sc_status:` |
| Validation | wrong AE / IP, unknown TxUID, incomplete or foreign sets, duplicate report, late report to an abandoned transaction, older generation |
| Re-send | `SCR` (ini limit), `SCR5`, `SCRI` past the deadline (OVERDUE, then COMMITTED on recovery), 0x0122 not re-sent, not local, node without R, mode 0 and 2, map change → HELD, `sc_cancel:`, one row per (AE, object), NoResend cases |
| Confirmed | all committed → 1; one not → 0; object added → 0; two AEs; AE case on MariaDB; switch 0; hook fires once |
| Fail-closed | local overwrite after COMMITTED; queue not drained; overflow; DB failure during overwrite; unclean kill → sentinel; clean run → no sentinel; regen keeps COMMITTED; `sc_unsafe_clear:` reconciliation |
| TLS | outgoing and incoming with and without client certificate; wrong CA; node certificate match / mismatch / damaged file; hostname check; self-signed on/off; T node on the plain port rejected; `TLSPort`=`TCPPort`; never plain fallback; build without OpenSSL; connect timeout to a black-hole address; ini cache concurrency stress |
| Verify | archive holds all → 1; missing → `0 failed`; silent → `0 no result`; new association; wrong AE/IP; local change during the verify → `0 changed`; never sent, rewritten after the commit (through dgate and on disk) and no file stamps → `0 changed since send`, after a new send and its commit → 1; forward with a script clause and C-MOVE with RetrieveResultConverter0 → COMMITTED without a stamp, `0 content transformed at send`; TLS path, also the legacy profile; multiple chunks; `0 busy`; `0 disabled`; advisory tables unchanged; Lua in the server and from a client; reconciliation of every state; simulated fetch error |
| Worker limits | a destination whose packets are dropped: connect timeout with attempt line, the other destinations commit meanwhile; an archive that freezes after the N-ACTION: release aborted after the limit, others go on; file stamps recorded per send and compared by the reconciliation (untouched objects stay COMMITTED, rewritten ones and the whole-second fallback CHANGED, column added to an old table); log lines with attempts and `***`; `T` ↔ `TL` switch → HELD |
| Legacy TLS | legacy client on `TLSLegacyPort` accepted and on `TLSPort` refused; NULL/RC4/EXPORT/DES/3DES/anonymous offers refused on both ports; `TL` to a legacy-only receiver; `T` without L to it refused, never plain; node certificate mismatch and weak key refused; modern T node on the legacy port rejected; `L` without `T` warning; `TLSLegacyPort` = `TCPPort` / `TLSPort`; `TLSLegacyPort` alone; build without OpenSSL; map option parse/format unit test |

---

## 7. Known limitations and upstream observations

**Limitations of this change**
- The Windows GUI (`Serverdriver.pas`) "Save config" rewrites a map line with 4 columns. Since it
  rewrites the first matching line without checking the AE, it can drop the 5th column of that line.
- Comment handling in `acrnema.map` is unchanged: a comment after data on the same line follows
  the old parsing.
- Outgoing SC applies to forwards, C-MOVE and Lua `dicomstore`. It does not apply to C-GET or
  `TestForward`.
- Incoming SC reports the state at the moment the N-ACTION arrives. The status of the requester's
  N-EVENT-REPORT-RSP is not evaluated, as in the original code.
- Per-node SC timing keys and binding a certificate to an AE title (rather than a fingerprint) are
  not implemented.

**Upstream observations**
1. **`AllowOnlyMappedRemoteAE` / `AllowOnlyMappedLocalAE` / `CheckIPofRemoteAE` are inactive.**
   - `flpdu.hpp` declares `virtual BOOL ShouldIAcceptRemoteMapped(BYTE *)` (and `…LocalMapped`).
   - `ExtendedPDU_Service` in `nkiqrsop.hpp` defines `ShouldIAcceptRemoteMapped(BYTE *A, int ip)`.
   - Because the signatures differ, this hides the virtual function instead of overriding it.
     `flpdu.cxx` calls the base version, which always accepts, so the keys have no effect.
   - This change does not alter it. The new `ShouldIAcceptRemoteTransport` hook uses a matching
     signature.
   - The N-ACTION refusal of commit `edadb73` does not rely on these keys. `StorageCommitmentSupport`
     looks up the calling AE with `GetACRNema` for every N-ACTION it reads (only with
     `EnableStorageCommitment=1`), whatever the keys say, and answers 0x0124 when no entry matches.
     It follows `GetACRNema` matching (an entry whose name ends in `*` matches by prefix, then exact,
     then case-insensitive), so a catch-all wildcard entry admits every calling AE, and it does not
     compare the source address. That commit's message describes `AllowOnlyMappedRemoteAE` as
     rejecting such an association earlier; with the keys inactive that never happens, and this
     check is the only acrnema.map check of the calling AE of an N-ACTION (on the TLS ports the TLS
     checks of section 4 come on top).
2. **gpps ini cache (`gpps.cpp`): fixed here** (`3bba371`, `b094da6`, `9074846`).
   - A cache hit was copied with an unbounded `strcpy` (up to 1023 characters, whatever the caller's
     buffer length was). It is now copied bounded, under the lock.
   - Entries that do not fit the 64/64/1024 arrays are not cached.
   - The mutex is initialised once by a static object, and cache state and statistics are kept under
     it.
   - Results for values that fit are byte-identical to before.
3. **Fixed in passing:**
   - `write_amap` dropped columns after the 4th.
   - `put_amap` with a 6th item wrote `"un,xx"` into the compression field.
   - The incoming SC event on a new association was sent without its dataset.
4. **Not changed (decision: plain path stays as it is):** the plain `Socket::SendBinary` returns TRUE
   after a partial `send`. Only the TLS path has a full write loop.

---

## 8. Commit map (`f8c0057` to the end of this series)

**Incoming storage commitment (variant A)**
- `3183c6e` Storage commitment SCP behind dicom.ini EnableStorageCommitment (default 0)
- `edadb73` Storage commitment: refuse N-ACTION from a calling AE not in acrnema.map

**acrnema.map**
- `e713b29` acrnema.map: access under a lock, reload swaps in the new map in one step
- `a194ccd` acrnema.map: optional 5th column with per-node options (T, SC, R, Rn, RI)

**Outgoing storage commitment**
- `6fe440c` Outgoing storage commitment, part 1: state tables and N-EVENT-REPORT handling
- `747b65f` Outgoing storage commitment, part 2a: note successful sends to SC nodes
- `111680b` Outgoing storage commitment, part 2: worker thread, N-ACTION, retries
- `3c34620` Outgoing storage commitment, part 3c: "study confirmed" check, commands, Lua function and hook
- `903a4f5` Outgoing storage commitment, part 3b: re-send of missing objects (R, Rn, RI)
- `6f9aaca` Outgoing storage commitment: never re-send an object whose sent copy may differ

**DICOM TLS**
- `9677647` Outgoing DICOM TLS to nodes with T in acrnema.map
- `c506233` Incoming DICOM TLS on a separate, configurable port (TLSPort)
- `e4871c3` TLS: dicom.ini TLSAcceptSelfSigned (default 0) for self-signed peer certificates
- `a896f77` TLS: node certificates in a separate file (TLSNodeCertsFile, read_tlscerts:)

**Fixes after review: storage commitment**
- `d5e780f` Outgoing storage commitment: content generation, local overwrite, queue window
- `fa33421` Outgoing storage commitment: record the destination actually used by each send
- `a005869` Outgoing storage commitment: NoResend for every way the sent copy can differ
- `9449442` Outgoing storage commitment: RI nodes keep cycling in re-send mode 1 too
- `1c160ab` Outgoing storage commitment: deadline also ends objects waiting in REQUESTED
- `5315224` Outgoing storage commitment: bound the reads of the same-association wait
- `dd8a528` Outgoing storage commitment: build the TransactionUID with bounded buffers
- `07017d7` Outgoing storage commitment: no recorded send outlives a later local write
- `0489851` Outgoing storage commitment: the watchdog shuts down only its own descriptor
- `51882d8` Outgoing storage commitment: no noted send is lost silently
- `97e1ba3` Outgoing storage commitment: regen keeps commitment rows; Gen never wraps
- `523ecfd` Outgoing storage commitment: C-MOVE send sequence taken per object
- `c8f9552` Outgoing storage commitment: durable "commitment unsafe" sentinel file
- `f013037` Outgoing storage commitment: a positive confirmation is checked again before it is given
- `c63988a` Outgoing storage commitment: the watchdog's duplicate is never descriptor 0

**Fixes after review: TLS and transport**
- `bad909e` Outgoing TLS: one acrnema.map lookup per connection (address and transport together)
- `589f1ec` Outgoing TLS: bounded TCP connect (TLS mode only)
- `d706b0f` Outgoing TLS: unbound-node warning list under a lock
- `3a145ee` TLS: verify mode per connection, server context made under the context lock
- `b6b4e08` TLS: own review of the socket layer (plain paths, SAN check, OpenSSL versions)
- `e344ec7` Incoming TLS: one canonical calling AE form on both ports; cache-safe TLS ini reads
- `1c88a14` TLS: a damaged node certificate file never removes certificates
- `7319011` TLS: fail the connection when the socket cannot be made non-blocking
- `5c2f563` C-MOVE: the sub-association transport only from the QualifyOn entry

**Ini cache (upstream issue)**
- `3bba371` Ini cache: bounded copies in gpps.cpp; TLSIni reads the section name with 64 bytes
- `b094da6` Ini cache: one lock made once; cache state and statistics under it
- `9074846` Ini cache: the status display reads the gpps statistics under the cache lock

**Build identification**
- `f5ba9e8` Build variant line at startup (SC-in SC-out [TLS]); on the variant A branch, `c81c14d`
  Build variant line at startup (SC-in)

**Verify before delete**
- `68fc2f4` Outgoing storage commitment: sc_verify_now: and Lua scverifynow, a fresh check before a local delete
- `89e4ba8` Outgoing storage commitment: the reconciliation of sc_unsafe_clear: covers every state
- `cf41be8` Outgoing storage commitment: a fetch error is told from the end of the rows

**Legacy TLS profile**
- `91f1507` Outgoing TLS legacy profile: map option L (TL) for OsiriX-class nodes
- `8712265` Incoming TLS legacy profile: TLSLegacyPort listener for OsiriX-class callers
- `a80ef64` TLS legacy profile: certificate checks at level 1 and a post-handshake key/signature policy
- `695d43e` TLS log: one operator console line per TLS connection, strict and legacy
- `30c7444` acrnema.map header: describe the 5th-column options (T, L, SC, R, Rn, RI)

**Documentation**
- `627fe7c` Docs: storage commitment (incoming/outgoing) and DICOM TLS
- `cca3b99` Docs: resolve pending items after merge

**Worker limits and file stamps**
- `f2c6806` Outgoing storage commitment: connect time limit for the worker
- `14910e4` Outgoing storage commitment: the worker releases its associations with a time limit
- `5e9301d` Outgoing storage commitment: file stamp per send, compared by the reconciliation
- `f8b0a32` Outgoing storage commitment: log lines with *** on failures and attempt numbers
- `2494f20` Outgoing storage commitment: sc_verify_now: also checks that the local content is the content committed
- `bdbee38` Outgoing storage commitment: DestTLS 2 for sends with the TLS legacy profile
- `2d4ff41` Comments: remove development-process markers
- `52c13f4` Docs: worker limits, file stamps, verify step 7, upstream AE options

**Content guarantee, verified delete, deadlines**
- `31e7080` Outgoing storage commitment: a send that may change the content certifies nothing
- `53e6874` Outgoing storage commitment: check again after step 7; sc_verify_and_delete: with local writes excluded
- `0bfb7b5` Outgoing storage commitment: file stamp v2 with the change time
- `cfa5d22` Outgoing storage commitment: one deadline for the worker's connect and association setup
- this commit: Outgoing storage commitment: sc_verify_now: keeps its timeout when the archive stops answering
