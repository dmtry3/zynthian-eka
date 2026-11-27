# Eka Prism для Zynthian

[English](README.md) · **Русский**

**Двухцепочечный мультимодельный морфинг-фильтр с модуляцией** для [Zynthian](https://zynthian.org): стерео аудио-эффект в формате **LV2** с синхронизацией темпа (`time:Position`), отслеживанием MIDI-нот (`midi:MidiEvent`) и **интерактивным тач-виджетом** (живой график АЧХ обеих цепочек **Filter A**, **Filter B** и их суммы в реальном времени с отображением модуляции от LFO и огибающей, перетаскиваемые узлы `Cutoff ×Resonance`, полосы `Morph A / Morph B`, индикаторы `IN / ENV / OUT` и отдельный игровой экран **XY / FX**).

> Проверено на: Zynthian **Vangelis** (stable 2610), Raspberry Pi 5, набор V5, тачскрин 800×480.
> Другие версии и железо не проверялись.

## Возможности

- **Две независимые стереоцепочки (`Filter A` и `Filter B`), в каждой 6 моделей фильтров (`Model A` / `Model B`):**
  1. `SVF Morph` — фильтр состояния (Cytomic ZDF SVF) с бесшовным морфингом (`LP ↔ BP ↔ HP ↔ Notch`) и мягким насыщением в резонансе.
  2. `Ladder 24` — 4-полюсный (`24 дБ/окт`) транзисторный лестничный фильтр с компенсацией баса и плавным морфингом полюсов (`24 дБ LP ↔ 12 дБ LP ↔ 4-pole BP ↔ HP`).
  3. `MS-20 Scream` — агрессивный фильтр Саллена — Ки с асимметричным диодным клиппингом в петле обратной связи и морфингом `LP ↔ BP ↔ HP`.
  4. `Comb Res` — настраиваемый гребенчатый резонатор на дробной задержке Эрмита (`Morph` плавно переводит из отрицательной «кларнетной» гребёнки в положительную «струнную» с переменным демпфированием).
  5. `Formant` — 3-полосный гласный формантный фильтр с плавным морфингом между 5 гласными (`A ↔ E ↔ I ↔ O ↔ U`) и сдвигом высоты голосового тракта ручкой `Cutoff`.
  6. `Phaser 6` — 6-каскадный фазер, где `Morph` управляет разбросом полюсов и полярностью обратной связи.
- **3 режима маршрутизации и связка частот (`Routing`):**
  - `Serial A>B` — последовательное включение Filter A → Filter B (с балансом `Balance`).
  - `Parallel A+B` — параллельное сложение A + B с кнопкой инверсии фазы **`Phase B` (`Norm / Invert`)** для создания глубоких фазовых вырезов.
  - `Stereo L/R` — левый канал через Filter A, правый через Filter B для широкого стереодвижения.
  - `Link B` (`Free / Offset / Mirror`): частота `Cutoff B` либо независима (`Free`), либо следует за `Cutoff A` с заданным интервалом (`Offset`), либо движется зеркально навстречу вокруг `1 кГц` (`Mirror`).
- **Модуляция (`LFO` и `Mod Depth`):**
  - **7 форм LFO:** `Sine`, `Tri`, `Saw`, `Square`, `S&H` (сглаженный рандом), `Chaos` (нелинейный аттрактор/дрейф) и `Step 8` (8-шаговый ритмический грув-секвенсор).
  - Свободная частота `Rate` (`0.05..40 Гц`, вплоть до **аудио-частотной FM**) или синхронизация с темпом Zynthian `Sync` (`4 bars .. 1/64`).
  - **`B Phase` (`0..180°`):** сдвиг фазы LFO для второй цепочки (квадратура `90°` или противофаза `180°`).
  - Биполярные глубины `LFO > Cut`, `Mod > Morph` (модуляция морфинга/гласных/гребёнки) и `Env > Cut` (входной **Envelope Follower** с настройкой скорости `Env Speed`).
  - **MIDI Note Tracking:** подача MIDI-нот смещает частоты обоих фильтров относительно `C4` (`60`) для игры мелодий резонансом или гребёнкой.
- **Драйв и мастер (`Master`):**
  - Аналоговый `Drive` перед фильтрами с компенсацией громкости, параллельный `Mix`, уровень `Output` (`-18..+12 дБ`) и бесщёлчковый `Bypass`.
- **12 заводских пресетов** (банк *Factory*).

## Установка

На Zynthian (SSH под root):

```bash
git clone https://github.com/dmtry3/zynthian-eka.git
cd zynthian-eka/plugins/prism
sudo make install
systemctl restart zynthian
```

Обновление одной строкой:

```bash
cd /root/zynthian-eka && git pull && sudo make install-prism && systemctl restart zynthian
```

## Управление

Семь страниц энкодеров (включая отдельную страницу живой игры **Effects / Play**, сделанную по образцу *Eka Echo*):

| Страница | Ручка 1 | Ручка 2 | Ручка 3 | Ручка 4 |
|---|---|---|---|---|
| **1. Filter A** | `Model A` (`SVF / Ladder / MS-20 / Comb / Formant / Phaser`) | `Cutoff A` (`20..18000 Гц`) | `Res A` (`0..100%`) | `Morph A` (`0..100%`) |
| **2. Filter B** | `Model B` (`SVF / Ladder / MS-20 / Comb / Formant / Phaser`) | `Cutoff B` (`20..18000 Гц`) | `Res B` (`0..100%`) | `Morph B` (`0..100%`) |
| **3. Effects (Play)** | `Sweep FX` (`-100..+100%`, `±3.5 окт` поверх базовых частот) | `FX Link` (`A Only / Linked`) | `Morph FX` (`-100..+100%`) | `Scream FX` (`0..100%`) + `FX Zero` (MIDI-сброс), `FX Smooth` (`0..100%`) |
| **4. Routing** | `Routing` (`Serial A>B / Parallel A+B / Stereo L/R`) | `Link B` (`Free / Offset / Mirror`) | `Balance` (`-100..+100%`) | `Phase B` (`Norm / Invert`) |
| **5. LFO** | `Shape` (`Sine / Tri / Saw / Square / S&H / Chaos / Step 8`) | `Rate` (`0.05..40 Гц` / Sync) | `Sync` (`Free / Sync`) | `B Phase` (`0..180°`) |
| **6. Mod Depth** | `LFO > Cut` (`-100..+100%`) | `Mod > Morph` (`-100..+100%`) | `Env > Cut` (`-100..+100%`) | `Env Speed` (`0..100%`) |
| **7. Master** | `Drive` (`0..100%`) | `Mix` (`0..100%`) | `Output` (`-18..+12 дБ`) | `Bypass` (`Off / On`) |

## Сборка и тесты на Linux / WSL

```bash
make -C plugins/prism                 # собрать LV2-бандл
make -C plugins/prism test            # тесты DSP, LV2-хоста и виджета
SAN=1 make -C plugins/prism test      # с AddressSanitizer + UBSan
```

## Лицензия

GPL-3.0-or-later.
