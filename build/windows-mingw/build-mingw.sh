#!/bin/bash
# Cross-compile dgate64.exe with mingw-w64 (x86_64), the 64-bit part of src/dgate/amake.bat:
# the same defines, lua5.1.dll loaded at run time (LUA51EXTERN), static runtime.
# docker run --rm --platform linux/amd64 -v "$PWD":/src:ro -v "$PWD/out":/out dgate-mingw build-mingw.sh /src /out plain|tls
set -eu
SRCDIR="$1"; OUT="$2"; VARIANT="${3:-plain}"
mkdir -p "$OUT"
rm -rf /build && cp -r "$SRCDIR" /build && cd /build/src/dgate
CC=x86_64-w64-mingw32-gcc; CXX=x86_64-w64-mingw32-g++; B=/build/obj; mkdir -p $B

# as amake.bat: the .ms8 openjpeg headers; libjpeg with jconfig.vc
cp openjpeg/opj_config.h.ms8 openjpeg/opj_config.h
cp openjpeg/opj_inttypes.h.ms8 openjpeg/opj_inttypes.h
cp openjpeg/opj_config_private.h.ms8 openjpeg/opj_config_private.h
cp jpeg-6c/jconfig.vc jpeg-6c/jconfig.h

$CC -O2 -DHAVE_USLEEP -c sqlite3/sqlite3.c -o $B/sqlite3.o
$CXX -O2 -DDEBUG -c charls/all.cpp -o $B/charls.o
$CC -O2 -DNDEBUG -Iopenjpeg -c openjpeg/all.c -o $B/openjp2.o
for f in jcapimin jcapistd jctrans jcparam jdatadst jcinit jcmaster jcmarker jcmainct jcprepct jclossls jclossy \
         jccoefct jccolor jcsample jchuff jcphuff jcshuff jclhuff jcpred jcscale jcdiffct jcdctmgr jfdctfst jfdctflt \
         jfdctint jdapimin jdapistd jdtrans jdatasrc jdmaster jdinput jdmarker jdlossls jdlossy jdhuff jdlhuff jdphuff \
         jdshuff jdpred jdscale jddiffct jdmainct jdcoefct jdpostct jddctmgr jidctfst jidctflt jidctint jidctred \
         jdsample jdcolor jquant1 jquant2 jdmerge jcomapi jcodec jutils jerror jmemmgr jmemnobs; do
  $CC -O2 -Ijpeg-6c -c jpeg-6c/$f.c -o $B/j_$f.o
done
x86_64-w64-mingw32-ar rcs $B/libjpeg.a $B/j_*.o

# Two Windows-only include spellings of the sources need a cross build on a case-sensitive file system:
# odbci.cpp <sys\stat.h> and odbci.hpp "C:\mysql-5.0.22-win32\include\mysql.h" (USEMYSQL; the MariaDB
# client headers of the build host are used, the DLL is loaded at run time)
SHIM=/build/shim; mkdir -p $SHIM/inc
python3 -I -c "import sys; open(sys.argv[1]+'/sys\\\\stat.h','w').write('#include <sys/stat.h>\\n')" $SHIM
python3 -I -c "import sys; open(sys.argv[1]+'/C:\\\\mysql-5.0.22-win32\\\\include\\\\mysql.h','w').write('#include <mariadb/mysql.h>\\n')" $SHIM
cp -r /usr/include/mariadb $SHIM/inc/

DEFS="-D_CRT_SECURE_NO_DEPRECATE -DWIN32 -DWINDOWS -DNATIVE_ENDIAN=1 -DUSESQLITE -DUSEMYSQL -DPOSTGRES -DHAVE_LIBJPEG -DHAVE_LIBCHARLS -DHAVE_LIBOPENJPEG2 -DLUA51EXTERN"
INCS="-Idicomlib -Ijpeg-6c -Isqlite3 -Ilua_5.1.5 -Iopenjpeg -Icharls -Isrc -I$SHIM -I$SHIM/inc"
LIBS="-lodbc32 -lodbccp32 -lwsock32 -lws2_32 -ladvapi32 -luser32 -lkernel32"
TLSF=""; TLSL=""
if [ "$VARIANT" = "tls" ]; then
  TLSF="-DHAVE_OPENSSL -I/opt/ossl/mingw64/include"
  TLSL="-L/opt/ossl/mingw64/lib -lssl -lcrypto -lcrypt32 -lbcrypt"
fi
# -O0 as maklinux (no -O); -fpermissive: g++ rejects some constructs MSVC accepts (they stay warnings)
$CXX -O0 -g -fpermissive -Wno-write-strings -Wno-multichar $DEFS $TLSF $INCS -c src/total.cpp -o $B/total.o 2>$OUT/compile-total.txt
$CXX -static -o $OUT/dgate64.exe $B/total.o $B/sqlite3.o $B/charls.o $B/libjpeg.a $B/openjp2.o $TLSL $LIBS
echo "built $OUT/dgate64.exe ($VARIANT)"
