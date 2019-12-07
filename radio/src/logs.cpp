/*
 * Copyright (C) OpenTX
 *
 * Based on code named
 *   th9x - http://code.google.com/p/th9x
 *   er9x - http://code.google.com/p/er9x
 *   gruvin9x - http://code.google.com/p/gruvin9x
 *
 * License GPLv2: http://www.gnu.org/licenses/gpl-2.0.html
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "opentx.h"
#include "ff.h"

FIL g_oLogFile __DMA;
const char * g_logError = nullptr;
uint8_t logDelay;

void writeHeader();

#if defined(PCBTARANIS) || defined(PCBHORUS)
  int getSwitchState(uint8_t swtch) {
    int value = getValue(MIXSRC_FIRST_SWITCH + swtch);
    return (value == 0) ? 0 : (value < 0) ? -1 : +1;
  }
#else
  #define GET_2POS_STATE(sw) (switchState(SW_ ## sw) ? -1 : 1)
  #define GET_3POS_STATE(sw) (switchState(SW_ ## sw ## 0) ? -1 : (switchState(SW_ ## sw ## 2) ? 1 : 0))
#endif

void logsInit()
{
  memset(&g_oLogFile, 0, sizeof(g_oLogFile));
}

const char * logsOpen()
{
  // Determine and set log file filename
  FRESULT result;
  char filename[6 + LEN_MODEL_NAME + 16]; // /LOGS/modelnamexxxxxx_2013-01-01.log

  if (!sdMounted())
    return STR_NO_SDCARD;

  if (sdGetFreeSectors() == 0)
    return STR_SDCARD_FULL;

  // check and create folder here
  strcpy(filename, STR_LOGS_PATH);
  const char * error = sdCheckAndCreateDirectory(filename);
  if (error) {
    return error;
  }

  filename[sizeof(LOGS_PATH)-1] = '/';
  memcpy(&filename[sizeof(LOGS_PATH)], g_model.header.name, sizeof(g_model.header.name));
  filename[sizeof(LOGS_PATH)+sizeof(g_model.header.name)] = '\0';

  uint8_t i = sizeof(LOGS_PATH)+sizeof(g_model.header.name)-1;
  uint8_t len = 0;
  while (i>sizeof(LOGS_PATH)-1) {
    if (!len && filename[i])
      len = i+1;
    if (len) {
      if (filename[i])
        filename[i] = zchar2char(filename[i]);
      else
        filename[i] = '_';
    }
    i--;
  }

  if (len == 0) {
#if defined(EEPROM)
    uint8_t num = g_eeGeneral.currModel + 1;
#else
    // TODO
    uint8_t num = 1;
#endif
    strcpy(&filename[sizeof(LOGS_PATH)], STR_MODEL);
    filename[sizeof(LOGS_PATH) + PSIZE(TR_MODEL)] = (char)((num / 10) + '0');
    filename[sizeof(LOGS_PATH) + PSIZE(TR_MODEL) + 1] = (char)((num % 10) + '0');
    len = sizeof(LOGS_PATH) + PSIZE(TR_MODEL) + 2;
  }

  char * tmp = &filename[len];

#if defined(RTCLOCK)
  tmp = strAppendDate(&filename[len]);
#endif

  strcpy(tmp, STR_LOGS_EXT);

  result = f_open(&g_oLogFile, filename, FA_OPEN_ALWAYS | FA_WRITE | FA_OPEN_APPEND);
  if (result != FR_OK) {
    return SDCARD_ERROR(result);
  }

  if (f_size(&g_oLogFile) == 0) {
    writeHeader();
  }

  return nullptr;
}

tmr10ms_t lastLogTime = 0;

void logsClose()
{
  if (sdMounted()) {
    if (f_close(&g_oLogFile) != FR_OK) {
      // close failed, forget file
      g_oLogFile.obj.fs = 0;
    }
    lastLogTime = 0;
  }
}


void writeHeader()
{
#if defined(RTCLOCK)
  f_puts("Date,Time,", &g_oLogFile);
#else
  f_puts("Time,", &g_oLogFile);
#endif


  char label[TELEM_LABEL_LEN+7];
  for (int i=0; i<MAX_TELEMETRY_SENSORS; i++) {
    if (isTelemetryFieldAvailable(i)) {
      TelemetrySensor & sensor = g_model.telemetrySensors[i];
      if (sensor.logs) {
        memset(label, 0, sizeof(label));
        zchar2str(label, sensor.label, TELEM_LABEL_LEN);
        uint8_t unit = sensor.unit;
        if (unit == UNIT_CELLS ) unit = UNIT_VOLTS;
        if (UNIT_RAW < unit && unit < UNIT_FIRST_VIRTUAL) {
          strcat(label, "(");
          strncat(label, STR_VTELEMUNIT+1+3*unit, 3);
          strcat(label, ")");
        }
        strcat(label, ",");
        f_puts(label, &g_oLogFile);
      }
    }
  }

#if defined(PCBTARANIS) || defined(PCBHORUS)
  for (uint8_t i=1; i<NUM_STICKS+NUM_POTS+NUM_SLIDERS+1; i++) {
    const char * p = STR_VSRCRAW + i * STR_VSRCRAW[0] + 2;
    for (uint8_t j=0; j<STR_VSRCRAW[0]-1; ++j) {
      if (!*p) break;
      f_putc(*p, &g_oLogFile);
      ++p;
    }
    f_putc(',', &g_oLogFile);
  }

  for (uint8_t i=0; i<NUM_SWITCHES; i++) {
    if (SWITCH_EXISTS(i)) {
      char s[LEN_SWITCH_NAME + 2];
      char * temp;
      temp = getSwitchName(s, SWSRC_FIRST_SWITCH + i * 3);
      *temp++ = ',';
      *temp = '\0';
      f_puts(s, &g_oLogFile);
    }
  }
  f_puts("LSW,", &g_oLogFile);
#else
  f_puts("Rud,Ele,Thr,Ail,P1,P2,P3,THR,RUD,ELE,3POS,AIL,GEA,TRN,", &g_oLogFile);
#endif

  f_puts("TxBat(V)\n", &g_oLogFile);
}

uint32_t getLogicalSwitchesStates(uint8_t first)
{
  uint32_t result = 0;
  for (uint8_t i=0; i<32; i++) {
    result |= (getSwitch(SWSRC_FIRST_LOGICAL_SWITCH+first+i) << i);
  }
  return result;
}

void logsWrite()
{
  static const char * error_displayed = nullptr;

  if (!sdMounted()) {
    return;
  }

  if (isFunctionActive(FUNCTION_LOGS) && logDelay > 0) {
    tmr10ms_t tmr10ms = get_tmr10ms();
    if (lastLogTime == 0 || (tmr10ms_t)(tmr10ms - lastLogTime) >= (tmr10ms_t)logDelay*10) {
      lastLogTime = tmr10ms;

      if (!g_oLogFile.obj.fs) {
        const char * result = logsOpen();
        if (result) {
          if (result != error_displayed) {
            error_displayed = result;
            POPUP_WARNING(result);
          }
          return;
        }
      }

#if defined(RTCLOCK)
      {
        static struct gtm utm;
        static gtime_t lastRtcTime = 0;
        if (g_rtcTime != lastRtcTime) {
          lastRtcTime = g_rtcTime;
          gettime(&utm);
        }
        f_printf(&g_oLogFile, "%4d-%02d-%02d,%02d:%02d:%02d.%02d0,", utm.tm_year+TM_YEAR_BASE, utm.tm_mon+1, utm.tm_mday, utm.tm_hour, utm.tm_min, utm.tm_sec, g_ms100);
      }
#else
      f_printf(&g_oLogFile, "%d,", tmr10ms);
#endif

      for (int i=0; i<MAX_TELEMETRY_SENSORS; i++) {
        if (isTelemetryFieldAvailable(i)) {
          TelemetrySensor & sensor = g_model.telemetrySensors[i];
          TelemetryItem & telemetryItem = telemetryItems[i];
          if (sensor.logs) {
            if (sensor.unit == UNIT_GPS) {
              if (telemetryItem.gps.longitude && telemetryItem.gps.latitude) {
                div_t qr = div((int)telemetryItem.gps.latitude, 1000000);
                if (telemetryItem.gps.latitude < 0) f_printf(&g_oLogFile, "-");
                f_printf(&g_oLogFile, "%d.%06d ", abs(qr.quot), abs(qr.rem));
                qr = div((int)telemetryItem.gps.longitude, 1000000);
                if (telemetryItem.gps.longitude < 0) f_printf(&g_oLogFile, "-");
                f_printf(&g_oLogFile, "%d.%06d,", abs(qr.quot), abs(qr.rem));
              }
              else {
                f_printf(&g_oLogFile, ",");
              }
            }
            else if (sensor.unit == UNIT_DATETIME) {
              f_printf(&g_oLogFile, "%4d-%02d-%02d %02d:%02d:%02d,", telemetryItem.datetime.year, telemetryItem.datetime.month, telemetryItem.datetime.day, telemetryItem.datetime.hour, telemetryItem.datetime.min, telemetryItem.datetime.sec);
            }
            else if (sensor.prec == 2) {
              div_t qr = div((int)telemetryItem.value, 100);
              if (telemetryItem.value < 0) f_printf(&g_oLogFile, "-");
              f_printf(&g_oLogFile, "%d.%02d,", abs(qr.quot), abs(qr.rem));
            }
            else if (sensor.prec == 1) {
              div_t qr = div((int)telemetryItem.value, 10);
              if (telemetryItem.value < 0) f_printf(&g_oLogFile, "-");
              f_printf(&g_oLogFile, "%d.%d,", abs(qr.quot), abs(qr.rem));
            }
            else {
              f_printf(&g_oLogFile, "%d,", telemetryItem.value);
            }
          }
        }
      }

      for (uint8_t i=0; i<NUM_STICKS+NUM_POTS+NUM_SLIDERS; i++) {
        f_printf(&g_oLogFile, "%d,", calibratedAnalogs[i]);
      }

#if defined(PCBTARANIS) || defined(PCBHORUS)
      for (uint8_t i=0; i<NUM_SWITCHES; i++) {
        if (SWITCH_EXISTS(i)) {
          f_printf(&g_oLogFile, "%d,", getSwitchState(i));
        }
      }
      f_printf(&g_oLogFile, "0x%08X%08X,", getLogicalSwitchesStates(32), getLogicalSwitchesStates(0));
#else
      f_printf(&g_oLogFile, "%d,%d,%d,%d,%d,%d,%d,",
          GET_2POS_STATE(THR),
          GET_2POS_STATE(RUD),
          GET_2POS_STATE(ELE),
          GET_3POS_STATE(ID),
          GET_2POS_STATE(AIL),
          GET_2POS_STATE(GEA),
          GET_2POS_STATE(TRN));
#endif

      div_t qr = div(g_vbat100mV, 10);
      int result = f_printf(&g_oLogFile, "%d.%d\n", abs(qr.quot), abs(qr.rem));

      if (result<0 && !error_displayed) {
        error_displayed = STR_SDCARD_ERROR;
        POPUP_WARNING(STR_SDCARD_ERROR);
        logsClose();
      }
    }
  }
  else {
    error_displayed = nullptr;
    if (g_oLogFile.obj.fs) {
      logsClose();
    }
  }
}
