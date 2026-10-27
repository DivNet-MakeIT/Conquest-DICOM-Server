# Building and running dgate on Windows

This page covers the Windows builds of branch `sc-outgoing`: storage commitment (incoming and
outgoing) on top of upstream `f8c0057` (1.5.0g4), without DICOM TLS. The main document is
[StorageCommitment.md](StorageCommitment.md).

## 1. What was built and run

All runs used synthetic data on a Windows 11 25H2 test machine (ARM64; the x64 builds ran under
the Windows x64 emulation). The peer was a Linux dgate of this series (archive, storage
commitment SCP), plus pynetdicom. The runs were made with builds of `sc-outgoing-tls`, whose
storage commitment code this branch has unchanged; the table shows its plain (non-TLS) builds.

| Build | Compiler | Variant | Result |
|---|---|---|---|
| `src/dgate/amake.bat` (the repository's own scripts) | Visual Studio 2022 (14.44), x64 | plain | builds with the settings of 2.2; all checks of section 5 pass |
| the same flags, one script | Visual Studio 2022, x64 | plain | all checks of section 5 pass |
| the same flags, one script | Visual Studio 2022, ARM64 native | plain (Lua linked in) | all checks of section 5 pass |
| `build/windows-mingw` (Linux host) | mingw-w64 gcc 12, x64 | plain | all checks of section 5 pass |
| upstream `install64/dgate64.exe` | (prebuilt) | plain | control: C-ECHO, C-STORE, forward, C-MOVE |

The Intel 12.1 / Visual Studio 2010 toolchain named in `AsetCompiler64.bat` was not available.
32-bit builds were not tried.

## 2. Native build with `amake.bat`

### 2.1 Unchanged requirements (upstream)

- `amake.bat` in `src/dgate` builds the libraries (charls, dicomlib, jpeg-6c, lua, luasocket,
  openjpeg, sqlite3) and then `src/Amake.Bat` builds `dgate64.exe` and `dgate.exe`. The compiler
  comes from `AsetCompiler64.bat` / `AsetCompiler32.bat` (`compiler`, `ccompiler`, `include`,
  `lib`).
- Defines: `-DWIN32 -DWINDOWS -DNATIVE_ENDIAN=1 -DUSESQLITE -DUSEMYSQL -DPOSTGRES -DHAVE_LIBJPEG
  -DHAVE_LIBCHARLS -DHAVE_LIBOPENJPEG2`, plus `-DLUA51EXTERN` for 64 bit (Lua is loaded from
  `lua5.1.dll` at run time).
- `-DUSEMYSQL` includes `C:\mysql-5.0.22-win32\include\mysql.h` (`odbci.hpp`). Any MySQL or
  MariaDB client headers placed there work (the client DLL is loaded at run time); without them,
  remove `-DUSEMYSQL`.
- The script ends with `del build32\*.*` and `del build64\*.*`, which ask for confirmation (answer
  `Y`, or pipe `Y` lines into `amake.bat` in an unattended run).

### 2.2 Visual Studio 2015 and later

With a current Visual Studio, three things are needed. None of them is new in this series; the
first is fixed in the source here, the others are settings:

1. `odbci.cpp` defined `_errno` as `errno`. With the Universal CRT, `errno` is `(*_errno())`, so
   every later `errno` failed to compile (`errno: identifier not found` in `<xlocnum>`). Fixed in
   commit `odbci.cpp: no #define _errno errno` (also needed by mingw-w64).
2. Add `legacy_stdio_definitions.lib` to `set libs=` in `src/Amake.Bat`, as its header comment
   says. Without it, `odbccp32.lib` does not link (`_vsnwprintf_s`).
3. `AsetCompiler64.bat` for a Visual Studio command prompt (the prompt already sets `INCLUDE`,
   `LIB` and `PATH`):

   ```bat
   set compiler=%VCToolsInstallDir%bin\Hostx64\x64
   set ccompiler=cl
   ```

   On an ARM64 Windows host, use `Hostarm64\x64` and open the prompt with
   `vcvarsall.bat arm64_x64`.

`luasocket` does not link without `lua5.1.64.lib` / `lua5.1.32.lib`. This only matters for the
32-bit `dgate.exe`, which links Lua in.

### 2.3 ARM64 native (optional)

Visual Studio's ARM64 compiler builds the same sources unchanged. One difference: an ARM64
process cannot load the x64 `lua5.1.dll`, so Lua is linked in. The 64-bit build then leaves out
`-DLUA51EXTERN` and adds `lua_5.1.5\all.c`, as the 32-bit build does.

## 3. Cross build with mingw-w64 (Linux or macOS host)

`build/windows-mingw` has a Dockerfile (Debian 12, mingw-w64 gcc 12) and `build-mingw.sh`. The
script uses the defines and libraries of the 64-bit `amake.bat`. It takes the source and the
output directory (`build-mingw.sh /src /out`) and writes `dgate64.exe`.

```sh
docker build --platform linux/amd64 -t dgate-mingw build/windows-mingw
docker run --rm --platform linux/amd64 -v "$PWD":/src:ro -v "$PWD/out":/out dgate-mingw build-mingw.sh /src /out
```

Notes:
- `-O0`, as `maklinux` (no `-O`). A g++ `-O2` build of upstream `8e77e06` crashes on the first
  C-ECHO (in `CEchoRSP::Write`); MSVC `/O2` does not. This was not investigated further.
- `-fpermissive`: g++ refuses a few conversions that MSVC accepts; they remain warnings.
- Two Windows-only include spellings need a shim on a case-sensitive file system, made by the
  script: `<sys\stat.h>` and `"C:\mysql-5.0.22-win32\include\mysql.h"`. They take the host's
  MariaDB client headers.
- The result is static (`-static`); only Windows system DLLs are imported.

## 4. Running

- Prebuilt: `install64/dgate64.exe` (commit "Windows binaries: dgate64.exe", last on this branch;
  sha256 sum and build commands in its message). It needs only `lua5.1.dll` from `install64` and
  Windows system DLLs. `install32/dgate.exe` is upstream's and was not rebuilt.
- Files next to `dgate64.exe`:
  - `lua5.1.dll` from `install64` (x64 builds with `LUA51EXTERN`);
  - `dicom.ini` and `acrnema.map`;
  - `dgate.dic`;
  - `dgatesop.lst`, which needs the Storage Commitment Push Model line for incoming SC and for
    outgoing SC (the result comes on a new association, section 5);
  - `dicom.sql`.

  Then `dgate64.exe -v -r` once, and `dgate64.exe -v` to serve.
- Server commands work as on Linux: run `dgate64.exe --sc_status:` in the same directory.
- Open the firewall for `TCPPort`.
- With the Windows GUI (`ConquestDICOMServer.exe`), read section 6 first.

## 5. Windows behaviour and limits

These follow from the code and were checked on the test machine.

- **No same-association wait.** The worker and `sc_verify_now:` read the result on the N-ACTION
  association only under a watchdog. That watchdog works on its own duplicate of the socket, and
  a socket cannot be duplicated with winsock 1, so on Windows nothing is read there. The console
  shows `*** Storage commitment: no watchdog for the same-association wait (<AE>): association
  closed without reading, the result must come on a new association`. The archive must therefore
  be able to open an association back to dgate:
  - a map entry for dgate's AE at the archive;
  - the firewall open for it.

  The result is then handled as on Linux (`COMMITTED`).
- **No file stamps.** Windows `stat()` has no nanosecond times, so `FileStamp` stays empty.
  Consequences:
  - `sc_verify_now:` answers `0 changed since send` for every study, even when the archive confirms
    all objects;
  - `sc_verify_and_delete:` never deletes;
  - `sc_confirmed:` and the outgoing SC states are not affected.

  Deleting by verification is therefore not available on Windows.
- **Release.** The worker closes its N-ACTION association without reading, A-RELEASE or A-ABORT
  (the console line above, or `no watchdog for the release write` when it gets that far). Per the
  code, a verification's connection (with `Deadline`) writes its release bounded by the deadline.
  The archive side of that release was not observed separately here.
- **Plain sends under a deadline.** The socket is made non-blocking for the send (`FIONBIO`) and
  waits with `select()`. Checked: `sc_verify_now:` to a peer that accepts the association and then
  never reads, with an N-ACTION of 2002 objects and a timeout of 15 s, answers `0 no result` after
  15 s, and the server answers again at once.
- **select() instead of poll().** Every wait puts one socket into its own `fd_set`. On Windows,
  `fd_set` is a list, not a bitmap, so the descriptor number does not matter.
- `long` is 32 bits on 64-bit Windows: the `Deadline` arithmetic in `socket.cxx` (`long`) is
  valid until 2038.
- **Wine.** Wine is not a reference. The upstream prebuilt exe and the `-O0` mingw builds run
  there, but command output and some exceptions behave differently. Use a real Windows machine.

## 6. Windows GUI (`ConquestDICOMServer.exe`)

The Windows distribution ships a prebuilt GUI. It starts `dgate64.exe` from its own folder and
writes `dicom.ini`, `acrnema.map` and `dgatesop.lst` itself. This series changes the GUI source
(`src/gui/Serverdriver.pas`), but the prebuilt `ConquestDICOMServer.exe` is older than these
changes. The GUI is a Delphi VCL project for Embarcadero RAD Studio (`src/gui/amake.bat`,
`ConquestDICOMServer.dproj`). It uses VCL, ADODB, DBGrids, `System.RegularExpressions` and the ICS
sockets, so it cannot be built with Free Pascal or Lazarus; it has not been rebuilt here. Until it
is rebuilt, 6.1 to 6.3 apply.

### 6.1 `dicom.ini`

- The GUI has no fields for the keys of this series. Edit `dicom.ini` with a text editor
  (Notepad), inside `[sscscp]`: anywhere after the `[sscscp]` line and before the next `[...]`
  line. Keys in `[lua]` (e.g. `OutgoingSCStudyConfirmed`) go in `[lua]`.
- **The prebuilt GUI drops these keys.** "Save configuration" (Configuration page) rewrites
  `dicom.ini` from its fields (`SaveConfigButtonClick`: `Rewrite`, then one line per field). Every
  `[sscscp]` key without a field is lost: all keys of this series, also `EnableStorageCommitment`.
  Other sections are kept as they were. The GUI then kills and restarts dgate, so the server runs
  without them at once. The same save runs from "Make ODBC data source" (Installation page) and
  at a new installation.
  - Effect: storage commitment is off in both directions.
  - After every "Save configuration": open `dicom.ini` again, add the keys again, then "Kill and
    restart the server" (Server status page).
  - Most of these keys are read at startup only (section 3.2 of
    [StorageCommitment.md](StorageCommitment.md)); restart dgate after an edit.
- The GUI reads `dicom.ini` only at its start. An edit made while the GUI is open is kept until
  the next "Save configuration", which writes the GUI's values of its own fields.
- `dgate64.exe --put_param:Key,value` (run in the dgate folder while the server runs) replaces a
  key that is already in the file. A key that is not there yet is appended at the **end of the
  file**, i.e. in the last section, which in a GUI-written `dicom.ini` is usually not `[sscscp]`
  (checked: it landed in `[lua]`). Use it only for keys that are already present.
- **Rebuilt GUI (this source):** "Save configuration" keeps the `[sscscp]` lines that start with
  `OutgoingSC`, `OutgoingStorageCommitment`, `EnableStorageCommitment`, `SCUnsafeFile` or
  `SCTest`, unchanged, in a block headed `# Storage commitment (no fields in this GUI; kept as
  found)` after the GUI's own keys. A `dicom.ini` without such keys is written as
  before.

Block to paste (the defaults; only the first lines are needed to switch the features on):

```ini
# Storage commitment (StorageCommitment.md, sections 2 and 3.2)
EnableStorageCommitment   = 0
OutgoingStorageCommitment = 0
OutgoingSCCheckIP         = 1
OutgoingSCQuietTime       = 30
OutgoingSCRetries         = 5
OutgoingSCRetryInterval   = 60,300,900,3600
OutgoingSCDeadline        = 86400
OutgoingSCResendMissing   = 1
OutgoingSCResendMax       = 3
OutgoingSCConnectTimeout  = 30
```

### 6.2 `dgatesop.lst`

- A `dgatesop.lst` made by the prebuilt GUI has no storage commitment class. Add this line after
  the `Verification` line (tabs or spaces), then restart dgate:

  ```
  StorageCommitmentPushModel	1.2.840.10008.1.20.1		sop
  ```

  Without it, incoming SC is refused, and so is the result of outgoing SC, which on Windows always
  comes on a new association (section 5). With `EnableStorageCommitment` and
  `OutgoingStorageCommitment` both 0, dgate refuses the class even when it is listed.
- Switching "Enable Explicit and JPEG transfers" (Configuration page) and answering Yes to
  "Update DGATESOP.LST?" writes the file again and removes the line; add it again.
- **Rebuilt GUI:** a new `dgatesop.lst` has the line.

### 6.3 `acrnema.map`

- The GUI writes its header only when there is no `acrnema.map` (new installation). The prebuilt
  GUI's header does not describe the 5th column. An existing map is never given a new header.
- **Editing in the GUI:** page "Known DICOM providers" is a text editor for the map. "Save this
  list" saves the text as it is and sends `read_amap:` to dgate, which loads the 5th column at
  once (no restart). The description block can be pasted there, inside the `/* ... */` comment;
  section 3.1 of [StorageCommitment.md](StorageCommitment.md) has the options.
- **`dgate64.exe --write_amap:`** (in the dgate folder, while the server runs; checked on Windows
  11) rewrites the map from the server's memory with dgate's header, which always describes the
  options. It drops every comment and the GUI's header, writes the columns with spaces, and the
  "Written by write_amap on" line has no time (upstream: that line prints a variable that is not
  set there). "Restore this list" shows the new file in the GUI.
- "Save configuration" replaces the first map line that has no `*` and is longer than 10
  characters with `<MyACRNema>  127.0.0.1  <TCPPort>  un` (upstream). Keep the server's own entry
  as the first line after the header comment, without options, and put no comment line without
  `*` above it.
- **Rebuilt GUI:** the header of a new map has the options block, identical to the one written by
  `write_amap`. The compression lines of the GUI header stay as they were.

Example:

```
/* **********************************************************
 * ... (header as written by write_amap or the rebuilt GUI)  *
 * Options (optional 5th column, letters in this order):    *
 *   T   = node needs DICOM TLS: this dgate has no TLS,     *
 *         every association to it is refused (L = with     *
 *         T: legacy TLS profile, refused the same way)     *
 *   SC  = storage commitment for data sent to this node    *
 *   R   = re-send missing objects, limit from dicom.ini    *
 *         (OutgoingSCResendMax); R needs SC                *
 *   Rn  = re-send at most n times (n = 1..999), e.g. R5    *
 *   RI  = re-send until committed (I = infinite)           *
 * Examples: SC  SCR5  SCRI                                 *
 * No options = as in previous versions.                    *
 ********************************************************** */

CONQUESTSRV1     127.0.0.1      5678    un
ARCHIVE          192.0.2.20     104     un      SCR
ARCHIVE2         192.0.2.21     2762    un      SCR5

V*               *              1234    un
W*               *              666     un
S*               *              5678    un
```
