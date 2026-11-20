# Eka Pulse

[English](README.md) · **Русский**

Ритмический мультиэффект-шейпер в формате LV2 с сенсорным виджетом для [Zynthian](https://zynthian.org) (Raspberry Pi 5, комплект V5, тачскрин 800×480), восьмой плагин набора `zynthian-eka`.

## Возможности

- **4 независимых полиритмических слота** подряд, у каждого своя длина цикла `1..16` шагов, форма каждого шага (`_`, `\`, `/`, `/\`, `2x`, `3x`), `Warp`, `Smooth / Punch` и 24 встроенных паттерна:
  1. **1: TIME** — стерео кольцевой буфер с 4-точечной интерполяцией Эрмита и кроссфейдом на скачках: `Curve` (TimeShaper), `Chop` (`THRU`, `1/8`, `1/16`, `1/2x`, `REV`, `STOP`), `Stutter`, `TapeStop` (время торможения регулируется через `Range`).
  2. **2: DRIVE & CRUSH** — `Tube`, `Fold` (вейвфолдер с ADAA), `Hard`, `Crush` (Sample Rate + Bit Crush), `Combo`.
  3. **3: FILTER** — стабильный TPT SVF (`LP 24`, `LP 12`, `BP`, `HP`, `Morph`, `Notch`) и стерео 4-ступенчатый allpass-`Phaser`.
  4. **4: GATE & SPACE** — `Gate` (с `Floor` и `Punch`), `Pan`, `Width` (M/S), `Gate+Pan`.
- **Кроссовер `Low Split` (Linkwitz-Riley 24 дБ/окт, `Off / 60..500 Hz`)** — оставляет бас чистым или давит его огибающей гейта через `Low Duck`.
- **Триггеры и огибающая** — `Sync` (стоит при остановленном транспорте, стартует синхронно с 1-й доли), `Free`, `Audio` (перезапуск по атаке входа), `MIDI` (перезапуск по Note On) и `Env → Depth` (`-100..+100%`).
- **Живая игра (вкладка `Perform`)** — сетка 3×3 моментальных пэдов (`TapeStop`, `1/16 Rpt`, `1/8 Rpt`, `Reverse`, `Half 1/2`, `GateChop`, `FiltDrop`, `BitCrush`, `Bypass`), **XY-пэд** (`X: Cutoff`, `Y: Depth`) и **4 сцены `A–D`** с `Store`, переключением педалью (`Prev Scene` / `Next Scene`) и сохранением в LV2 State.
- **24 заводских пресета** + встроенная вкладка **Help** и динамические подписи энкодеров под каждый слот.
- **Стерео-линкованный brickwall-лимитер** с нулевой задержкой (`LIM` на виджете), 0 аллокаций в аудиопотоке, flush-to-zero.

## Страницы энкодеров (5 × 4)

| Страница | Ручка 1 | Ручка 2 | Ручка 3 | Ручка 4 |
|---|---|---|---|---|
| **1. Main** | Slot (`1:Time..4:Gate`) | Mix % | Low Split (`Off, 60..500 Hz`) | Low Duck (`Off / On`) |
| **2. FX** *(слот)* | Mode *(имя слота)* | Param 1 *(Range / Drive / Cutoff / Floor)* | Param 2 *(Window / Tone-Crush / Res / Width)* | Depth (`-100..+100%`) |
| **3. Curve** *(слот)* | Pattern (`Custom` + 24) | Step Rate (`1/32..1 bar`, `Free`) | Steps (`1..16`) | Warp (`-100..+100%`) |
| **4. Trigger** *(слот)* | Trig (`Sync/Free/Audio/MIDI`) | Threshold dB | Env→Depth (`-100..+100%`) | Smooth / Punch |
| **5. Perform** | Live Pad (`Off` + 8 FX) | Prev Scene *(педаль)* | Next Scene *(педаль)* | Scene (`A–D`) |

## Сборка и установка

На Zynthian (SSH под root):

```bash
cd /root/zynthian-eka
sudo make install-pulse
systemctl restart zynthian
```

В WSL / Linux:

```bash
make -C plugins/pulse
make -C plugins/pulse test
SAN=1 make -C plugins/pulse test
```

## Лицензия

GPL-3.0-or-later. См. [LICENSE](LICENSE).
