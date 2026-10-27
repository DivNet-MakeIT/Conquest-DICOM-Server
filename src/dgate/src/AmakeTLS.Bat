rem 20261027 mmd Optional 64-bit build with DICOM TLS (-DHAVE_OPENSSL), called by ..\amake.bat only
rem              when OPENSSLDIR is set; the plain dgate64.exe/dgate.exe builds are unchanged.
rem              OPENSSLDIR = OpenSSL 1.1.1 or later built for the same compiler and the static runtime (/MT),
rem              laid out as the Shining Light installer (Win64OpenSSL): include\openssl\*.h and
rem              lib\VC\x64\MT\libssl_static.lib, libcrypto_static.lib. Other layouts: set ssllibs first.
rem              Result: ..\dgate64tls.exe (no OpenSSL DLL needed). See docs/Building-Windows.md
rem              Visual Studio 2015 and later: set EXTRALIBS=legacy_stdio_definitions.lib (as for amake.bat)
rem              The default library paths are quoted: OPENSSLDIR may contain spaces (C:\Program Files\OpenSSL-Win64).
rem              A ssllibs set by hand quotes its own paths when they contain spaces.

if not defined ssllibs set ssllibs="%OPENSSLDIR%\lib\VC\x64\MT\libssl_static.lib" "%OPENSSLDIR%\lib\VC\x64\MT\libcrypto_static.lib"

copy ..\openjpeg\opj_config.h.ms8 ..\openjpeg\opj_config.h
copy ..\openjpeg\opj_inttypes.h.ms8 ..\openjpeg\opj_inttypes.h
copy ..\openjpeg\opj_config_private.h.ms8 ..\openjpeg\opj_config_private.h

set luaextern=-DLUA51EXTERN
set flags=/D_CRT_SECURE_NO_DEPRECATE -DWIN32 -DWINDOWS -DNATIVE_ENDIAN=1 -DUSESQLITE -DUSEMYSQL -DPOSTGRES -DHAVE_LIBJPEG -DHAVE_LIBCHARLS -DHAVE_LIBOPENJPEG2 -DHAVE_OPENSSL
set libs=odbc32.lib odbccp32.lib wsock32.lib ws2_32.lib crypt32.lib advapi32.lib user32.lib kernel32.lib %EXTRALIBS%

set spath=%path%
call ..\asetcompiler64.bat
set path=%compiler%;%path%
set include=%include%;..\dicomlib;..\jpeg-6c;..\sqlite3;C:\mysql-5.0.22-win32\include;..\lua_5.1.5;..\openjpeg;..\charls;%OPENSSLDIR%\include
%ccompiler% /Zi /MT /nologo /O2 /W2 /c %flags% %luaextern% total.cpp
copy total.obj ..\build64\totaltls.obj
del ..\build64\dgatetls.map
link /OUT:..\build64\dgatetls.exe /MAP:..\build64\dgatetls.map /DEBUG ..\build64\totaltls.obj ..\build64\sqlite3.obj %libs% ..\build64\charls.obj ..\build64\libjpeg.lib ..\build64\openjp2.obj %ssllibs%
copy ..\build64\dgatetls.exe ..\dgate64tls.exe

set path=%spath%
del *.obj
del *.pdb

copy ..\openjpeg\opj_config.h.linux ..\openjpeg\opj_config.h
copy ..\openjpeg\opj_inttypes.h.linux ..\openjpeg\opj_inttypes.h
copy ..\openjpeg\opj_config_private.h.linux ..\openjpeg\opj_config_private.h
