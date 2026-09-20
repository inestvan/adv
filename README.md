# ADV7280M-CSI2 — CVBS → MIPI CSI-2 декодер для Raspberry Pi 4

Плата на Analog Devices **ADV7280WBCPZ-M** (LFCSP-32): композитный видеосигнал (PAL/NTSC/SECAM, разъём RCA)
преобразуется в поток MIPI CSI-2 (1 лента + клок) и подаётся на разъём камеры Raspberry Pi 4.
Работает со штатным overlay ядра Raspberry Pi без правок:

```
dtoverlay=adv728x-m,adv7280m=1
```

Проект сделан в KiCad 9 (`hw/`), плата 4 слоя 42 × 32 мм под стек JLCPCB JLC04161H-7628.
ERC и DRC — 0 ошибок, 0 предупреждений. Все решения и допущения по этапам — в `docs/assumptions.md`,
исходные требования и выжимка из документации — в `docs/requirements.md`.

![top](docs/render-top.png)

## Состав репозитория

| Путь | Что |
|---|---|
| `hw/adv7280m-csi2.kicad_pro / .kicad_sch / .kicad_pcb` | проект KiCad 9 |
| `hw/lib/` | символ ADV7280WBCPZ-M, символы питания, посадочные места FFC (Amphenol SFW15R-1STE1LF), RCA (PSG01546), X1SON |
| `docs/schematic.pdf`, `docs/bom.csv` | схема и BOM с кодами LCSC |
| `docs/render-top.png`, `docs/render-bottom.png`, `docs/layer-*.png` | 3D-рендеры и медь по слоям |
| `docs/datasheets/`, `docs/ref/` | даташит ADV7280 Rev. A, UG-637 Rev. A, AN-1260, схемы Pi 4 и Camera Module v2.1, overlay/драйвер из ядра Pi |
| `fab/adv7280m-csi2-gerber.zip` | Gerber (RS-274X, Protel-расширения) + Excellon drill + gbrjob со стеком |
| `fab/BOM_JLCPCB.csv`, `fab/CPL_JLCPCB.csv` | BOM и позиции в формате JLCPCB (только то, что собирает JLCPCB) |
| `tools/` | генераторы: `gen_lib.py` → `gen_fp.py` → `gen_sch.py` → `gen_pcb.py` → `route_pcb.py` → `write_pro.py`; `check_nets.py` проверяет нетлист |

Пересборка с нуля:

```
python3 tools/gen_lib.py && python3 tools/gen_fp.py && python3 tools/gen_sch.py && python3 tools/check_nets.py
cd hw && kicad-cli sch export netlist --format kicadxml -o adv7280m-csi2.xml adv7280m-csi2.kicad_sch && cd ..
python3 tools/gen_pcb.py && python3 tools/route_pcb.py && python3 tools/write_pro.py
```

## Схема в двух словах

* **Вход**: RCA J2 → ESD TPD1E10B06 → 22 Ω последовательно, 51 Ω на землю (терминация 73 Ω, делитель 0,7) → 100 нФ → AIN1.
* **Питание**: 3,3 В с пина 15 разъёма камеры. Два LDO AP2112K-1.8: U2 → DVDD+MVDD, U3 → AVDD, PVDD через феррит.
  У каждого вывода питания чипа 100 нФ + 10 нФ.
* **Кварц** 28.63636 МГц (YXC 5032, CL 20 пФ), нагрузочные 27 пФ.
* **MIPI**: D0P/D0N → пины 3/2 разъёма, CLKP/CLKN → пины 9/8; лента 1 не используется. Дифпары 100 Ω (0,18/0,15 мм),
  длины выровнены до 0,04 мм в паре и 0,03 мм между парами.
* **I²C**: SDA/SCL → пины 14/13, подтяжки 4,7 кОм (параллельно подтяжкам Pi). ALSB = 1 → 7-битный адрес **0x21**
  (8-битный 0x42 write / 0x43 read) — ровно то, что ждёт overlay.
* **RESET**: 10 кОм + 4,7 мкФ (τ ≈ 47 мс, низкий уровень ≥ 13 мс), **PWRDWN**: 10 кОм к 3,3 В — чип запускается сам, без участия Pi.

### Перемычки (паяльные, по умолчанию разомкнуты)

| | Замкнуть, если |
|---|---|
| JP1 `ALSB>GND` | нужен адрес 0x20; тогда в config.txt `dtoverlay=adv728x-m,adv7280m=1,addr=0x20` |
| JP2 `CAM_GPIO>RST` | хотите сбрасывать чип линией CAM_GPIO (пин 11 разъёма). Штатный overlay эту линию не включает, поэтому по умолчанию разомкнуто |
| JP3 `CAM_GPIO>PWRDWN` | хотите усыплять чип линией CAM_GPIO. То же самое: только со своим overlay, где включён регулятор `cam1_reg` |

### Тестовые точки

TP1 3V3 · TP2 1V8D · TP8 1V8A · TP3 SDA · TP4 SCL · TP5 RESET · TP6 GND · TP7 CVBS после терминации (сюда должно приходить ≈0,7 В p-p при 1 В на входе).

## Подключение к Raspberry Pi 4

1. Стандартный 15-жильный шлейф камеры Pi (тот же, что у Camera Module). На плате разъём такой же, как на модуле камеры:
   контакты шлейфа **к плате**, синяя сторона к защёлке. Пин 1 (GND) помечен треугольником, он у верхнего края.
   На Pi 4: контакты шлейфа в сторону HDMI, синяя сторона к Ethernet/USB.
2. Перед первым включением прозвоните: пин 15 шлейфа (3,3 В) ↔ TP1, пин 1 ↔ TP6 (GND).
3. В `/boot/firmware/config.txt` (на старых образах `/boot/config.txt`):

   ```
   camera_auto_detect=0
   dtoverlay=adv728x-m,adv7280m=1
   ```

   Строка `camera_auto_detect=0` отключает автопоиск камер Pi, который иначе может занять I²C и CSI.
4. Перезагрузка и проверка:

   ```
   dmesg | grep -i adv7180          # ожидается: adv7180 10-0021: chip found @ 0x21 ... (шина i2c-10 на Pi 4)
   v4l2-ctl --list-devices          # unicam -> /dev/video0
   v4l2-ctl -d /dev/video0 --get-detected-standard
   v4l2-ctl -d /dev/video0 --set-standard=PAL     # или NTSC
   ```

   Драйвер `adv7180` работает как V4L2 subdevice за `bcm2835-unicam` (в overlay по умолчанию legacy-режим,
   Media Controller выключен). Формат на выходе — 8-битный YUV 4:2:2 (UYVY), 720×576 (PAL) или 720×480 (NTSC), чересстрочный.
5. Захват:

   ```
   v4l2-ctl -d /dev/video0 --set-fmt-video=width=720,height=576,pixelformat=UYVY --stream-mmap --stream-count=100 --stream-to=out.uyvy
   ffmpeg -f v4l2 -input_format uyvy422 -video_size 720x576 -i /dev/video0 -c:v libx264 -preset veryfast out.mkv
   ```

### Вывод на ПК как «карта захвата»

Плата отдаёт только MIPI CSI-2, напрямую к ПК её не подключить. Варианты через Pi 4:

* **UVC-вебкамера по USB-C.** Pi 4 умеет режим USB-устройства: `dtoverlay=dwc2` в config.txt, модуль `libcomposite`,
  и `uvc-gadget` (github.com/raspberrypi/uvc-gadget или kbingham/uvc-gadget с источником V4L2 `/dev/video0`).
  ПК видит обычную вебкамеру 720×576. Не проверялось на этой плате, но это штатный механизм Pi.
* **Поток по сети**: `ffmpeg -f v4l2 -i /dev/video0 ... -f mpegts udp://ПК:5000` или GStreamer/RTSP; на ПК VLC/OBS/ffplay.

## Заказ и сборка на JLCPCB

1. Плата: загрузить `fab/adv7280m-csi2-gerber.zip`. Параметры: 4 слоя, 1,6 мм, стек **JLC04161H-7628**
   (файл `.gbrjob` содержит стек), «Impedance control: yes» (пары 100 Ω дифф, 0,18/0,15 мм), остальное по умолчанию.
2. Сборка: Economic (SMT), верхняя сторона. Загрузить `fab/BOM_JLCPCB.csv` и `fab/CPL_JLCPCB.csv`.
   **В предпросмотре обязательно сверить ориентацию** полярных деталей: U2/U3 (SOT-23-5, пин 1 у метки-треугольника),
   D1 (ESD, двунаправленный — ориентация не важна), J1 (FFC: контакты к краю платы, пин 1 сверху), Y1 (кварц — не важна).
   JLCPCB иногда поворачивает корпуса относительно данных KiCad; в предпросмотре это видно по картинке детали.
3. Не собирает JLCPCB (припаять самостоятельно): **U1 ADV7280WBCPZ-M** (нет на складе; фен + трафарет, паста на термопад ≈50 %),
   **J2** RCA-гнездо (любое угловое 3-выводное с штырём 1,3×2,5 и ушками 2,5×1 на 10 мм, например Multicomp PSG01546),
   перемычки/тестовые точки — это просто площадки.
4. Трафарет: заказать вместе с платой («Stencil», верхняя сторона) — он нужен для чипа; паста на термопад уже разбита на 4 окна.

Extended-позиции (по 3 $ за наименование): LDO, кварц, 27 пФ, ESD-диод, FFC-разъём. Остальное — Basic.

## Известные особенности

* В каждой дифпаре MIPI линия N делает один переход на нижний слой и обратно (два via 0,45/0,2 мм): порядок P/N на
  разъёме Pi обратен порядку выводов чипа, без перекрёстка это не разводится (см. `docs/assumptions.md`, A3-2).
* 22 Ω вместо 24 Ω в терминации — 24 Ω нет в базовой библиотеке JLCPCB; на видео это не влияет.
* Правая нижняя часть платы оставлена свободной под дополнительный каскад (буфер THS7314 для раздачи CVBS на монитор).
