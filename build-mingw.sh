#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
compiler="${CXX:-x86_64-w64-mingw32-g++}"
resource_compiler="${WINDRES:-x86_64-w64-mingw32-windres}"
mkdir -p dist
(cd app && "$resource_compiler" -I . -O coff resources.rc -o ../dist/resources.o)
"$compiler" -std=c++20 -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -O2 -Wall -Wextra -Werror -municode -mwindows -static -static-libgcc -static-libstdc++ -s -Wl,--dynamicbase,--nxcompat,--high-entropy-va -o dist/LunarTweaks.exe app/main.cpp app/catalog.cpp dist/resources.o -lgdiplus -ldwmapi -lcomctl32 -lwinhttp -lbcrypt -lncrypt -lcrypt32 -lshell32 -lole32 -luuid -lpowrprof -liphlpapi -lws2_32 -ladvapi32 -lgdi32 -luser32 -lmsimg32 -lwinmm -lsetupapi
cp -R assets dist/
cp LunarTweaks.ini dist/
cp docs/CUSTOMER-START.txt dist/START-HERE.txt
cp docs/THIRD-PARTY-NOTICES.txt dist/THIRD-PARTY-NOTICES.txt
rm -f dist/resources.o

(cd app && "$resource_compiler" -I . -O coff owner_start.rc -o ../dist/owner-start.o)
"$compiler" -std=c++20 -O2 -Wall -Wextra -Werror -municode -mwindows -static -static-libgcc -static-libstdc++ -s -Wl,--dynamicbase,--nxcompat,--high-entropy-va app/owner_start.cpp dist/owner-start.o -o dist/OPEN-LUNARTWEAKS.exe -luser32
