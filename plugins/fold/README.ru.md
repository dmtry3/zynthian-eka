# Eka Fold для Zynthian

[English version](README.md)

Полифонический **West Coast синтезатор** для [Zynthian](https://zynthian.org) в виде **LV2-инструмента** со
**своим виджетом** для сенсорного экрана Zynthian: на каждой вкладке видно, что делают ручки своей страницы.

Пара осцилляторов (Principal + Mod OSC) с FM, AM, Sync и Timbre между ними, вейвфолдер и два шейпера Чебышёва,
lopass gate на модели вактрола, два функциональных генератора, матрица модуляции и waveguide / delay в каждом голосе.
До 8 голосов. Вдохновлён Buchla 259 / 292 / 281 и Music Easel, а также Madrona Aalto.

> Проверено на: Raspberry Pi 5, набор Zynthian V5, сенсорный экран 800x480. Другое железо не проверялось.

## Возможности

- **Principal**, шесть источников: **Sine**, **VA** (два осциллятора переменной формы: треугольник, пила, меандр,
  узкий импульс; второй — до двух октав в сторону), **FM2** (двухоператорная FM с обратной связью), **Wave**
  (4 банка по 8 × 8 волн), **Harm** (аддитивный, 24 гармоники), **PD** (phase distortion). Harmonics, Color и Morph
  меняют звук источника; смена источника — плавный переход.
- **Mod OSC** (треугольник, синус, пила, меандр) в режимах **Free** (0,01 Гц … 10 кГц: и LFO, и звуковой
  осциллятор), **Keytrack** или **Ratio** — в Principal через **FM** (линейная, через ноль), **AM**, жёсткий **Sync**
  и **Timbre**, всё одной ручкой **Mod Index**.
- **Секция Timbre:** треугольный вейвфолдер и шейперы Saw / M, кроссфейд ручкой **Order**; **Timbre** — глубина,
  **Symmetry** — смещение (чётные гармоники), **Drive** — мягкое насыщение. Без алиасинга: голоса считаются на 2×,
  на фолдере ADAA.
- **Lopass gate** по мотивам Buchla 292: вактрол быстро открывается и медленно закрывается, тем медленнее, чем
  темнее; режимы **VCA**, **LP**, **Combo**, Decay, Resonance.
- **Функциональные генераторы** FG1 (ведёт гейт) и FG2 по мотивам Buchla 281: Rise / Fall, **Once**, **Gate** или
  **Cycle** (LFO).
- **Матрица модуляции** 8 × 11: FG1, FG2, Mod OSC, Random (новое значение на каждом периоде Mod OSC), Velocity, Key,
  колесо модуляции, aftertouch (канальный и полифонический) → Pitch, Harmonics, Color, Morph, Mod Index, Order,
  Symmetry, Timbre, LPG, Delay Time, Delay Feedback.
- **Waveguide / delay в каждом голосе:** **Tuned** (струна, настроенная по ноте, −24…+24 полутона: Karplus-Strong,
  строй в пределах цента от C2 до C7) или **Free** (эхо от 1 мс до 2 с); в петле peaking EQ и шейпер. Хвост держит
  голос и после отпускания клавиши.
- **Play:** Poly (1…8 голосов), **Mono** (звучит последняя клавиша, возврат к зажатой), **Legato**; Glide; диапазон
  pitch bend.
- **28 заводских пресетов** в банке *Factory*: Perc, String, Bell, Pad, Drone, Bass, Lead, FX.
- **Сенсорный виджет**, вкладка на каждую страницу: формы волн, кривая фолдера, АЧХ гейта, функциональные
  генераторы, EQ и шейпер петли, голоса, сетка матрицы; живые метки (ноты, фаза Mod OSC, вактрол, точки FG, уровень
  линии delay).

## Установка

На Zynthian (по SSH под root):

```bash
git clone https://github.com/dmtry3/zynthian-eka.git
cd zynthian-eka/plugins/fold
sudo make install              # собирает плагин и подключает его к Zynthian (см. ниже)
systemctl restart zynthian
```

Обновление одной строкой:

```bash
cd /root/zynthian-eka && git pull && cd plugins/fold && sudo make install && systemctl restart zynthian
```

`make install` собирает на самом устройстве (gcc, g++, `lv2-dev` — всё есть в Zynthian) и создаёт:

| Ссылка | Куда |
|---|---|
| `/usr/local/lib/lv2/eka-fold.lv2` | `build/eka-fold.lv2` |
| `/zynthian/zynthian-ui/zyngui/widgets_user/zynthian_widget_eka_fold.py` | `ui/zynthian_widget_eka_fold.py` |

Ещё он создаёт папку для своих пресетов (`zynthian-my-data/presets/lv2/Eka_Fold.presets.lv2`) и обновляет список
пресетов Zynthian. Файлы самого Zynthian не меняются. Удаление: `sudo make uninstall`.

Добавьте **Eka Fold** в цепочку как синтезатор (Instrument).

## Управление

Семь страниц по 4 ручки (4 энкодера). Вкладка виджета следует за страницей, тап по вкладке выбирает её страницу.

| Страница | Ручки | |
|---|---|---|
| **Osc** | Source, Harmonics, Color, Morph | Sine / VA / FM2 / Wave / Harm / PD и три его ручки |
| **Mod** | Mod Freq, Mod Index, Mod Shape, Mod Mode | Mod Freq читается по режиму: Гц, полутоны или отношение (1/8 … 16) |
| **Timbre** | Timbre, Order, Symmetry, Drive | Folder (Order 0) … Saw (50) … M (100) |
| **Gate** | Gate Mode, Decay, Resonance, Level | VCA / LP / Combo; Decay 10 мс … 2 с |
| **Env** | FG1 Rise, FG1 Fall, FG2 Rise, FG2 Fall | 1 мс … 10 с |
| **Delay** | Delay, Time, Feedback, Mix | Off / Tuned / Free; Time: полутоны (Tuned) или 2 с × (Time / 100)³ (Free) |
| **Play** | Play, Voices, Glide, Bend | Poly / Mono / Legato; 1…8 голосов; Glide до 2 с; Bend 0…24 полутона |

Остальные настройки — в виджете (они сохраняются в снапшотах и пресетах):

| Вкладка | На экране |
|---|---|
| **Osc** | Плитки источников, **Pitch** и **Fine** (палец вверх / вниз, двойной тап — сброс), волна, ноты |
| **Mod** | Тап по стрелкам **FM / AM / Sync / Timbre** включает маршруты Mod OSC; результат для синуса |
| **Fold** | Передаточная кривая и что она делает с волной |
| **Gate** | Кнопки VCA / LP / Combo; АЧХ по мере закрытия вактрола; живой вактрол |
| **Env** | Кнопки **Once / Gate / Cycle** для FG1 и FG2; глубина FG2 → Timbre |
| **Delay** | Кнопки Off / Tuned / Free; ползунки **Shaper**, **EQ Freq**, **EQ Gain** (шаг 0,1 дБ); уровень петли |
| **Play** | Кнопки Poly / Mono / Legato; 8 ячеек голосов (тап задаёт Voices); скольжение |
| **Matrix** | Сетка 8 × 11: палец вверх / вниз на ячейке, двойной тап — сброс (синий +, оранжевый −) |

## Советы

- **Струна:** Delay **Tuned**, Mix 100 %, Feedback 90…99 %, FG1 в **Once** с коротким Fall (вкладка Env). Лучше
  всего её возбуждает богатый удар (пила VA, немного Timbre или шумовой всплеск FM2). EQ Gain действует на каждом
  проходе петли: несколько десятых дБ вниз глушат струну; выше 0 дБ петля заводится сама (шейпер её держит).
- **Buchla-«бонк»:** Gate **Combo**, FG1 Once, короткий Fall, Decay 20…40 %.
- **Shaper** окрашивает delay с первого повтора: попробуйте на эхе простого синуса.
- **Матрица** прибавляет к ручкам: FG2 (Cycle) → Pitch понемногу — вибрато; Random → Pitch при Mod OSC в Free
  2…8 Гц — случайные ступени; Velocity → Timbre — сильнее удар, ярче звук.

## Нагрузка

Raspberry Pi 5, `./scripts/bench.sh` (% одного ядра): 4 голоса — 2,6…9,3 % в зависимости от патча, delay +1,4 %;
8 голосов с VA, всей матрицей и фолдером — 18 %. В Zynthian 8 голосов при плотной игре: jalv 11 % одного ядра,
без xruns. Если цепочке нужно больше места, уменьшите **Voices**.

## Сборка и тесты (любой Linux)

```bash
make            # build/eka-fold.lv2
make test       # движок офлайн (строй, алиасинг, щелчки, ...), плагин в маленьком хосте, каждый пресет,
                # виджет с заглушками модулей Zynthian
```

Пресеты генерируются: правьте `scripts/gen_presets.py` и запускайте его (он пишет `lv2/presets.ttl` и
`lv2/manifest.ttl`). `make test` записывает каждый пресет в `build_test/presets/*.wav`.

## Как устроено

- `src/fold_core.c`: движок на чистом C без LV2 (проверяется обычной программой). Голоса на удвоенной частоте с
  halfband-децимацией; матрица и коэффициенты фильтра раз в 32 сэмпла, пустые ячейки пропускаются; в аудиопотоке
  ничего не выделяется.
- Источники VA, FM2, Wave, Harm и PD: небольшая C++-единица в `src/` (без исключений, RTTI и STL) за C-интерфейсом
  (`engines.h`).
- `src/eka_fold.c`: обёртка LV2. Настройки виджета и матрица — LV2-параметры (`patch:writable`), сохраняются в
  LV2 State; живые данные для виджета идут через выходные порты.
- `ui/zynthian_widget_eka_fold.py`: виджет (Tkinter). Кривые он считает формулами самого плагина и перерисовывает
  только то, что изменилось.

## Лицензия

GPL-3.0-or-later. Источники VA, FM2, Wave, Harm и PD и две их таблицы основаны на коде Эмили Жилле под лицензией MIT: текст
лицензии и что изменено — в [THIRD_PARTY.md](THIRD_PARTY.md).
