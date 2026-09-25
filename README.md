# Orange Pi Zero 2W (H618, 1 GB) — EDK2 UEFI portu

U-Boot proper yerine **TianoCore EDK2** çalıştıran bir önyükleyici. Hedef: SD karttan açılıp
UEFI ortamı sunmak (UEFI Shell, FAT, `BOOTAA64.EFI` ile Linux/GRUB başlatma, Device Tree).

> Durum (v0.2.0): **donanımda çalışıyor** — SPL → TF-A → EDK2 → UEFI Shell; SD kart (FAT) okunuyor,
> `startup.nsh` otomatik çalışıyor, DTB EFI tablosunda. Sıradaki adım: Linux açmak.
>
> Önemli: TF-A'ya `patches/tf-a-0001-sunxi-edk2-bl33.patch` uygulanmalı (build-all.sh yapıyor).
> Yamasız BL31, DTB bulamayınca PMIC kodunda NULL okuyup çöküyor.

## Boot zinciri

```
BROM (SoC ROM)
 └─ SD kart 8 KiB: U-Boot SPL  ... sadece LPDDR4 DRAM init + AXP313 PMIC (mainline, orangepi_zero2w_defconfig)
     └─ FIT @ 8 KiB+40 KiB
         ├─ TF-A BL31  -> 0x40000000  (EL3, PSCI: SMP / reset / poweroff)
         └─ EDK2 FD    -> 0x4A000000  (BL33, EL2)   <-- U-Boot proper'ın yerine
             PeilessSec -> DXE -> BDS -> BOOTAA64.EFI / UEFI Shell (startup.nsh)
```

## Hazır dosyalar (`out/`)

| Dosya | Ne işe yarar |
|---|---|
| `opi-zero2w-edk2-sd.img` | Tam SD kart imajı (64 MB). **Rufus** veya **balenaEtcher** ile karta yaz. |
| `opi-zero2w-edk2-boot.bin` | Sadece önyükleyici. Mevcut bir karta `dd if=... of=/dev/sdX bs=1k seek=8` ile yazılır (Linux imajının üstüne EDK2 koymak için). |

SD imajının FAT32 bölümünde:
- `startup.nsh` — UEFI Shell açılınca **kendiliğinden** çalışır. UART'tan yazamadığın için komutları buraya koy (kartı PC'de düzenle).
- `EFI/BOOT/` — buraya `BOOTAA64.EFI` (GRUB, systemd-boot veya EFI stub'lı Linux çekirdeği) koyarsan otomatik başlar.
- `dtb/` — kartın device tree dosyası (firmware zaten kendi içindekini EFI tablosu olarak veriyor).

## UART (sadece okuma yeterli)

- Header: **pin 6 GND, pin 8 TX (PH0)** → adaptörün RX'ine. 115200 8N1.
- Firmware hiçbir tuşa basılmasını beklemez: 2 sn zaman aşımı → otomatik boot → yoksa UEFI Shell + `startup.nsh`.
- DEBUG build olduğu için her sürücünün yüklenişi UART'a basılır.

Beklenen log sırası (kısaltılmış):
```
U-Boot SPL 2025.07 ... DRAM: 1024 MiB ... Trying to boot from MMC1
NOTICE:  BL31: v2.13.0 ... NOTICE:  BL31: Detected Allwinner H616 SoC
[EDK2] Orange Pi Zero 2W (H618) - UEFI firmware starting
... SunxiMmc: SMHC0 @ 0x4020000 ... FdtDxe: installing DT "OrangePi Zero 2W"
... UEFI Interactive Shell ... === Orange Pi Zero 2W / H618 - EDK2 startup.nsh ===
```

## Yeniden derleme

**Windows + WSL:** `scripts\windows\1-wsl-kur.bat` (yönetici, bir kere, sonra yeniden başlat) →
`scripts\windows\2-derle.bat`. Çıktılar `out\` klasörüne gelir.

**Linux / WSL içinden:** `bash scripts/build-all.sh` (DEBUG) veya `bash scripts/build-all.sh RELEASE`.

Sürümler: U-Boot v2025.07 (SPL), TF-A v2.13.0, EDK2 edk2-stable202608.

## Kaynak yapısı

```
Silicon/Allwinner/H616Pkg/
  H616Pkg.dec
  Include/H616.h                       register haritası (CCU, PIO, GIC, UART, SMHC)
  Drivers/SunxiMmcDxe/                 SD kart sürücüsü (PIO, 400 kHz -> 50 MHz, 4-bit, auto-stop)
Platform/OrangePi/OrangePiZero2W/
  OrangePiZero2W.dsc / .fdf            platform tanımı, FD @ 0x4A000000 (1 MB)
  Library/OrangePiZero2WLib/           ArmPlatformLib: bellek haritası (BL31 deliği), çekirdek listesi
  Drivers/FdtDxe/                      gömülü DTB'yi EFI FDT tablosu olarak kurar, /memory düzeltir
  DeviceTree/sun50i-h618-orangepi-zero2w.dtb
scripts/  build-all.sh, build-edk2.sh, make-image.sh, windows/*.bat
sdcard/startup.nsh
```

## Bellek haritası

| Adres | İçerik |
|---|---|
| `0x0000_0000 – 0x3FFF_FFFF` | SRAM + çevre birimleri (Device) |
| `0x4000_0000 – 0x4003_FFFF` | TF-A BL31 (UEFI'ye verilmez) |
| `0x4004_0000 – 0x7FFF_FFFF` | DRAM (UEFI/OS) |
| `0x4A00_0000 – 0x4A0F_FFFF` | EDK2 FD (boot services data) |
| `0x7C00_0000 – 0x7FFF_FFFF` | UEFI'nin ilk 64 MB çalışma alanı |

## Yapılacaklar (yol haritası)

1. ~~Donanım testi~~ — tamam: Shell + SD + FAT + DTB çalışıyor.
1b. Linux: `EFI/BOOT/BOOTAA64.EFI` (GRUB veya EFI stub'lı çekirdek) ile açılış.
2. SD sürücüsü: DMA (IDMAC) ile hız, kart algılama (PF6).
3. USB (EHCI1/OHCI1 NonDiscoverable) → USB klavye ile giriş (UART RX olmadan etkileşim).
4. Kalıcı değişkenler: SPI NOR (varsa) veya SD kartta dosya tabanlı değişken deposu.
5. HDMI GOP (DE3.3 + HDMI PHY) — en zor adım.
6. Wi-Fi (SDIO, SMHC1) ve RTC.
7. ACPI (Windows / genel ARM dağıtımları için) — isteğe bağlı.

## Lisans

Platform kodu BSD-2-Clause-Patent (EDK2 ile aynı). Register değerleri mainline Linux / U-Boot'tan alınmıştır.
