// Сдвиг высоты без смены темпа (этап 8): стерео-обёртка над Signalsmith Stretch (third_party/signalsmith).
// Вход и выход одной длины: растяжения нет, скорость «как лента» считает сам лупер.
// init() — не RT (выделяет память). Остальное — RT, без выделения памяти.
#pragma once

namespace alp {

class PitchShifter {
 public:
  PitchShifter() = default;
  ~PitchShifter();
  PitchShifter(const PitchShifter&) = delete;
  PitchShifter& operator=(const PitchShifter&) = delete;

  // presetDefault с разнесённым по блокам расчётом (ECO — presetCheaper — убран 2026-11-12: звучит плохо)
  bool init(double sample_rate);
  bool ok() const { return p_ != nullptr; }
  // Задержка вход → выход, кадров: выход сейчас — это вход, поданный latency() кадров назад.
  int latency() const;
  int seek_len() const;   // длина истории для seek() и start()
  int warm_len() const;   // сколько входа start() прогоняет вперёд
  void reset();
  void set_semitones(float s);
  // Запуск с готовым выходом. Звук x: hist — seek_len() кадров до точки X, pre — warm_len() кадров
  // с X (выход этого прогона выбрасывается в tmp). Дальше на вход идёт x с X + warm_len(), а выход
  // соответствует x с X + warm_len() − latency(). (outputSeek() библиотеки при шаге не в 1/4 блока даёт щелчок.)
  void start(const float* const* hist, const float* const* pre, float* const* tmp);
  // Скачок по входу без обрыва выхода: in — история перед новым местом (n == seek_len()).
  void seek(const float* const* in, int n);
  void process(const float* const* in, float* const* out, int n);

 private:
  struct Impl;
  Impl* p_ = nullptr;
};

}  // namespace alp
