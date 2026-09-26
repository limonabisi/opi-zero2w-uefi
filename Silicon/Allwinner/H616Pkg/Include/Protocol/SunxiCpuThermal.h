/** @file
  H616/H618 CPU clock, core voltage and temperature information, produced by
  SunxiCpuThermalDxe.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef SUNXI_CPU_THERMAL_H_
#define SUNXI_CPU_THERMAL_H_

#define SUNXI_CPU_THERMAL_PROTOCOL_GUID \
  { 0x6c2f1a9e, 0x3b7d, 0x4e51, { 0x9a, 0x0c, 0x47, 0xd2, 0x8e, 0x15, 0xb3, 0x6f } }

//
// "CpuSpeed" variable (UINT8, setup GUID below): 0 = highest speed the chip's
// speed bin allows, 1 = 1008 MHz, 2 = 1200 MHz.
//
#define SUNXI_SETUP_VARIABLE_GUID \
  { 0x9a1e3b57, 0x2c64, 0x4d8f, { 0xb0, 0x71, 0x5e, 0x8c, 0x3a, 0x42, 0x6d, 0x19 } }

#define SUNXI_CPU_SPEED_MAX   0
#define SUNXI_CPU_SPEED_1008  1
#define SUNXI_CPU_SPEED_1200  2

#define SUNXI_THS_SENSOR_GPU  0
#define SUNXI_THS_SENSOR_VE   1
#define SUNXI_THS_SENSOR_CPU  2
#define SUNXI_THS_SENSOR_DDR  3

typedef struct _SUNXI_CPU_THERMAL_PROTOCOL SUNXI_CPU_THERMAL_PROTOCOL;

typedef
EFI_STATUS
(EFIAPI *SUNXI_GET_TEMPERATURE)(
  IN  SUNXI_CPU_THERMAL_PROTOCOL  *This,
  IN  UINTN                       Sensor,
  OUT INT32                       *MilliCelsius
  );

struct _SUNXI_CPU_THERMAL_PROTOCOL {
  UINT32                   SpeedBin;       // 0..5, as in Linux sun50i-cpufreq-nvmem
  UINT32                   MaxMhz;         // highest speed allowed for this chip
  UINT32                   CurrentMhz;
  UINT32                   VddCpuMv;
  SUNXI_GET_TEMPERATURE    GetTemperature;
};

extern EFI_GUID  gSunxiCpuThermalProtocolGuid;

#endif
