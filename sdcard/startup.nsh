@echo -off
# Orange Pi Zero 2W - EDK2 UEFI Shell otomatik betigi
# UART'tan klavye girisi olmadigi icin komutlari buraya yaz
# (SD karti PC'ye takip bu dosyayi duzenle). SD kart = fs0:
echo " "
echo "=== Orange Pi Zero 2W / H618 - EDK2 startup.nsh ==="
ver
echo "== Diskler =="
map -r
echo "== SD kart (fs0:) =="
ls fs0:\
echo "== Bellek haritasi =="
memmap
echo "== EFI tablolari (DTB Table dolu olmali) =="
dmem
# Linux / GRUB EFI dosyasi varsa onu calistir:
if exist fs0:\EFI\BOOT\BOOTAA64.EFI then
  echo "BOOTAA64.EFI baslatiliyor..."
  fs0:\EFI\BOOT\BOOTAA64.EFI
endif
echo "startup.nsh bitti. (Girdi olmadigi icin shell burada bekler.)"
