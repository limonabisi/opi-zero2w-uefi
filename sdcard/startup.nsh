@echo -off
# Orange Pi Zero 2W - EDK2 UEFI Shell otomatik betigi
# UART'tan klavye girisi olmadigi icin tum komutlari buraya yaz.
# Bu dosyayi SD karti PC'ye takip duzenleyebilirsin.
echo " "
echo "=== Orange Pi Zero 2W / H618 - EDK2 startup.nsh ==="
ver
echo "--- Diskler ---"
map -r
echo "--- SD kart (fs0:) ---"
ls fs0:\
echo "--- Bellek haritasi ---"
memmap
echo "--- Yapilandirma tablolari (FDT olmali) ---"
dmem -verbose
# Linux/GRUB EFI dosyasi varsa onu calistir:
if exist fs0:\EFI\BOOT\BOOTAA64.EFI then
  echo "BOOTAA64.EFI baslatiliyor..."
  fs0:\EFI\BOOT\BOOTAA64.EFI
endif
echo "startup.nsh bitti. (Girdi olmadigi icin shell burada bekler.)"
