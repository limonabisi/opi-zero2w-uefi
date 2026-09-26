/** @file
  OpiSetup user interface strings (Turkish, English).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "OpiSetup.h"

UINTN  gLanguage = 1;

STATIC CONST CHAR16  *mStrings[StrCount][2] = {
  [StrSetup]            = { L"UEFI Kurulum",                              L"UEFI Setup"                               },
  [StrMain]             = { L"Ana Sayfa",                                 L"Main"                                     },
  [StrConfig]           = { L"Yapılandırma",                              L"Config"                                   },
  [StrDateTime]         = { L"Tarih/Saat",                                L"Date/Time"                                },
  [StrSecurity]         = { L"Güvenlik",                                  L"Security"                                 },
  [StrStartup]          = { L"Başlangıç",                                 L"Startup"                                  },
  [StrExit]             = { L"Çıkış",                                     L"Restart"                                  },
  [StrMainSub]          = { L"Sistem bilgileri",                          L"System information"                       },
  [StrConfigSub]        = { L"Ekran, dil ve genel ayarlar",               L"Display, language and general settings"   },
  [StrDateTimeSub]      = { L"Kartın saati (pil yok: elektrik kesilince sıfırlanır)", L"Board clock (no battery: resets on power loss)" },
  [StrSecuritySub]      = { L"Güvenlik özellikleri",                      L"Security features"                        },
  [StrStartupSub]       = { L"Açılış sırası ve açılış aygıtı",            L"Boot order and boot device"               },
  [StrExitSub]          = { L"Kaydet, yeniden başlat veya çık",           L"Save, restart or leave setup"             },
  [StrSystem]           = { L"Sistem",                                    L"System"                                   },
  [StrBoard]            = { L"Kart",                                      L"Board"                                    },
  [StrSoc]              = { L"İşlemci",                                   L"Processor"                                },
  [StrCpu]              = { L"Çekirdek / hız",                            L"Cores / speed"                            },
  [StrMemory]           = { L"Bellek",                                    L"Memory"                                   },
  [StrFirmware]         = { L"Firmware sürümü",                           L"Firmware version"                         },
  [StrFirmwareSec]      = { L"Firmware",                                  L"Firmware"                                 },
  [StrBuildDate]        = { L"Derleme tarihi",                            L"Build date"                               },
  [StrUefi]             = { L"UEFI",                                      L"UEFI"                                     },
  [StrDisplay]          = { L"Ekran",                                     L"Display"                                  },
  [StrStorage]          = { L"Depolama",                                  L"Storage"                                  },
  [StrNoStorage]        = { L"Aygıt yok",                                 L"No devices"                               },
  [StrConsole]          = { L"Seri konsol",                               L"Serial console"                           },
  [StrDisplaySec]       = { L"Ekran",                                     L"Display"                                  },
  [StrHdmiRes]          = { L"HDMI çözünürlüğü",                          L"HDMI resolution"                          },
  [StrAuto]             = { L"Otomatik",                                  L"Automatic"                                },
  [StrHdmiNote]         = { L"Yeniden başlatınca geçerli olur",           L"Takes effect after a restart"             },
  [StrGeneral]          = { L"Genel",                                     L"General"                                  },
  [StrLanguage]         = { L"Dil",                                       L"Language"                                 },
  [StrTimeout]          = { L"Açılış bekleme süresi",                     L"Boot timeout"                             },
  [StrSeconds]          = { L"sn",                                        L"s"                                        },
  [StrNow]              = { L"Şu an",                                     L"Now"                                      },
  [StrDate]             = { L"Tarih",                                     L"Date"                                     },
  [StrTime]             = { L"Saat",                                      L"Time"                                     },
  [StrYear]             = { L"Yıl",                                       L"Year"                                     },
  [StrMonth]            = { L"Ay",                                        L"Month"                                    },
  [StrDay]              = { L"Gün",                                       L"Day"                                      },
  [StrHour]             = { L"Saat",                                      L"Hour"                                     },
  [StrMinute]           = { L"Dakika",                                    L"Minute"                                   },
  [StrApplyTime]        = { L"Saati ayarla",                              L"Set clock"                                },
  [StrTimeSet]          = { L"Saat ayarlandı.",                           L"Clock set."                               },
  [StrSecureBoot]       = { L"Secure Boot",                               L"Secure Boot"                              },
  [StrNotSupported]     = { L"Desteklenmiyor",                            L"Not supported"                            },
  [StrTpm]              = { L"TPM",                                       L"TPM"                                      },
  [StrNone]             = { L"Yok",                                       L"None"                                     },
  [StrAcpi]             = { L"ACPI tabloları (Windows)",                  L"ACPI tables (Windows)"                    },
  [StrAcpiOn]           = { L"Etkin",                                     L"Enabled"                                  },
  [StrPassword]         = { L"Kurulum parolası",                          L"Setup password"                           },
  [StrNotSet]           = { L"Ayarlanmadı",                               L"Not set"                                  },
  [StrBootOrder]        = { L"Açılış sırası",                             L"Boot order"                               },
  [StrBootOrderHint]    = { L"+/- ile sırala, Enter ile şimdi başlat",    L"+/- to reorder, Enter to boot now"        },
  [StrBootNow]          = { L"Şimdi başlat",                              L"Boot now"                                 },
  [StrBootFailed]       = { L"Bu aygıttan açılamadı.",                    L"Could not boot from this device."         },
  [StrOneTime]          = { L"Tek seferlik açılış",                       L"One-time boot"                            },
  [StrSaveExit]         = { L"Kaydet ve çık",                             L"Save and exit"                            },
  [StrSaveRestart]      = { L"Kaydet ve yeniden başlat",                  L"Save and restart"                         },
  [StrDiscardExit]      = { L"Kaydetmeden çık",                           L"Exit without saving"                      },
  [StrRestart]          = { L"Yeniden başlat",                            L"Restart"                                  },
  [StrShutdown]         = { L"Kapat",                                     L"Shut down"                                },
  [StrShell]            = { L"UEFI Shell",                                L"UEFI Shell"                               },
  [StrClassic]          = { L"Klasik menü",                               L"Classic menu"                             },
  [StrSaveExitDesc]     = { L"Ayarları kaydet ve açılışa devam et",       L"Save settings and continue booting"       },
  [StrSaveRestartDesc]  = { L"Ayarları kaydet ve kartı yeniden başlat",   L"Save settings and restart the board"      },
  [StrDiscardExitDesc]  = { L"Değişiklikleri at ve açılışa devam et",     L"Discard changes and continue booting"     },
  [StrRestartDesc]      = { L"Kaydetmeden yeniden başlat",                L"Restart without saving"                   },
  [StrShutdownDesc]     = { L"Kartı kapat",                               L"Power the board off"                      },
  [StrShellDesc]        = { L"Komut satırını aç",                         L"Open the command line"                    },
  [StrClassicDesc]      = { L"EDK2'nin metin tabanlı menüsü",             L"EDK2's text based menu"                   },
  [StrKeyMove]          = { L"Seç",                                       L"Select"                                   },
  [StrKeyChange]        = { L"Değiştir",                                  L"Change"                                   },
  [StrKeyBack]          = { L"Geri",                                      L"Back"                                     },
  [StrKeySave]          = { L"Kaydet ve çık",                             L"Save and exit"                            },
  [StrYes]              = { L"Evet",                                      L"Yes"                                      },
  [StrNo]               = { L"Hayır",                                     L"No"                                       },
  [StrOk]               = { L"Tamam",                                     L"OK"                                       },
  [StrQuitNoSave]       = { L"Kaydetmeden çıkılsın mı?",                  L"Exit without saving?"                     },
  [StrQuitNoSaveMsg]    = { L"Yaptığın değişiklikler kaybolacak.",        L"Your changes will be lost."               },
  [StrSaveQ]            = { L"Kaydedilsin mi?",                           L"Save changes?"                            },
  [StrSaveMsg]          = { L"Ayarlar kaydedilip açılışa devam edilecek.", L"Settings will be saved and booting continues." },
  [StrRestartNeeded]    = { L"Ekran ayarı için kart yeniden başlatılacak.", L"The board restarts to apply the display setting." },
  [StrBootQ]            = { L"Bu aygıttan açılsın mı?",                   L"Boot from this device?"                   },
  [StrBootMsg]          = { L"Seçilen aygıt bir kez başlatılacak.",       L"The selected device is started once."     },
  [StrBootMenu]         = { L"Açılış menüsü",                             L"Boot Menu"                                },
  [StrBootMenuSub]      = { L"Başlatılacak aygıtı seç",                   L"Choose a device to start"                 },
  [StrEnterSetup]       = { L"Kurulum (Setup)",                           L"Enter Setup"                              },
  [StrContinueBoot]     = { L"Normal açılış",                             L"Continue boot"                            },
  [StrSaved]            = { L"Kaydedildi.",                               L"Saved."                                   },
  [StrTurkish]          = { L"Türkçe",                                    L"Türkçe"                                   },
  [StrEnglish]          = { L"English",                                   L"English"                                  },
};

CONST CHAR16 *
S (
  STR_ID  Id
  )
{
  CONST CHAR16  *Str;

  Str = mStrings[Id][gLanguage & 1];
  return (Str != NULL) ? Str : L"?";
}
