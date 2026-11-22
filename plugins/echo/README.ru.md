# Eka Echo для Zynthian

[English](README.md) · **Русский**

**Стерео и полиритмический концертный дилей** для [Zynthian](https://zynthian.org): аудио-эффект в формате **LV2** (стерео вход, стерео выход, синхронизация с BPM Zynthian через `time:Position`) с **интерактивным тач-виджетом** (перетаскиваемые L/R маркеры на временной шкале, полоска фильтра петли HPF–LPF, метры IN/DUCK/OUT и кнопки Freeze/Trails/Bypass).

Сделан под четыре энкодера: шесть логичных страниц управления — время, режим/роутинг, тон петли, модуляция/дакинг, концертные функции и отдельный экран живых эффектов.

> Проверено на: Zynthian **Vangelis** (stable 2610), Raspberry Pi 5, набор V5, тачскрин 800×480.
> Другие версии и железо не проверялись.

## Возможности

- **Независимое время L и R (`5..2500 мс`) + BPM Sync:** плавный «ленточный» репитч с кубической интерполяцией Эрмита; в режиме `Sync` привязывается к темпу Zynthian (`1/64 .. 1 bar`, включая триоли и точки).
- **4 характера задержки (`Mode`):**
  1. `Clean` — чистый современный цифровой дилей.
  2. `Tape` — тёплая асимметричная сатурация, DC-блокер, живой wow/flutter и инерция ленточного мотора для музыкального репитча при кручении времени.
  3. `BBD` — тёмный плотный аналоговый хвост в духе BBD-педалей.
  4. `Diff` — 4-каскадный стерео all-pass диффузор в петле для размытых эмбиент-облаков.
- **3 стерео-роутинга:** `Stereo`, `PingPong` и `Cross` + регулировка ширины `Width` (`0..100%`).
- **Фильтрация петли и движение:** фильтры Баттерворта `HPF` (`20..1000 Гц`) и `LPF` (`500..20000 Гц`), `Drive`, `Diffuse`, модуляция `Mod Rate`/`Mod Depth`, автоматический дакер `Duck` (`0..100%`, до `-24 дБ`) с настройкой `Duck Rel`.
- **Для живой игры:** бесконечный `Freeze`, даб-хвосты `Trails` (`Mute In`), выходной уровень `Output` и плавный `Bypass`.
- **10 заводских пресетов** (банк *Factory*): `Init Quarter`, `Dotted Eighth Edge`, `Warm Space Tape`, `Analog BBD Slap & Tail`, `Golden Ratio Ping-Pong`, `Ducking Lead Echo`, `Cloudy Diffused Wash`, `Dub Siren Throw`, `Polyrhythm 3 vs 4`, `Infinite Freeze Pad`.
- **Безопасен для реального времени:** без выделений памяти в `run()`, защита от денормалов и NaN, плавное сглаживание параметров без щелчков.

## Установка

На Zynthian (SSH под root):

```bash
git clone https://github.com/dmtry3/zynthian-eka.git
cd zynthian-eka/plugins/echo
sudo make install
systemctl restart zynthian
```

Обновление одной строкой:

```bash
cd /root/zynthian-eka && git pull && sudo make install-echo && systemctl restart zynthian
```

| Ссылка или папка | Что внутри |
|---|---|
| `/usr/local/lib/lv2/eka-echo.lv2` | LV2-бандл (`eka_echo.so`, `manifest.ttl`, `eka_echo.ttl`, `presets.ttl`) |
| `zyngui/widgets_user/zynthian_widget_eka_echo.py` | Тач-виджет (`ui/`) |
| `zynthian-my-data/presets/lv2/Eka_Echo.presets.lv2` | Папка пользовательских пресетов |

`install.sh` обновляет кэш пресетов и прописывает категорию **Delay**. Удаление: `sudo make uninstall`.

При первом добавлении включите **Eka Echo** в webconf Zynthian (*Software → LV2 Plugins*), затем добавьте его в цепочку как *Audio Effect*.

## Управление

Шесть страниц по 4 энкодера:

| Страница | Ручка 1 | Ручка 2 | Ручка 3 | Ручка 4 |
|---|---|---|---|---|
| **1. Time** | `Time L` (`5..2500 мс`) | `Time R` (`5..2500 мс`) | `Feedback` (`0..110%`) | `Mix` (`0..100%`) |
| **2. Effects** | `Time FX` (`-100..+100%`, 0 = исходное) | `Time Link` (`Off / On`) | `Filter FX` (`-100..+100%`) | `Wash FX` (`0..100%`) |
| **3. Mode & Sync** | `Mode` (`Clean/Tape/BBD/Diff`) | `Routing` (`Stereo/PingPong/Cross`) | `Sync` (`Free / Sync`) | `Width` (`0..100%`) |
| **4. Tone** | `HPF` (`20..1000 Гц`) | `LPF` (`500..20000 Гц`) | `Drive` (`0..100%`) | `Diffuse` (`0..100%`) |
| **5. Motion** | `Mod Rate` (`0.05..8 Гц`) | `Mod Depth` (`0..100%`) | `Duck` (`0..100%`) | `Duck Rel` (`50..1000 мс`) |
| **6. Perform** | `Freeze` (`Off / On`) | `Trails` (`Send On / Mute In`) | `Output` (`-18..+12 дБ`) | `Bypass` (`Off / On`) |

## Виджет и жесты

- **Шкала повторов (сверху):** тяните маркер `L` (верхняя половина) или `R` (нижняя) влево/вправо для времени (при включённом `LINK` оба канала двигаются пропорционально; при включённом `Sync` притягивается к нотным длительностям) и вверх/вниз для `Feedback`; переключает энкодеры на `Time`.
- **Полоса фильтра петли (в центре):** тяните левый/правый край для `HPF` / `LPF`; переключает энкодеры на `Tone`.
- **Отдельный экран `FX`:** тап по кнопке `FX` открывает игровой тач-экран **Effects** (и переключает энкодеры на `Effects`): большой **X-Y пэд** (`X = TIME FX`, `-100..0..+100%`; `Y = FILTER FX`, вниз LPF / вверх HPF), снизу слайдер `WASH FX` с кнопкой `0`, переключатель `HOLD / SNAP 0` (пружинный возврат в ноль при отпускании пальца), `LINK / L ONLY` и `ZERO 0` (сброс в ноль без потери исходных `Time L`/`Time R`).
- **6 нижних кнопок:** `MODE`, `ROUTE`, `SYNC`, `FX`, `FREEZE`, `TRAILS` (Mute In); **долгий тап по `TRAILS`** включает общий `Bypass` — весь виджет становится серым.

## Сборка и тесты на Linux / WSL

```bash
make -C plugins/echo                 # собрать LV2-бандл
make -C plugins/echo test            # тесты DSP, LV2-хоста и виджета
SAN=1 make -C plugins/echo test      # с AddressSanitizer + UBSan
```

## Лицензия

GPL-3.0-or-later. См. [LICENSE](LICENSE).
