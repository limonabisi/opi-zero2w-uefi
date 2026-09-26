/** @file
  H616/H618: raise the CPU clock (PLL_CPUX) and core voltage (AXP313A DCDC2
  on the R_I2C bus) to the highest operating point the chip's speed bin
  allows, and bring up the thermal sensor (THS) so setup can show the
  temperature.

  Operating points and voltages follow Linux sun50i-h616-cpu-opp.dtsi, capped
  at 1.10 V (the Orange Pi Zero 2W device tree limit for vdd-cpu).

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/SunxiCpuThermal.h>

#define CCU_BASE          0x03001000
#define CCU_PLL_CPUX      0x000
#define CCU_CPU_AXI       0x500
#define CCU_THS_BGR       0x9FC

#define PLL_EN            BIT31
#define PLL_LOCK_EN       BIT29
#define PLL_LOCK          BIT28
#define PLL_OUT_EN        BIT27
#define PLL_CLOCK_TIME_2  (2 << 24)
#define PLL_N(n)          (((n) - 1) << 8)
#define CPU_MUX_MASK      (3 << 24)
#define CPU_MUX_OSC24M    (0 << 24)
#define CPU_MUX_PLL       (3 << 24)

#define SYS_CFG_BASE      0x03000000
#define SID_BASE          0x03006200      // SID efuse values (value_offset 0x200)

#define THS_BASE          0x05070400
#define THS_CTRL0         0x00
#define THS_ENABLE        0x04
#define THS_PC            0x08
#define THS_MFC           0x30
#define THS_CALIB         0xA0
#define THS_DATA          0xC0
#define THS_SENSORS       4
#define THS_OFFSET        263655
#define THS_SCALE         810
#define THS_FT_DEVIATION  8000

#define R_PRCM_BASE       0x07010000
#define R_TWI_BGR         0x19C
#define R_PIO_BASE        0x07022000      // port L
#define R_TWI_BASE        0x07081400

#define TWI_DATA          0x08
#define TWI_CNTR          0x0C
#define TWI_STAT          0x10
#define TWI_CCR           0x14
#define TWI_SRST          0x18
#define TWI_EFR           0x1C

#define CNTR_BUS_EN       0x40
#define CNTR_M_STA        0x20
#define CNTR_M_STP        0x10
#define CNTR_INT_FLAG     0x08
#define CNTR_A_ACK        0x04

#define AXP313_ADDR       0x36
#define AXP313_CHIP_ID    0x03
#define AXP313_DCDC2_V    0x14

#define VDD_CPU_MAX_MV    1100

typedef struct {
  UINT32    Mhz;
  UINT16    Mv[6];      // per speed bin, 0 = not supported by that bin
} CPU_OPP;

STATIC CONST CPU_OPP  mOpps[] = {
  { 1008, { 950,  940,  950,  950,  1020, 900  } },
  { 1200, { 1050, 1020, 1050, 1050, 1100, 1020 } },
  { 1320, { 1100, 0,    1100, 1100, 1100, 0    } },
  { 1416, { 1100, 0,    1100, 1100, 0,    1100 } },
  { 1512, { 0,    1100, 0,    1100, 0,    0    } },
};

STATIC SUNXI_CPU_THERMAL_PROTOCOL  mProtocol;
STATIC BOOLEAN                     mPmicOk;
STATIC EFI_EVENT                   mGuardEvent;

// ------------------------------------------------------------ R_I2C ---

STATIC
BOOLEAN
TwiWait (
  VOID
  )
{
  UINTN  Timeout;

  for (Timeout = 0; Timeout < 10000; Timeout++) {
    if ((MmioRead32 (R_TWI_BASE + TWI_CNTR) & CNTR_INT_FLAG) != 0) {
      return TRUE;
    }

    MicroSecondDelay (1);
  }

  return FALSE;
}

STATIC
VOID
TwiStop (
  VOID
  )
{
  UINTN  Timeout;

  MmioWrite32 (R_TWI_BASE + TWI_CNTR, CNTR_BUS_EN | CNTR_M_STP | CNTR_INT_FLAG);
  for (Timeout = 0; Timeout < 10000; Timeout++) {
    if ((MmioRead32 (R_TWI_BASE + TWI_CNTR) & CNTR_M_STP) == 0) {
      break;
    }

    MicroSecondDelay (1);
  }
}

STATIC
BOOLEAN
TwiStart (
  UINT8   Addr,
  UINT32  Expect
  )
{
  UINT32  Stat;

  MmioWrite32 (R_TWI_BASE + TWI_CNTR, CNTR_BUS_EN | CNTR_M_STA | CNTR_INT_FLAG);
  if (!TwiWait ()) {
    return FALSE;
  }

  Stat = MmioRead32 (R_TWI_BASE + TWI_STAT);
  if ((Stat != 0x08) && (Stat != 0x10)) {
    return FALSE;
  }

  MmioWrite32 (R_TWI_BASE + TWI_DATA, Addr);
  MmioWrite32 (R_TWI_BASE + TWI_CNTR, CNTR_BUS_EN | CNTR_INT_FLAG);
  return TwiWait () && (MmioRead32 (R_TWI_BASE + TWI_STAT) == Expect);
}

STATIC
BOOLEAN
TwiSend (
  UINT8  Byte
  )
{
  MmioWrite32 (R_TWI_BASE + TWI_DATA, Byte);
  MmioWrite32 (R_TWI_BASE + TWI_CNTR, CNTR_BUS_EN | CNTR_INT_FLAG);
  return TwiWait () && (MmioRead32 (R_TWI_BASE + TWI_STAT) == 0x28);
}

STATIC
EFI_STATUS
PmicRead (
  UINT8  Reg,
  UINT8  *Val
  )
{
  BOOLEAN  Ok;

  Ok = TwiStart (AXP313_ADDR << 1, 0x18) && TwiSend (Reg) &&
       TwiStart ((AXP313_ADDR << 1) | 1, 0x40);
  if (Ok) {
    MmioWrite32 (R_TWI_BASE + TWI_CNTR, CNTR_BUS_EN | CNTR_INT_FLAG);     // no ACK: last byte
    Ok = TwiWait () && (MmioRead32 (R_TWI_BASE + TWI_STAT) == 0x58);
    *Val = (UINT8)MmioRead32 (R_TWI_BASE + TWI_DATA);
  }

  TwiStop ();
  return Ok ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
EFI_STATUS
PmicWrite (
  UINT8  Reg,
  UINT8  Val
  )
{
  BOOLEAN  Ok;

  Ok = TwiStart (AXP313_ADDR << 1, 0x18) && TwiSend (Reg) && TwiSend (Val);
  TwiStop ();
  return Ok ? EFI_SUCCESS : EFI_DEVICE_ERROR;
}

STATIC
VOID
TwiInit (
  VOID
  )
{
  // PL0/PL1 = S_TWI (function 3), R_TWI clock + reset, 100 kHz
  MmioAndThenOr32 (R_PIO_BASE + 0x00, ~(UINT32)0xFF, 0x33);
  MmioOr32 (R_PRCM_BASE + R_TWI_BGR, BIT0 | BIT16);
  MicroSecondDelay (10);
  MmioWrite32 (R_TWI_BASE + TWI_SRST, 1);
  MicroSecondDelay (10);
  MmioWrite32 (R_TWI_BASE + TWI_CCR, (5 << 3) | 2);      // 24 MHz / (10 * 6 * 4)
  MmioWrite32 (R_TWI_BASE + TWI_EFR, 0);
  MmioWrite32 (R_TWI_BASE + TWI_CNTR, CNTR_BUS_EN);
}

STATIC
UINT32
Dcdc2ToMv (
  UINT8  Reg
  )
{
  Reg &= 0x7F;
  return (Reg <= 70) ? (500 + Reg * 10) : (1220 + (Reg - 71) * 20);
}

STATIC
UINT8
MvToDcdc2 (
  UINT32  Mv
  )
{
  return (UINT8)((Mv <= 1200) ? ((Mv - 500) / 10) : (71 + (Mv - 1220) / 20));
}

STATIC
EFI_STATUS
SetVddCpu (
  UINT32  Mv
  )
{
  EFI_STATUS  Status;
  UINT8       Cur;

  if (!mPmicOk || (Mv < 810) || (Mv > VDD_CPU_MAX_MV)) {
    return EFI_UNSUPPORTED;
  }

  Status = PmicRead (AXP313_DCDC2_V, &Cur);
  if (EFI_ERROR (Status)) {
    return Status;
  }

  Status = PmicWrite (AXP313_DCDC2_V, (Cur & 0x80) | MvToDcdc2 (Mv));
  if (!EFI_ERROR (Status)) {
    MicroSecondDelay (2000);            // ramp
    mProtocol.VddCpuMv = Mv;
  }

  return Status;
}

// -------------------------------------------------------------- PLL ---

STATIC
UINT32
GetCpuMhz (
  VOID
  )
{
  UINT32  Val;

  Val = MmioRead32 (CCU_BASE + CCU_PLL_CPUX);
  return (24 * (((Val >> 8) & 0xFF) + 1)) >> ((Val >> 16) & 3);
}

STATIC
VOID
SetCpuMhz (
  UINT32  Mhz
  )
{
  UINT32  Val;
  UINTN   Timeout;

  MmioAndThenOr32 (CCU_BASE + CCU_CPU_AXI, ~(UINT32)CPU_MUX_MASK, CPU_MUX_OSC24M);
  MicroSecondDelay (1);

  Val = PLL_EN | PLL_LOCK_EN | PLL_OUT_EN | PLL_CLOCK_TIME_2 | PLL_N (Mhz / 24);
  MmioWrite32 (CCU_BASE + CCU_PLL_CPUX, Val);
  for (Timeout = 0; Timeout < 100000; Timeout++) {
    if ((MmioRead32 (CCU_BASE + CCU_PLL_CPUX) & PLL_LOCK) != 0) {
      break;
    }
  }

  MicroSecondDelay (20);
  MmioAndThenOr32 (CCU_BASE + CCU_CPU_AXI, ~(UINT32)CPU_MUX_MASK, CPU_MUX_PLL);
  mProtocol.CurrentMhz = GetCpuMhz ();
}

//
// Move to an operating point: voltage up before the clock, down after it.
//
STATIC
EFI_STATUS
SetOpp (
  CONST CPU_OPP  *Opp
  )
{
  UINT32      Mv;
  EFI_STATUS  Status;

  Mv = Opp->Mv[mProtocol.SpeedBin];
  if (Mv == 0) {
    return EFI_UNSUPPORTED;
  }

  if (Opp->Mhz > mProtocol.CurrentMhz) {
    Status = SetVddCpu (Mv);
    if (EFI_ERROR (Status)) {
      return Status;
    }

    SetCpuMhz (Opp->Mhz);
  } else {
    SetCpuMhz (Opp->Mhz);
    SetVddCpu (Mv);
  }

  return EFI_SUCCESS;
}

// ---------------------------------------------------------- speed bin ---

STATIC
UINT32
GetSpeedBin (
  UINT32  *Raw
  )
{
  *Raw = MmioRead32 (SID_BASE) & 0xFFFF;
  switch (*Raw) {
    case 0x2000:
    case 0x5d00:
      return 0;
    case 0x2400:
    case 0x7400:
    case 0x2c00:
    case 0x7c00:
      return 2;                         // IC version C and later (A/B would be 1)
    case 0x5000:
    case 0x5400:
    case 0x6000:
      return 3;
    case 0x5c00:
      return 4;
    case 0x6c00:
      return 5;
    default:
      return 0;
  }
}

// ---------------------------------------------------------- thermal ---

STATIC
INT32
ThsCalcTemp (
  INT32  Reg
  )
{
  return THS_OFFSET - (Reg * THS_SCALE / 10);
}

STATIC
VOID
ThsInit (
  VOID
  )
{
  UINT32  W0, W1;
  UINT16  Cal[4];
  INT32   FtTemp;
  UINTN   I;
  INT32   SensorReg, SensorTemp, CData, Shift;

  MmioAnd32 (SYS_CFG_BASE, ~(UINT32)BIT16);                  // H616: THS needs this cleared
  MmioOr32 (CCU_BASE + CCU_THS_BGR, BIT0 | BIT16);
  MicroSecondDelay (10);

  W0     = MmioRead32 (SID_BASE + 0x14);
  W1     = MmioRead32 (SID_BASE + 0x18);
  Cal[0] = (UINT16)W0;
  Cal[1] = (UINT16)(W0 >> 16);
  Cal[2] = (UINT16)W1;
  Cal[3] = (UINT16)(W1 >> 16);

  if (Cal[0] != 0) {
    FtTemp = (Cal[0] & 0xFFF) * 100;
    for (I = 0; I < THS_SENSORS; I++) {
      if (I == 3) {
        SensorReg = (Cal[1] >> 12) | ((Cal[2] >> 12) << 4) | ((Cal[3] >> 12) << 8);
      } else {
        SensorReg = Cal[I + 1] & 0xFFF;
      }

      SensorTemp = ThsCalcTemp (SensorReg);
      CData      = 0x800 - ((SensorTemp - FtTemp) * 10 / THS_SCALE);
      if ((CData & ~0xFFF) != 0) {
        continue;
      }

      Shift = (I % 2) * 16;
      MmioAndThenOr32 (THS_BASE + THS_CALIB + (I / 2) * 4, ~((UINT32)0xFFF << Shift), (UINT32)CData << Shift);
    }
  }

  MmioWrite32 (THS_BASE + THS_CTRL0, ((480 - 1) << 16) | (48 - 1));
  MmioWrite32 (THS_BASE + THS_MFC, BIT2 | 1);                // filter, 4 samples
  MmioWrite32 (THS_BASE + THS_PC, 365 << 12);                // ~0.25 s
  MmioWrite32 (THS_BASE + THS_ENABLE, (1 << THS_SENSORS) - 1);
}

STATIC
EFI_STATUS
EFIAPI
GetTemperature (
  IN  SUNXI_CPU_THERMAL_PROTOCOL  *This,
  IN  UINTN                       Sensor,
  OUT INT32                       *MilliCelsius
  )
{
  UINT32  Reg;

  if ((Sensor >= THS_SENSORS) || (MilliCelsius == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  Reg = MmioRead32 (THS_BASE + THS_DATA + Sensor * 4) & 0xFFF;
  if (Reg == 0) {
    return EFI_NOT_READY;
  }

  *MilliCelsius = ThsCalcTemp ((INT32)Reg) + THS_FT_DEVIATION;
  return EFI_SUCCESS;
}

#ifdef SUNXI_NO_HW
STATIC
EFI_STATUS
EFIAPI
GetTemperatureFake (
  IN  SUNXI_CPU_THERMAL_PROTOCOL  *This,
  IN  UINTN                       Sensor,
  OUT INT32                       *MilliCelsius
  )
{
  *MilliCelsius = 42500;
  return EFI_SUCCESS;
}

#endif

//
// While firmware runs: drop to 1008 MHz if the CPU gets hot.
//
STATIC
VOID
EFIAPI
ThermalGuard (
  IN EFI_EVENT  Event,
  IN VOID       *Context
  )
{
  INT32  T;

  if (!EFI_ERROR (GetTemperature (&mProtocol, SUNXI_THS_SENSOR_CPU, &T)) &&
      (T > 90000) && (mProtocol.CurrentMhz > 1008))
  {
    DEBUG ((DEBUG_WARN, "SunxiCpu: %d.%d C, throttling to 1008 MHz\n", T / 1000, (T % 1000) / 100));
    SetOpp (&mOpps[0]);
  }
}

EFI_STATUS
EFIAPI
SunxiCpuThermalDxeInitialize (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  STATIC EFI_GUID  SetupGuid = SUNXI_SETUP_VARIABLE_GUID;
  UINT32           Raw;
  UINT8            Id;
  UINT8            Dcdc2;
  UINT8            Speed;
  UINTN            Size;
  UINTN            I;
  CONST CPU_OPP    *Target;
  EFI_STATUS       Status;
  EFI_HANDLE       Handle;

  mProtocol.GetTemperature = GetTemperature;

 #ifdef SUNXI_NO_HW
  // QEMU test build: no H616 registers, just publish the protocol
  mProtocol.SpeedBin       = 0;
  mProtocol.MaxMhz         = 1416;
  mProtocol.CurrentMhz     = 1416;
  mProtocol.VddCpuMv       = 1100;
  mProtocol.GetTemperature = GetTemperatureFake;
  Handle                   = NULL;
  return gBS->InstallMultipleProtocolInterfaces (&Handle, &gSunxiCpuThermalProtocolGuid, &mProtocol, NULL);
 #endif

  mProtocol.SpeedBin       = GetSpeedBin (&Raw);
  mProtocol.CurrentMhz     = GetCpuMhz ();

  // highest operating point this bin supports within the 1.10 V limit
  Target = &mOpps[0];
  for (I = 0; I < ARRAY_SIZE (mOpps); I++) {
    if ((mOpps[I].Mv[mProtocol.SpeedBin] != 0) && (mOpps[I].Mv[mProtocol.SpeedBin] <= VDD_CPU_MAX_MV)) {
      Target = &mOpps[I];
    }
  }

  mProtocol.MaxMhz = Target->Mhz;

  ThsInit ();

  TwiInit ();
  mPmicOk = !EFI_ERROR (PmicRead (AXP313_CHIP_ID, &Id)) && ((Id & 0xC8) == 0x48) &&
            !EFI_ERROR (PmicRead (AXP313_DCDC2_V, &Dcdc2));
  if (mPmicOk) {
    mProtocol.VddCpuMv = Dcdc2ToMv (Dcdc2);
  }

  Size  = sizeof (Speed);
  Speed = SUNXI_CPU_SPEED_MAX;
  gRT->GetVariable (L"CpuSpeed", &SetupGuid, NULL, &Size, &Speed);
  if (Speed == SUNXI_CPU_SPEED_1008) {
    Target = &mOpps[0];
  } else if (Speed == SUNXI_CPU_SPEED_1200) {
    Target = &mOpps[1];
  }

  DEBUG ((
    DEBUG_INFO,
    "SunxiCpu: speed bin %u (efuse 0x%04x), max %u MHz; PMIC %a, VDD-CPU %u mV, CPU %u MHz\n",
    mProtocol.SpeedBin,
    Raw,
    mProtocol.MaxMhz,
    mPmicOk ? "AXP313A" : "not found",
    mProtocol.VddCpuMv,
    mProtocol.CurrentMhz
    ));

  if (!mPmicOk) {
    DEBUG ((DEBUG_WARN, "SunxiCpu: no PMIC access, CPU stays at %u MHz\n", mProtocol.CurrentMhz));
  } else if (Target->Mhz != mProtocol.CurrentMhz) {
    Status = SetOpp (Target);
    DEBUG ((
      DEBUG_INFO,
      "SunxiCpu: -> %u MHz @ %u mV: %r\n",
      mProtocol.CurrentMhz,
      mProtocol.VddCpuMv,
      Status
      ));
  }

  gBS->CreateEvent (EVT_TIMER | EVT_NOTIFY_SIGNAL, TPL_CALLBACK, ThermalGuard, NULL, &mGuardEvent);
  gBS->SetTimer (mGuardEvent, TimerPeriodic, 10000000);

  Handle = NULL;
  return gBS->InstallMultipleProtocolInterfaces (&Handle, &gSunxiCpuThermalProtocolGuid, &mProtocol, NULL);
}
