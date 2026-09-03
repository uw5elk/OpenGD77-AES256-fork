#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
OpenGD77-AES256-fork -- простий GUI-прошивальник для тестування "з коробки".

Ідея: не кожен тестер має бажання/навички ставити Python, pip-пакети, копіювати
libusb-1.0.dll і пам'ятати два різні VID:PID (звичайний режим і DFU). Цей файл --
тонка обгортка з вікном і двома кнопками навколо вже перевіреного коду
opengd77_stm32_firmware_loader.py та dmr_reboot_dfu.py. Жоден протокольний код
(DFU-команди, злиття кодека) тут НЕ дублюється і не переписується -- обидва модулі
імпортуються як є, щоб GUI і CLI завжди прошивали побайтово однаково.

Збирається в один портативний .exe через PyInstaller на GitHub Actions
(windows-latest) -- дивись job build-gui-flasher у .github/workflows/build.yml.
При збірці поруч кладуться:
  - openuv380-10w.bin      -- поточна українська прошивка (щоб не якати ЇЇ окремо);
  - libusb-1.0.dll         -- бо в .exe немає "сусіднього python.exe в PATH",
                               на який можна покластись, як при ручному запуску;
  - build_info.txt         -- короткий хеш коміту й дата збірки (для звітів "у мене
                               не працює" -- видно ЯКУ саме збірку тестували).

Драйвер WinUSB для режиму DFU (0483:DF11) цей інструмент НЕ ставить -- це
обмеження Windows, а не наше: без нього жоден інструмент (ні CLI, ні цей GUI) не
побачить рацію в завантажувачі. Раз на комп'ютер це робиться вручну через Zadig
(кнопка "Драйвер DFU (Zadig)" відкриє сторінку із завантаженням).
"""

import os
import sys
import glob
import time
import queue
import threading
import webbrowser
import configparser
from pathlib import Path

import tkinter as tk
from tkinter import ttk, filedialog, messagebox, scrolledtext

# --- Перевірені модулі проєкту, що роблять усю фактичну роботу ---------------------
import opengd77_stm32_firmware_loader as loader
import dmr_reboot_dfu as reboot

try:
    import serial  # той самий пакет, яким уже користується dmr_reboot_dfu.py
except ImportError:
    serial = None


CONFIG_FILENAME = str(Path.home() / ".gd77firmwareloader.ini")
ZADIG_URL = "https://zadig.akeo.ie/"


def resource_path(name):
    """Шлях до файлу, вбудованого поруч зі скомпільованим .exe (PyInstaller кладе
    їх у тимчасову теку sys._MEIPASS), або поруч зі скриптом при запуску з джерела
    (для розробки/тестування без збірки .exe)."""
    base = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(base, name)


def load_usb_backend():
    """У .exe не можна покладатись на автопошук libusb-1.0.dll (немає "сусіднього
    python.exe в PATH", як при ручному запуску) -- підключаємо вбудовану копію явно.
    Якщо запущено з джерела і dll поруч немає -- повертаємо None, pyusb сам
    спробує знайти системну бібліотеку (як завжди)."""
    try:
        import usb.backend.libusb1
    except ImportError:
        return None

    dll = resource_path("libusb-1.0.dll")
    if os.path.isfile(dll):
        return usb.backend.libusb1.get_backend(find_library=lambda x: dll)
    return None


def find_bundled_firmware():
    """Прошивка, вбудована поруч зі скомпільованим .exe."""
    matches = sorted(glob.glob(resource_path("openuv380-10w*.bin")))
    return matches[0] if matches else None


def read_build_info():
    """Короткий підпис збірки (коміт+дата), якщо CI його поклав поруч. Використовуй
    цей рядок у звітах про проблеми -- він каже, ЯКУ саме збірку тестували."""
    path = resource_path("build_info.txt")
    if os.path.isfile(path):
        try:
            return open(path, encoding="utf-8").read().strip()
        except OSError:
            pass
    return "збірка з джерела (без build_info.txt)"


class QueueWriter:
    """Перехоплює print() з бібліотечного коду (loader/reboot друкують прямо в
    stdout, це нормально для CLI) і жене рядки в чергу для показу у вікні -- без
    цього фонового потоку GUI-текст і бібліотечний текст переплутались би між
    собою, а Tkinter не можна безпечно оновлювати з чужого потоку напряму."""

    def __init__(self, q):
        self.q = q
        self._buf = ""

    def write(self, s):
        self._buf += s
        while "\n" in self._buf or "\r" in self._buf:
            sep = "\n" if "\n" in self._buf else "\r"
            line, self._buf = self._buf.split(sep, 1)
            line = line.strip()
            if line:
                self.q.put(("log", line))

    def flush(self):
        pass


class FlasherApp(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("OpenGD77 -- Прошивка (тест)")
        self.geometry("560x520")
        self.minsize(520, 460)

        self.queue = queue.Queue()
        self.backend = load_usb_backend()
        self.flashing = False

        self.donor_path = self._load_saved_donor()
        self.firmware_path = find_bundled_firmware()
        self.build_info = read_build_info()

        self._build_ui()
        self._refresh_donor_label()
        self._refresh_firmware_label()
        self.after(50, self._poll_queue)

    # --- інтерфейс -------------------------------------------------------------

    def _build_ui(self):
        pad = {"padx": 12, "pady": 6}

        header = ttk.Label(self, text="OpenGD77 -- прошивка MD-UV390",
                            font=("Segoe UI", 13, "bold"))
        header.pack(anchor="w", **pad)

        info = ttk.Label(self, text=self.build_info, foreground="#555555")
        info.pack(anchor="w", padx=12)

        # --- Крок 1: донор кодека ---
        donor_frame = ttk.LabelFrame(self, text="1. Донор кодека AMBE (файл MD9600 V5)")
        donor_frame.pack(fill="x", **pad)

        self.donor_label = ttk.Label(donor_frame, text="", wraplength=500, justify="left")
        self.donor_label.pack(anchor="w", padx=8, pady=(6, 2))

        ttk.Button(donor_frame, text="Обрати файл донора...",
                   command=self._pick_donor).pack(anchor="w", padx=8, pady=(0, 8))

        # --- Крок 2: прошивка ---
        fw_frame = ttk.LabelFrame(self, text="2. Прошивка")
        fw_frame.pack(fill="x", **pad)

        self.firmware_label = ttk.Label(fw_frame, text="", wraplength=500, justify="left")
        self.firmware_label.pack(anchor="w", padx=8, pady=(6, 2))

        ttk.Button(fw_frame, text="Обрати інший файл прошивки...",
                   command=self._pick_firmware).pack(anchor="w", padx=8, pady=(0, 8))

        # --- Крок 3: прошити ---
        action_frame = ttk.Frame(self)
        action_frame.pack(fill="x", **pad)

        self.flash_button = ttk.Button(action_frame, text="ПРОШИТИ",
                                       command=self._on_flash_clicked)
        self.flash_button.pack(side="left")

        self.zadig_button = ttk.Button(action_frame, text="Драйвер DFU (Zadig)",
                                       command=lambda: webbrowser.open(ZADIG_URL))
        self.zadig_button.pack(side="left", padx=8)

        self.progress = ttk.Progressbar(self, mode="determinate", maximum=100)
        self.progress.pack(fill="x", padx=12, pady=(0, 6))

        self.status_label = ttk.Label(self, text="Готово до роботи.", foreground="#0a6b2a")
        self.status_label.pack(anchor="w", padx=12)

        # --- журнал ---
        log_frame = ttk.LabelFrame(self, text="Журнал")
        log_frame.pack(fill="both", expand=True, **pad)

        self.log_text = scrolledtext.ScrolledText(log_frame, height=10, state="disabled",
                                                    font=("Consolas", 9))
        self.log_text.pack(fill="both", expand=True, padx=6, pady=6)

    # --- донор -------------------------------------------------------------

    def _load_saved_donor(self):
        """Читаємо той самий ini-файл, яким користується CLI-прошивальник
        (~/.gd77firmwareloader.ini) -- обраний тут донор одразу бачить і CLI,
        і навпаки, обраний у CLI одразу підхоплюється тут."""
        cfg = configparser.ConfigParser()
        cfg.read(CONFIG_FILENAME)
        loader.config = cfg  # patch_and_download_firmware() читає саме цю глобальну змінну
        try:
            path = cfg["GLOBAL"]["SourceSTM32Firmware"]
        except KeyError:
            path = ""
        return path if (path and os.path.isfile(path)) else ""

    def _save_donor(self, path):
        cfg = getattr(loader, "config", None) or configparser.ConfigParser()
        cfg.read(CONFIG_FILENAME)
        if "GLOBAL" not in cfg:
            cfg["GLOBAL"] = {}
        cfg["GLOBAL"]["SourceSTM32Firmware"] = path
        with open(CONFIG_FILENAME, "w") as f:
            cfg.write(f)
        loader.config = cfg

    def _pick_donor(self):
        path = filedialog.askopenfilename(
            title="Обери файл донора кодека (MD9600-CSV(2571V5)-V26.45.bin)",
            filetypes=[("Файл прошивки", "*.bin"), ("Усі файли", "*.*")],
        )
        if not path:
            return

        checksum = loader.GetSHA256Checksum(path)
        if checksum != loader.FW2645_SHA256_Checksum:
            messagebox.showerror(
                "Не той файл",
                "Контрольна сума не збігається з донором MD9600-CSV(2571V5)-V26.45.bin.\n\n"
                "Це або інший файл, або пошкоджене завантаження. З ним DMR не запрацює -- "
                "оберіть правильний файл, або натисніть \"ПРОШИТИ\" без донора для тесту "
                "тільки FM-частини.",
            )
            return

        self.donor_path = path
        self._save_donor(path)
        self._refresh_donor_label()

    def _refresh_donor_label(self):
        if self.donor_path:
            self.donor_label.config(
                text="Донор перевірено: " + self.donor_path, foreground="#0a6b2a"
            )
        else:
            self.donor_label.config(
                text="Донор не обрано -- прошивка піде тільки в режимі FM, без DMR.",
                foreground="#8a5300",
            )

    # --- прошивка (файл) ----------------------------------------------------

    def _pick_firmware(self):
        path = filedialog.askopenfilename(
            title="Обери файл прошивки (.bin)",
            filetypes=[("Файл прошивки", "*.bin"), ("Усі файли", "*.*")],
        )
        if not path:
            return
        self.firmware_path = path
        self._refresh_firmware_label()

    def _refresh_firmware_label(self):
        if self.firmware_path:
            name = os.path.basename(self.firmware_path)
            bundled = " (вбудована)" if self.firmware_path == find_bundled_firmware() else ""
            self.firmware_label.config(
                text="Прошивка: " + name + bundled, foreground="#0a6b2a"
            )
        else:
            self.firmware_label.config(
                text="Файл прошивки не знайдено -- обери вручну.", foreground="#b00000"
            )

    # --- лог/прогрес ---------------------------------------------------------

    def _log(self, text):
        self.log_text.config(state="normal")
        self.log_text.insert("end", text + "\n")
        self.log_text.see("end")
        self.log_text.config(state="disabled")

    def _poll_queue(self):
        try:
            while True:
                kind, payload = self.queue.get_nowait()
                if kind == "log":
                    self._log(payload)
                elif kind == "progress":
                    self.progress["value"] = payload
                elif kind == "done":
                    self._on_flash_finished(*payload)
        except queue.Empty:
            pass
        self.after(50, self._poll_queue)

    # --- сама прошивка (у фоновому потоці) ------------------------------------

    def _on_flash_clicked(self):
        if self.flashing:
            return

        if not self.firmware_path or not os.path.isfile(self.firmware_path):
            messagebox.showerror("Немає прошивки", "Спершу оберіть файл прошивки.")
            return

        if not self.donor_path:
            if not messagebox.askyesno(
                "Без донора кодека",
                "Донор кодека не обрано або не пройшов перевірку.\n\n"
                "Прошити БЕЗ DMR (тільки FM, для тесту решти змін)?",
            ):
                return

        if not messagebox.askokcancel(
            "Прошити рацію?",
            "Переконайся, що USB-кабель підключено, а рація увімкнена.\n\n"
            "Прошивка займе приблизно хвилину. Не від'єднуй кабель, поки не "
            "з'явиться повідомлення \"Готово\".",
        ):
            return

        self.flashing = True
        self.flash_button.config(state="disabled")
        self.progress["value"] = 0
        self.status_label.config(text="Прошиваю...", foreground="#14506b")
        self._log("=" * 40)
        self._log("Починаю прошивку: " + os.path.basename(self.firmware_path))

        thread = threading.Thread(target=self._flash_worker, daemon=True)
        thread.start()

    def _connect_kwargs(self):
        kwargs = {"idVendor": loader.defaultVID, "idProduct": loader.defaultPID}
        if self.backend is not None:
            kwargs["backend"] = self.backend
        return kwargs

    def _try_connect(self):
        try:
            loader.init(**self._connect_kwargs())
            return True
        except Exception:
            return False

    def _try_auto_reboot_to_dfu(self):
        """Якщо рація ще у звичайному режимі (не в DFU), автоматично переводимо її
        туди самою тільки командою по USB -- так само, як робить окремий
        dmr_reboot_dfu.py. Спрацьовує тільки на збірках з ENABLE_DMR_DATA=1; якщо
        рація вже в DFU (переведена вручну кнопками) або команда не спрацювала --
        нічого страшного, просто повертаємо False і пробуємо підключитись напряму."""
        if serial is None:
            return False

        port = reboot.find_port()
        if not port:
            return False

        self.queue.put(("log", "Рація у звичайному режимі, переводжу в DFU автоматично..."))
        try:
            with serial.Serial(port, 115200, timeout=0.5) as ser:
                ser.reset_input_buffer()
                ser.write(bytes([reboot.CMD, reboot.SUB_REBOOT_DFU]))
                ser.flush()
        except serial.SerialException:
            pass  # порт міг зникнути, бо рація вже почала перезавантажуватись -- це очікувано

        time.sleep(2.0)
        return True

    def _flash_worker(self):
        old_stdout = sys.stdout
        sys.stdout = QueueWriter(self.queue)
        ok = False
        reason = None
        try:
            connected = self._try_connect()

            if not connected:
                self._try_auto_reboot_to_dfu()
                connected = self._try_connect()

            if not connected:
                reason = "no_dfu"
            else:
                def progress_cb(addr, offset, size, strPrefix=None):
                    pct = int(offset * 100 / size) if size else 0
                    self.queue.put(("progress", pct))

                loader.patch_and_download_firmware(
                    self.firmware_path, None, loader.FWPlatformOutput.MD_UV380, progress_cb
                )
                ok = True
        except SystemExit as e:
            reason = "exit_{}".format(e.code)
        except Exception as e:  # noqa: BLE001 -- показуємо будь-яку помилку користувачу, не ховаємо
            reason = "exception: {}".format(e)
        finally:
            sys.stdout = old_stdout
            self.queue.put(("done", (ok, reason)))

    def _on_flash_finished(self, ok, reason):
        self.flashing = False
        self.flash_button.config(state="normal")

        if ok:
            self.progress["value"] = 100
            self.status_label.config(text="Готово! Можна від'єднати USB.", foreground="#0a6b2a")
            self._log("*** Готово.")
            messagebox.showinfo("Готово", "Прошивку завершено. Можна від'єднати USB і "
                                           "увімкнути рацію звичайним способом.")
            return

        self.status_label.config(text="Не вдалося прошити.", foreground="#b00000")

        if reason == "no_dfu":
            self._log("!!! Рацію не знайдено в режимі DFU.")
            messagebox.showerror(
                "Рацію не знайдено",
                "Не вдалося підключитись до рації в режимі DFU.\n\n"
                "Найчастіші причини:\n"
                "1. USB-кабель не підключено або рація вимкнена.\n"
                "2. Це перший запуск на цьому комп'ютері -- Windows ще не має "
                "драйвера для режиму DFU. Натисни кнопку \"Драйвер DFU (Zadig)\", "
                "постав Zadig, тоді запусти прошивку ще раз (Zadig працює тільки "
                "коли рація вже в DFU -- увімкни її, утримуючи бічні кнопки, якщо "
                "автоматичний перехід не спрацював).\n"
                "3. Стара прошивка на рації не вміє автоматично йти в DFU -- "
                "увімкни рацію вручну, утримуючи бічні кнопки, і спробуй ще раз.",
            )
        else:
            self._log("!!! Помилка: {}".format(reason))
            messagebox.showerror("Помилка прошивки", "Щось пішло не так: {}\n\n"
                                  "Подробиці -- у журналі внизу вікна.".format(reason))


def main():
    try:
        app = FlasherApp()
        app.mainloop()
    except Exception as e:  # noqa: BLE001 -- останній рубіж: показати хоч щось, а не мовчки згаснути
        import traceback
        detail = traceback.format_exc()
        try:
            messagebox.showerror("Помилка запуску", "{}\n\n{}".format(e, detail))
        except Exception:
            try:
                import ctypes
                ctypes.windll.user32.MessageBoxW(
                    0, "{}\n\n{}".format(e, detail), "OpenGD77 -- Помилка запуску", 0x10
                )
            except Exception:
                pass
        sys.exit(1)


if __name__ == "__main__":
    main()
