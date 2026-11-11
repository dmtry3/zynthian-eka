# Eka Probe: можно ли хранить звук сессии Eka Tape в пресетах Zynthian

Временный эксперимент, не плагин набора. После проверки папку удаляем.

Eka Probe пропускает звук насквозь. При сохранении пресета или снапшота он пишет тестовый WAV
(размер задаёт крутилка **Test MB**, по умолчанию 100 МБ), при загрузке читает его обратно и сверяет.
Всё, что произошло, он пишет в `/tmp/eka-probe.log` (лог стирается при перезагрузке Pi).

## Что выясняем
1. Даёт ли jalv в Zynthian плагину место для файлов (`makePath`) и куда кладёт файл: рядом с пресетом?
2. Сколько длится сохранение 100 МБ и 1000 МБ и не зависает ли интерфейс Zynthian.
3. Идёт ли звук во время сохранения и во время загрузки (строка `longest audio gap`).
4. Сохраняет ли снапшот файл плагина или только значения.

## Установка (на Pi)
```bash
cd /root/zynthian-eka && git pull
cd experiments/state-probe && sudo make install
sudo systemctl restart zynthian
```
Если Eka Probe нет в списке аудиоэффектов: в веб-интерфейсе Zynthian (http://zynthian.local)
откройте раздел с LV2-плагинами, найдите «Eka Probe», включите и сохраните.

## Проверка
Перед началом очистите лог: `: > /tmp/eka-probe.log`

**А. Пресет, 100 МБ**
1. Добавьте Eka Probe аудиоэффектом в цепочку, где идёт звук (например, после синтезатора или на входе).
2. Играйте или пустите звук, чтобы слышать, прерывается ли он.
3. Сохраните пресет Eka Probe (меню пресетов → Save / New preset), имя `probe100`.
   Засеките, сколько секунд Zynthian «думал». Звук прерывался?
4. Загрузите другой пресет (или Default), потом снова `probe100`. Звук прерывался? Надолго?

**Б. Пресет, 1000 МБ**
1. Поставьте крутилку Test MB на 1000.
2. Сохраните пресет `probe1000`. Сколько секунд? Интерфейс не завис, не было ошибки?
3. Загрузите его, как в А4.

**В. Снапшот**
1. Test MB на 100. Сохраните снапшот (Save snapshot), имя `probe-snap`.
2. Перезапустите Zynthian: `sudo systemctl restart zynthian`
3. Загрузите снапшот `probe-snap`.

## Что прислать мне
```bash
cat /tmp/eka-probe.log
find /zynthian/zynthian-my-data -name 'eka-probe*.wav' -exec ls -la {} \;
ls -la /zynthian/zynthian-my-data/presets/lv2/ | grep -i probe
ls -lat /zynthian/zynthian-my-data/snapshots/ | head -5
```
И коротко словами: сколько длилось сохранение 100 и 1000 МБ, прерывался ли звук, не было ли ошибок
или зависаний.

## Удаление после проверки
```bash
cd /root/zynthian-eka/experiments/state-probe && sudo make uninstall
rm -rf /zynthian/zynthian-my-data/presets/lv2/Eka_Probe.presets.lv2
sudo systemctl restart zynthian
```
Тестовые WAV лежат там, где их покажет команда `find` выше, их тоже можно удалить.
