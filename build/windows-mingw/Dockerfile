# mingw-w64 cross build environment for dgate64.exe (see docs/Building-Windows.md)
# docker build --platform linux/amd64 -t dgate-mingw build/windows-mingw
FROM debian:12
RUN apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    g++-mingw-w64-x86-64 gcc-mingw-w64-x86-64 binutils-mingw-w64-x86-64 make ca-certificates \
    python3 libmariadb-dev >/dev/null
RUN update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix && \
    update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-posix
COPY build-mingw.sh /usr/local/bin/build-mingw.sh
RUN chmod +x /usr/local/bin/build-mingw.sh
