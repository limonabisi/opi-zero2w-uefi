@echo off
REM Bu klasoru WSL icine kopyalar ve her seyi derler (ilk sefer ~20-30 dk).
REM Cikti: out\opi-zero2w-edk2-sd.img  (Rufus / balenaEtcher ile SD karta yaz)
set HERE=%~dp0..\..
wsl -d Ubuntu-24.04 -- bash -lc "rm -rf ~/opi-zero2w-edk2 && cp -r \"$(wslpath '%HERE%')\" ~/opi-zero2w-edk2 && rm -rf ~/opi-zero2w-edk2/src ~/opi-zero2w-edk2/build && cd ~/opi-zero2w-edk2 && bash scripts/build-all.sh DEBUG && mkdir -p \"$(wslpath '%HERE%')/out\" && cp out/* \"$(wslpath '%HERE%')/out/\""
pause
