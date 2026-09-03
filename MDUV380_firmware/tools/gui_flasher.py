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
import aes_key_store            # CPS-протокол (flash_read/flash_write_block/find_port) --
                                 # той самий, перевірений код, що й у CLI-версії
import stock_key_table as skt   # wrap/unwrap-логіка стокової таблиці ключів (без USB)
import custom_data as cd        # безпечний read/write custom-data блоків (тема/AES-селектор/
                                 # RCTL і т.д. в ОДНОМУ регіоні -- див. коментар у custom_data.py)
import rctl_config as rctl      # формат блоку "RCTL" (allowlist віддаленого керування)

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


def enable_entry_clipboard(entry):
    """Вмикає копіювання/вставляння/вирізання в текстовому полі Tkinter так, щоб
    воно ГАРАНТОВАНО працювало на Windows з українською (чи будь-якою нелатинською)
    розкладкою клавіатури.

    Проблема, яку це виправляє: стандартні прив'язки Ctrl+C/Ctrl+V у Tk на Windows
    спрацьовують за КОДОМ СИМВОЛУ, який дає поточна розкладка для фізичної клавіші --
    а не за самою фізичною клавішею. З українською розкладкою фізична клавіша "V"
    видає не 'v', а кирилицю ("м"), тому Ctrl+V не розпізнається як вставляння і
    поле здається "заблокованим для вставки", хоча насправді просто не спрацьовує
    прив'язка. Пункту контекстного меню (по правій кнопці миші) в ttk.Entry також
    немає за замовчуванням -- тому користувачу взагалі нема як вставити ключ.

    Рішення -- два незалежні шляхи, кожен обходить проблему розкладки по-своєму:
      1) Контекстне меню правої кнопки миші (Вирізати/Копіювати/Вставити/Виділити
         все) -- клік мишею не залежить від розкладки клавіатури взагалі, тому
         працює завжди, на будь-якій ОС і розкладці.
      2) Прив'язка по event.keycode (апаратний код фізичної клавіші), а не по
         event.keysym (символ, залежний від розкладки) -- Ctrl+V/C/X/A тепер
         спрацьовують незалежно від того, яка розкладка активна. Коди клавіш V/C/X/A
         однакові на Windows (віртуальні коди VK_*) і на X11/Linux (evdev-коди) для
         звичайної розкладки QWERTY-сумісної фізичної клавіатури.
    """
    # --- 1) контекстне меню правої кнопки миші --------------------------------
    menu = tk.Menu(entry, tearoff=0)
    menu.add_command(label="Вирізати", command=lambda: entry.event_generate("<<Cut>>"))
    menu.add_command(label="Копіювати", command=lambda: entry.event_generate("<<Copy>>"))
    menu.add_command(label="Вставити", command=lambda: entry.event_generate("<<Paste>>"))
    menu.add_separator()
    menu.add_command(label="Виділити все", command=lambda: entry.selection_range(0, "end"))

    def _show_context_menu(event):
        try:
            menu.tk_popup(event.x_root, event.y_root)
        finally:
            menu.grab_release()

    entry.bind("<Button-3>", _show_context_menu)

    # --- 2) Ctrl+V/C/X/A за фізичним кодом клавіші, а не символом розкладки ---
    # 'V','C','X','A' -- віртуальні коди Windows (VK_*) і типові X11/evdev-коди
    # тих самих фізичних клавіш на PC-клавіатурі.
    KEYCODES_PASTE = (86, 118, 47, 55)
    KEYCODES_COPY = (67, 99, 54, 25)
    KEYCODES_CUT = (88, 120, 53, 45)
    KEYCODES_SELECT_ALL = (65, 97, 38, 24)

    def _on_ctrl_key(event):
        kc = event.keycode
        if kc in KEYCODES_PASTE:
            entry.event_generate("<<Paste>>")
            return "break"
        if kc in KEYCODES_COPY:
            entry.event_generate("<<Copy>>")
            return "break"
        if kc in KEYCODES_CUT:
            entry.event_generate("<<Cut>>")
            return "break"
        if kc in KEYCODES_SELECT_ALL:
            entry.selection_range(0, "end")
            return "break"
        return None

    entry.bind("<Control-Key>", _on_ctrl_key)


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

        self.aes_button = ttk.Button(action_frame, text="Керування AES-ключами...",
                                     command=self._open_aes_key_manager)
        self.aes_button.pack(side="left", padx=8)

        self.rctl_button = ttk.Button(action_frame, text="Віддалене керування (RCTL)...",
                                      command=self._open_rctl_config)
        self.rctl_button.pack(side="left", padx=8)

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

    # --- AES-ключі (окреме вікно -- інший стан USB, рація НЕ в DFU) -----------

    def _open_aes_key_manager(self):
        AesKeyManagerWindow(self)

    def _open_rctl_config(self):
        RctlConfigWindow(self)

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


class AesKeyManagerWindow(tk.Toplevel):
    """Окреме вікно для додавання/видалення AES-256 ключів шифрування голосу.

    ІНША фізична фаза USB, ніж прошивка: рація тут має бути у ЗВИЧАЙНОМУ
    робочому режимі (VID:PID 1fc9:0094, той самий CPS-протокол, яким
    користується офіційна CPS-програма) -- НЕ в DFU-завантажувачі. Тому це
    окреме вікно, а не ще один розділ у вікні прошивки: змішувати два різні
    очікувані стани рації в одному екрані тільки б плутало користувача.

    Ключі зберігаються у форматі стокової таблиці TYT (не в кастомних даних
    OpenGD77) -- див. tools/stock_key_table.py й коментар при
    STOCK_KEY_TABLE_BASE у dmr_aes_hook.c. Сам протокольний код читання/запису
    флеш (flash_read/flash_write_block/find_port) береться з aes_key_store.py
    як є -- нічого не дублюється."""

    def __init__(self, parent):
        super().__init__(parent)
        self.title("OpenGD77 -- AES-ключі шифрування")
        self.geometry("480x520")
        self.minsize(440, 460)

        self.queue = queue.Queue()
        self.busy = False

        self._build_ui()
        self.after(50, self._poll_queue)

    # --- інтерфейс -------------------------------------------------------------

    def _build_ui(self):
        pad = {"padx": 12, "pady": 6}

        note = ttk.Label(
            self,
            text=("Рація має бути УВІМКНЕНА У ЗВИЧАЙНОМУ РЕЖИМІ (не в DFU!). "
                  "Матеріал ключа ніде на екрані не показується -- лише факт "
                  "\"зайнято/порожньо\"."),
            wraplength=440, justify="left", foreground="#8a5300",
        )
        note.pack(anchor="w", **pad)

        key_frame = ttk.LabelFrame(self, text="Слот ключа (1..15)")
        key_frame.pack(fill="x", **pad)

        row1 = ttk.Frame(key_frame)
        row1.pack(fill="x", padx=8, pady=(6, 2))
        ttk.Label(row1, text="ID ключа:").pack(side="left")
        self.keyid_var = tk.IntVar(value=1)
        ttk.Spinbox(row1, from_=1, to=15, width=4, textvariable=self.keyid_var).pack(side="left", padx=6)

        row2 = ttk.Frame(key_frame)
        row2.pack(fill="x", padx=8, pady=2)
        ttk.Label(row2, text="Ключ (64 hex):").pack(side="left")
        self.key_var = tk.StringVar()
        self.key_entry = ttk.Entry(row2, textvariable=self.key_var, width=40, font=("Consolas", 9))
        self.key_entry.pack(side="left", padx=6, fill="x", expand=True)
        # Явно вмикаємо вставку/копіювання -- інакше на Windows з українською
        # розкладкою клавіатури Ctrl+V у це поле не спрацьовує (див. докладний
        # коментар при enable_entry_clipboard() вище).
        enable_entry_clipboard(self.key_entry)

        self.show_var = tk.BooleanVar(value=True)
        ttk.Checkbutton(key_frame, text="показувати введений ключ", variable=self.show_var,
                        command=self._toggle_key_visibility).pack(anchor="w", padx=8)

        row3 = ttk.Frame(key_frame)
        row3.pack(fill="x", padx=8, pady=(2, 8))
        ttk.Button(row3, text="Записати ключ", command=self._on_write_key).pack(side="left")
        ttk.Button(row3, text="Очистити ключ", command=self._on_clear_key).pack(side="left", padx=8)

        tx_frame = ttk.LabelFrame(self, text="Активний ключ для передачі (TX)")
        tx_frame.pack(fill="x", **pad)
        row4 = ttk.Frame(tx_frame)
        row4.pack(fill="x", padx=8, pady=8)
        ttk.Label(row4, text="TX-ключ (0 = вимк.):").pack(side="left")
        self.txkey_var = tk.IntVar(value=0)
        ttk.Spinbox(row4, from_=0, to=15, width=4, textvariable=self.txkey_var).pack(side="left", padx=6)
        ttk.Button(row4, text="Встановити", command=self._on_set_tx_key).pack(side="left", padx=8)

        ttk.Button(self, text="Оновити список зайнятих слотів",
                  command=self._on_refresh_slots).pack(anchor="w", padx=12)

        self.status_label = ttk.Label(self, text="Готово.", foreground="#0a6b2a")
        self.status_label.pack(anchor="w", padx=12, pady=(4, 0))

        log_frame = ttk.LabelFrame(self, text="Журнал")
        log_frame.pack(fill="both", expand=True, **pad)
        self.log_text = scrolledtext.ScrolledText(log_frame, height=10, state="disabled",
                                                    font=("Consolas", 9))
        self.log_text.pack(fill="both", expand=True, padx=6, pady=6)

    def _toggle_key_visibility(self):
        self.key_entry.config(show="" if self.show_var.get() else "*")

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
                elif kind == "done":
                    self._on_worker_finished(*payload)
        except queue.Empty:
            pass
        self.after(50, self._poll_queue)

    # --- валідація вводу -------------------------------------------------------

    def _read_key32(self):
        text = self.key_var.get().strip().replace(" ", "")
        try:
            key = bytes.fromhex(text)
        except ValueError:
            messagebox.showerror("Невірний ключ", "Ключ має бути рядком із 64 шістнадцяткових символів (0-9, A-F).")
            return None
        if len(key) != 32:
            messagebox.showerror("Невірний ключ", "Ключ має бути рівно 64 hex-символи (32 байти), зараз {}.".format(len(text)))
            return None
        return key

    # --- дії користувача ---------------------------------------------------------

    def _on_write_key(self):
        if self.busy:
            return
        key32 = self._read_key32()
        if key32 is None:
            return
        key_id = self.keyid_var.get()
        if not messagebox.askokcancel(
            "Записати ключ?",
            "Записати ключ у слот {} на рації?\n\n"
            "Переконайся, що рація увімкнена у ЗВИЧАЙНОМУ режимі (не в DFU).".format(key_id),
        ):
            return
        self._run_worker(lambda: self._write_key_worker(key_id, key32),
                         "Записую ключ {}...".format(key_id))

    def _on_clear_key(self):
        if self.busy:
            return
        key_id = self.keyid_var.get()
        if not messagebox.askokcancel(
            "Очистити ключ?",
            "Стерти ключ у слоті {} на рації? Цю дію не можна скасувати.".format(key_id),
        ):
            return
        self._run_worker(lambda: self._clear_key_worker(key_id),
                         "Очищую ключ {}...".format(key_id))

    def _on_set_tx_key(self):
        if self.busy:
            return
        tx_id = self.txkey_var.get()
        self._run_worker(lambda: self._set_tx_key_worker(tx_id),
                         "Встановлюю активний TX-ключ = {}...".format(tx_id))

    def _on_refresh_slots(self):
        if self.busy:
            return
        self._run_worker(self._refresh_slots_worker, "Читаю зайняті слоти...")

    # --- фонові операції (у потоці) ------------------------------------------------

    def _connect(self):
        port = aes_key_store.find_port()
        if not port:
            raise RuntimeError(
                "рацію не знайдено у звичайному режимі (VID:PID 1fc9:0094). "
                "Переконайся, що вона увімкнена звичайним способом (НЕ в DFU) і кабель підключено."
            )
        ser = serial.Serial(port, 115200, timeout=0.6)
        aes_key_store.show_cps(ser)
        return ser

    def _write_key_worker(self, key_id, key32):
        with self._connect() as ser:
            addr = skt.entry_addr(key_id)
            existing = aes_key_store.flash_read(ser, addr, skt.STOCK_KEY_ENTRY_LEN)
            entry = skt.build_entry(key_id, key32, existing_entry100=existing if len(existing) == skt.STOCK_KEY_ENTRY_LEN else None)
            aes_key_store.flash_write_block(ser, addr, entry)
            readback = aes_key_store.flash_read(ser, addr, skt.STOCK_KEY_ENTRY_LEN)
            ok = (readback == entry)
            print("Ключ {}: записано, звірка {}.".format(key_id, "OK" if ok else "НЕЗБІГ (спробуй ще раз)"))

    def _clear_key_worker(self, key_id):
        with self._connect() as ser:
            addr = skt.entry_addr(key_id)
            existing = aes_key_store.flash_read(ser, addr, skt.STOCK_KEY_ENTRY_LEN)
            entry = skt.build_entry(key_id, skt.BLANK_KEY, existing_entry100=existing if len(existing) == skt.STOCK_KEY_ENTRY_LEN else None)
            aes_key_store.flash_write_block(ser, addr, entry)
            print("Ключ {}: очищено (позначено як порожній слот).".format(key_id))

    def _set_tx_key_worker(self, tx_id):
        with self._connect() as ser:
            payload = cd.read_block(ser, aes_key_store.TYPE_AES_KEYS, aes_key_store.AESK_BLOCK_LEN)
            payload = bytearray(payload) if payload else aes_key_store.fresh_payload()
            if payload[:4] != b"AESK":
                payload = bytearray(aes_key_store.fresh_payload())
            payload[5] = tx_id & 0xFF

            ok, msg = cd.write_block(ser, aes_key_store.TYPE_AES_KEYS, bytes(payload))
            if not ok:
                raise RuntimeError(msg)
            print("Активний TX-ключ встановлено: {}.".format(tx_id if tx_id else "вимкнено (0)"))

    def _refresh_slots_worker(self):
        with self._connect() as ser:
            occupied = []
            for key_id in range(skt.STOCK_KEY_MIN_ID, skt.STOCK_KEY_MAX_ID + 1):
                entry = aes_key_store.flash_read(ser, skt.entry_addr(key_id), skt.STOCK_KEY_ENTRY_LEN)
                if len(entry) != skt.STOCK_KEY_ENTRY_LEN:
                    continue
                entry_type, _name, wrapped = skt.parse_entry(entry)
                if entry_type in (skt.STOCK_KEY_TYPE_AES256, skt.STOCK_KEY_TYPE_AES256B):
                    if skt.is_key_present(skt.unwrap_key(wrapped)):
                        occupied.append(key_id)

            payload = cd.read_block(ser, aes_key_store.TYPE_AES_KEYS, aes_key_store.AESK_BLOCK_LEN)
            tx_id = payload[5] if payload else 0

            empty = [i for i in range(skt.STOCK_KEY_MIN_ID, skt.STOCK_KEY_MAX_ID + 1) if i not in occupied]
            print("Зайняті слоти: {}".format(", ".join(map(str, occupied)) if occupied else "немає"))
            print("Порожні слоти: {}".format(", ".join(map(str, empty)) if empty else "немає"))
            print("Активний TX-ключ: {}".format(tx_id if tx_id else "вимкнено (0)"))

    def _run_worker(self, fn, status_text):
        self.busy = True
        self.status_label.config(text=status_text, foreground="#14506b")
        self._log("=" * 30)
        self._log(status_text)

        def worker():
            old_stdout = sys.stdout
            sys.stdout = QueueWriter(self.queue)
            ok, reason = False, None
            try:
                fn()
                ok = True
            except Exception as e:  # noqa: BLE001 -- показуємо будь-яку помилку, не ховаємо
                reason = str(e)
            finally:
                sys.stdout = old_stdout
                self.queue.put(("done", (ok, reason)))

        threading.Thread(target=worker, daemon=True).start()

    def _on_worker_finished(self, ok, reason):
        self.busy = False
        if ok:
            self.status_label.config(text="Готово.", foreground="#0a6b2a")
        else:
            self.status_label.config(text="Помилка.", foreground="#b00000")
            self._log("!!! Помилка: {}".format(reason))
            messagebox.showerror("Помилка", "Щось пішло не так: {}".format(reason))


class RctlConfigWindow(tk.Toplevel):
    """Увімк/вимк "приймати команди віддаленого керування" (RCTL --
    functions/dmr_rctl_cfg.c). Пише блок "RCTL" у custom-data (CHIRP-стиль:
    прошивка лише читає, сама не пише).

    Модель довіри (2026-09-03, за зразком Motorola/Hytera): БІНАРНА -- увімкнено
    означає "приймати команди від БУДЬ-КОГО з правильним AES-ключем каналу" (тим
    самим, що й голос/SMS), вимкнено -- не приймати ні від кого. Жодного окремого
    списку довірених ID тут немає -- сам канальний ключ і є межею довіри.

    ВАЖЛИВО: це вікно НЕ вміє надіслати саму команду Radio Check в ефір -- команда
    йде рація-рації по DMR, а не через USB/CPS. Тут лише перемикається "приймати
    чи ні" (fail closed: enabled=0 за замовчуванням, доки не увімкнено явно). Сам
    запит з рації -- окремий, ще не написаний пункт меню (PLANS.md §3)."""

    def __init__(self, parent):
        super().__init__(parent)
        self.title("OpenGD77 -- Віддалене керування (RCTL)")
        self.geometry("480x360")
        self.minsize(440, 320)

        self.queue = queue.Queue()
        self.busy = False

        self._build_ui()
        self.after(50, self._poll_queue)

    def _build_ui(self):
        pad = {"padx": 12, "pady": 6}

        note = ttk.Label(
            self,
            text=("Увімкнено = приймати команди RCTL (напр. Radio Check) від "
                  "БУДЬ-КОГО, хто знає ключ шифрування каналу -- як у Motorola/"
                  "Hytera. Вимкнено = не приймати ні від кого. Рація має бути "
                  "УВІМКНЕНА У ЗВИЧАЙНОМУ РЕЖИМІ (не в DFU)."),
            wraplength=440, justify="left", foreground="#8a5300",
        )
        note.pack(anchor="w", **pad)

        self.enabled_var = tk.BooleanVar(value=False)
        ttk.Checkbutton(self, text="Приймати команди RCTL на цій рації (enabled)",
                        variable=self.enabled_var).pack(anchor="w", padx=12, pady=(4, 0))

        btn_row = ttk.Frame(self)
        btn_row.pack(fill="x", **pad)
        ttk.Button(btn_row, text="Прочитати поточний стан", command=self._on_read).pack(side="left")
        ttk.Button(btn_row, text="Записати на рацію", command=self._on_write).pack(side="left", padx=8)

        self.status_label = ttk.Label(self, text="Натисни «Прочитати поточний стан».", foreground="#14506b")
        self.status_label.pack(anchor="w", padx=12, pady=(4, 0))

        log_frame = ttk.LabelFrame(self, text="Журнал")
        log_frame.pack(fill="both", expand=True, **pad)
        self.log_text = scrolledtext.ScrolledText(log_frame, height=8, state="disabled",
                                                    font=("Consolas", 9))
        self.log_text.pack(fill="both", expand=True, padx=6, pady=6)

    # --- лог/прогрес -------------------------------------------------------------

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
                elif kind == "state":
                    self._apply_state(payload)
                elif kind == "done":
                    self._on_worker_finished(*payload)
        except queue.Empty:
            pass
        self.after(50, self._poll_queue)

    def _apply_state(self, state):
        self.enabled_var.set(state["enabled"])

    # --- дії користувача -----------------------------------------------------------

    def _on_read(self):
        if self.busy:
            return
        self._run_worker(self._read_worker, "Читаю поточний стан...")

    def _on_write(self):
        if self.busy:
            return
        enabled = self.enabled_var.get()
        if enabled:
            if not messagebox.askokcancel(
                "Увімкнути RCTL?",
                "Ця рація прийматиме команди віддаленого керування від БУДЬ-КОГО, "
                "хто знає ключ шифрування каналу -- без окремого списку довірених "
                "ID (як у Motorola/Hytera). Продовжити?",
            ):
                return
        self._run_worker(lambda: self._write_worker(enabled), "Записую...")

    # --- фонові операції -----------------------------------------------------------

    def _connect(self):
        port = aes_key_store.find_port()
        if not port:
            raise RuntimeError(
                "рацію не знайдено у звичайному режимі (VID:PID 1fc9:0094). "
                "Переконайся, що вона увімкнена звичайним способом (НЕ в DFU) і кабель підключено."
            )
        ser = serial.Serial(port, 115200, timeout=0.6)
        aes_key_store.show_cps(ser)
        return ser

    def _read_worker(self):
        with self._connect() as ser:
            payload = cd.read_block(ser, rctl.TYPE_RCTL_CONFIG, rctl.PAYLOAD_LEN)
            state = rctl.parse_payload(payload) if payload else {"version": rctl.VERSION, "enabled": False}
            self.queue.put(("state", state))
            print("Стан: enabled={}".format(state["enabled"]))

    def _write_worker(self, enabled):
        with self._connect() as ser:
            payload = rctl.build_payload(enabled)
            ok, msg = cd.write_block(ser, rctl.TYPE_RCTL_CONFIG, payload)
            if not ok:
                raise RuntimeError(msg)
            rb = cd.read_block(ser, rctl.TYPE_RCTL_CONFIG, rctl.PAYLOAD_LEN)
            verify_ok = (rb == payload)
            print("Записано ({}), enabled={}. Звірка читанням: {}.".format(
                msg, enabled, "OK" if verify_ok else "НЕЗБІГ"))

    def _run_worker(self, fn, status_text):
        self.busy = True
        self.status_label.config(text=status_text, foreground="#14506b")
        self._log("=" * 30)
        self._log(status_text)

        def worker():
            old_stdout = sys.stdout
            sys.stdout = QueueWriter(self.queue)
            ok, reason = False, None
            try:
                fn()
                ok = True
            except Exception as e:  # noqa: BLE001
                reason = str(e)
            finally:
                sys.stdout = old_stdout
                self.queue.put(("done", (ok, reason)))

        threading.Thread(target=worker, daemon=True).start()

    def _on_worker_finished(self, ok, reason):
        self.busy = False
        if ok:
            self.status_label.config(text="Готово.", foreground="#0a6b2a")
        else:
            self.status_label.config(text="Помилка.", foreground="#b00000")
            self._log("!!! Помилка: {}".format(reason))
            messagebox.showerror("Помилка", "Щось пішло не так: {}".format(reason))


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
