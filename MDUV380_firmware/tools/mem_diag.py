#!/usr/bin/env python3
"""Read the fork's runtime memory-diagnostics counters over USB CPS (opcode
0xB4, потребує прошивки, зібраної з ENABLE_MEM_DIAG=1 -- типова збірка цей
опкод не має, і рація відповість звичайним "-").

Навіщо: docs/recording-feasibility.md п.1.3 мав лише СТАТИЧНИЙ здогад про
вільну RAM/CCMRAM (tools/mem_report.py рахує його з .map/.elf у CI). Цей
скрипт -- друга половина: РЕАЛЬНИЙ запас під навантаженням (активний прийом),
знятий прямо з рації через FreeRTOS xPortGetFreeHeapSize() /
uxTaskGetStackHighWaterMark().

Формат відповіді на 0xB4 (див. usb_com.c, case 0xB4):
  [cmd, len_hi, len_lo, 5x uint32 LE]
    d0 = xPortGetFreeHeapSize()                    -- вільно в купі FreeRTOS, Б
    d1 = configTOTAL_HEAP_SIZE                     -- розмір купи FreeRTOS, Б
    d2 = uxTaskGetStackHighWaterMark(hrc6000Task)  -- запас стека, СЛОВА
    d3 = HRC6000_TASK_STACK_WORDS                  -- виділено стека hrc6000Task, слова
    d4 = uxTaskGetStackHighWaterMark(NULL)         -- запас стека головної задачі, слова
         (опкод обробляється саме на ній -- applicationMainTask())

Слово стека (portSTACK_TYPE) на цій платі -- 4 Б (Cortex-M4, 32-біт); тому
d2/d3/d4 тут множаться на 4 для показу в байтах.

Вжиток:
    python3 tools/mem_diag.py            # разовий знімок
    python3 tools/mem_diag.py --watch 2  # знімок кожні 2 с (Ctrl+C -- вихід)

Запускати з Windows або WSL; автовизначення рації OpenGD77 (1FC9:0094), як
і в tools/sms_diag.py.
"""
import sys
import time
import struct
import serial
from serial.tools import list_ports

APP_VID, APP_PID = 0x1FC9, 0x0094
STACK_WORD_BYTES = 4  # portSTACK_TYPE на Cortex-M4 -- 4 Б


def find_port():
    for p in list_ports.comports():
        if p.vid == APP_VID and p.pid == APP_PID:
            return p.device
    return None


def read_once(ser):
    ser.reset_input_buffer()
    ser.write(bytes([ord("C"), 0xB4]))
    ser.flush()
    time.sleep(0.2)
    r = ser.read(64)
    if len(r) < 3 + 20 or r[0] != ord("C"):
        return None, r
    n = (r[1] << 8) | r[2]
    if n < 20:
        return None, r
    d = struct.unpack_from("<5I", r, 3)
    return d, r


def print_report(d):
    free_heap, total_heap, hrc_stack_free_w, hrc_stack_total_w, main_stack_free_w = d

    used_heap = total_heap - free_heap
    pct_heap = (used_heap * 100.0 / total_heap) if total_heap else 0.0
    print("Купа FreeRTOS:  вільно %5d Б  зайнято %5d / %5d Б (%.1f%%)"
          % (free_heap, used_heap, total_heap, pct_heap))

    hrc_free_b = hrc_stack_free_w * STACK_WORD_BYTES
    hrc_total_b = hrc_stack_total_w * STACK_WORD_BYTES
    pct_hrc = (100.0 - hrc_stack_free_w * 100.0 / hrc_stack_total_w) if hrc_stack_total_w else 0.0
    print("Стек hrc6000Task: запас %5d слів (%5d Б) з %5d слів (%5d Б) виділено -- зайнято %.1f%%"
          % (hrc_stack_free_w, hrc_free_b, hrc_stack_total_w, hrc_total_b, pct_hrc))

    main_free_b = main_stack_free_w * STACK_WORD_BYTES
    print("Стек головної задачі: запас %5d слів (%5d Б)" % (main_stack_free_w, main_free_b))

    if hrc_stack_free_w < hrc_stack_total_w * 0.1:
        print("  !!! запас стека hrc6000Task < 10% -- ризик переповнення під навантаженням.")
    if free_heap < total_heap * 0.1:
        print("  !!! вільна купа FreeRTOS < 10%.")


def main():
    port = find_port()
    if not port:
        sys.exit("radio not found (USB 1FC9:0094)")
    ser = serial.Serial(port, 115200, timeout=0.5)

    interval = None
    if "--watch" in sys.argv:
        i = sys.argv.index("--watch")
        try:
            interval = float(sys.argv[i + 1])
        except (IndexError, ValueError):
            sys.exit("вжиток: mem_diag.py --watch <секунд>")

    def one_shot():
        d, raw = read_once(ser)
        if d is None:
            print("немає відповіді або несподіваний формат (%d Б): %s -- "
                  "перевір, що прошивка зібрана з ENABLE_MEM_DIAG=1" % (len(raw), raw.hex()))
            return False
        print_report(d)
        return True

    if interval is None:
        one_shot()
        return

    try:
        while True:
            ts = time.strftime("%H:%M:%S")
            print("--- %s ---" % ts)
            one_shot()
            print()
            time.sleep(interval)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
