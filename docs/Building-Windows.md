# Building and running dgate on Windows

This page covers the Windows builds of this series: storage commitment (incoming and outgoing)
and DICOM TLS on top of upstream `f8c0057`. The main document is
[StorageCommitment-and-TLS.md](StorageCommitment-and-TLS.md).

## 1. What was built and run

All runs used synthetic data on a Windows 11 25H2 test machine (ARM64; the x64 builds ran under
the Windows x64 emulation). The peer was a Linux dgate of this series (archive, storage
commitment SCP, TLS), plus pynetdicom.

| Build | Compiler | Variant | Result |
|---|---|---|---|
| `src/dgate/amake.bat` (the repository's own scripts) | Visual Studio 2022 (14.44), x64 | plain and TLS (`OPENSSLDIR`) | builds with the settings of 2.2; all checks of section 5 pass |
| the same flags, one script | Visual Studio 2022, x64 | plain, TLS | all checks of section 5 pass |
| the same flags, one script | Visual Studio 2022, ARM64 native | plain (Lua linked in) | all checks of section 5 pass |
| `build/windows-mingw` (Linux host) | mingw-w64 gcc 12, x64 | plain, TLS | all checks of section 5 pass |
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

### 2.3 TLS build (optional)

The TLS build is optional and needs OpenSSL 1.1.1 or later for the same compiler. The default
`amake.bat` run is unchanged.

- Set `OPENSSLDIR` before `amake.bat`. It then also runs `src\AmakeTLS.Bat`, which builds
  `src\dgate\dgate64tls.exe`. Without `OPENSSLDIR`, nothing extra runs.
- Expected layout: that of the Shining Light installer (`Win64OpenSSL-3_x.exe`, tested with
  3.5.9): `%OPENSSLDIR%\include\openssl\*.h`, `%OPENSSLDIR%\lib\VC\x64\MT\libssl_static.lib` and
  `libcrypto_static.lib`. For another layout, set `ssllibs` to the two libraries.
- Compared with the plain 64-bit build:
  - one define: `-DHAVE_OPENSSL`;
  - one include directory: `%OPENSSLDIR%\include`;
  - libraries: OpenSSL plus `ws2_32.lib crypt32.lib`.
- With Visual Studio 2015 and later, also set `EXTRALIBS=legacy_stdio_definitions.lib` (see 2.2).
- Static OpenSSL with `/MT`, as the rest of `amake.bat`, needs no OpenSSL DLL next to the exe.
  When linking the DLL import libraries (`libssl.lib`, `libcrypto.lib`) instead, ship
  `libssl-3-x64.dll` and `libcrypto-3-x64.dll` with the exe.

```bat
rem Visual Studio 2022 x64 prompt, AsetCompiler64.bat as in 2.2
set OPENSSLDIR=C:\OpenSSL-Win64
set EXTRALIBS=legacy_stdio_definitions.lib
cd src\dgate
amake.bat
rem -> dgate64.exe (plain), dgate64tls.exe (TLS)
```

### 2.4 ARM64 native (optional)

Visual Studio's ARM64 compiler builds the same sources unchanged. One difference: an ARM64
process cannot load the x64 `lua5.1.dll`, so Lua is linked in. The 64-bit build then leaves out
`-DLUA51EXTERN` and adds `lua_5.1.5\all.c`, as the 32-bit build does. Tested plain only; TLS
needs an ARM64 OpenSSL.

## 3. Cross build with mingw-w64 (Linux or macOS host)

`build/windows-mingw` has a Dockerfile (Debian 12, mingw-w64 gcc 12, OpenSSL 3.0.15 built static
for `mingw64`) and `build-mingw.sh`. The script uses the defines and libraries of the 64-bit
`amake.bat`.

```sh
docker build --platform linux/amd64 -t dgate-mingw build/windows-mingw
docker run --rm --platform linux/amd64 -v "$PWD":/src:ro -v "$PWD/out-plain":/out dgate-mingw build-mingw.sh /src /out plain
docker run --rm --platform linux/amd64 -v "$PWD":/src:ro -v "$PWD/out-tls":/out dgate-mingw build-mingw.sh /src /out tls
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

- Files next to `dgate64.exe`:
  - `lua5.1.dll` from `install64` (x64 builds with `LUA51EXTERN`);
  - `dicom.ini` and `acrnema.map`;
  - `dgate.dic`;
  - `dgatesop.lst`, which needs the Storage Commitment Push Model line for incoming SC;
  - `dicom.sql`.

  Then `dgate64.exe -v -r` once, and `dgate64.exe -v` to serve.
- Server commands work as on Linux: run `dgate64.exe --sc_status:` in the same directory.
- Open the firewall for `TCPPort`, `TLSPort` and `TLSLegacyPort`.

## 5. Windows behaviour and limits

These follow from the code and were checked on the test machine.

- **No same-association wait.** The worker and `sc_verify_now:` read the result on the N-ACTION
  association only under a watchdog. That watchdog works on its own duplicate of the socket, and
  a socket cannot be duplicated with winsock 1, so on Windows nothing is read there. The console
  shows `*** Storage commitment: no watchdog for the same-association wait (<AE>): association
  closed without reading, the result must come on a new association`. The archive must therefore
  be able to open an association back to dgate:
  - a map entry for dgate's AE at the archive;
  - with `T`, to dgate's `TLSPort`;
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
