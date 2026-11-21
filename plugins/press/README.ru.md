# Eka Press

Однополосный живой компрессор (LV2) для [Zynthian](https://zynthian.org) (Raspberry Pi 5, сенсорный экран V5).

- **LV2 URI:** `http://dmtry3.github.io/zynthian-eka/press`
- **4 характера сжатия:** `Clean` (VCA), `Opto` (оптическая программа-зависимая огибающая), `Punch` (быстрый FET с плотной атакой), `Glue` (шинный VCA для склейки микса и лупов после *Eka Tape*)
- **Детектор:** фильтр `SC HPF` (`20..500 Гц`), `Stereo Link` (`0..100%`), `Auto Release`, прослушивание детектора `SC Listen`
- **Окрас и выход:** аналоговый `Drive`, `Makeup` + `Auto Makeup`, параллельная компрессия `Mix` (`0..100%`), мягкий потолок `Soft Clip` (`-0.3 дБ`), `Output`
- **Сенсорный виджет:** интерактивная кривая сжатия (тачем можно менять `Threshold` и `Ratio`), бегущая лента уровня и Gain Reduction, вертикальные пикметры `IN / GR / OUT`, кнопки режимов и тумблеров
- **10 заводских пресетов**

## Сборка и установка

```bash
make                # собрать eka-press.lv2
make test           # прогнать тесты ядра, LV2-хоста и виджета
sudo make install   # установить на Zynthian
```
