// Проба нагрузки Signalsmith Stretch (этап 8, часть 3), отдельно от плагина.
// N стерео-дорожек по очереди в одном потоке, как в run() плагина, блоками по B кадров при 48 кГц.
// Для каждого случая: средняя загрузка ядра (время обработки / длительность звука) и худший блок
// (p99 и максимум, в % от длительности блока: больше 100% — xrun).
//
//   make probe-stretch            (собрать и запустить, B=256)
//   ./build_test/stretch_probe 128
#include "signalsmith-stretch.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

constexpr int kRate = 48000;
constexpr double kAudioSec = 8.0;   // звука на один случай
constexpr double kWarmSec = 1.0;    // первые блоки не считаем

using Stretch = signalsmith::stretch::SignalsmithStretch<float>;

// Тестовый луп 4 с: аккорд из 4 нот с обертонами и удары шума каждые 0,5 с.
// Тишины нет: на тишине Stretch почти ничего не считает, и замер был бы занижен.
struct Loop {
	std::vector<float> l, r;
	Loop() {
		int n = 4 * kRate;
		l.resize(n);
		r.resize(n);
		const double f[4] = {110.0, 138.6, 164.8, 220.0};
		unsigned seed = 12345;
		for (int i = 0; i < n; ++i) {
			double t = double(i) / kRate, a = 0, b = 0;
			for (int k = 0; k < 4; ++k)
				for (int h = 1; h <= 5; ++h) {
					double ph = 2 * M_PI * f[k] * h * t;
					a += std::sin(ph) / (h * 8.0);
					b += std::sin(ph + 0.3 * k) / (h * 8.0);
				}
			double hit = std::fmod(t, 0.5);
			double env = std::exp(-hit * 30.0);
			seed = seed * 1664525u + 1013904223u;
			double nz = (double(seed >> 8) / double(1 << 24) * 2 - 1) * env * 0.5;
			l[i] = float(a + nz);
			r[i] = float(b + nz);
		}
	}
};

struct Track {
	Stretch st;
	int pos = 0;
	std::vector<float> in_l, in_r, out_l, out_r;
};

struct Mode {
	const char* name;
	float semis;  // сдвиг высоты
	float rate;   // сколько входа на кадр выхода (1 — без растяжения)
};

struct Preset {
	const char* name;
	int kind;  // 0 default, 1 default+split, 2 cheaper (split)
};

struct Result {
	double avg, p99, max;
};

Result run_case(const Loop& loop, const Preset& p, const Mode& m, int ntr, int block) {
	std::vector<Track> tr(ntr);
	int max_in = int(std::ceil(block * m.rate)) + 2;
	for (int t = 0; t < ntr; ++t) {
		Track& k = tr[t];
		if (p.kind == 0) k.st.presetDefault(2, kRate, false);
		else if (p.kind == 1) k.st.presetDefault(2, kRate, true);
		else k.st.presetCheaper(2, kRate, true);
		k.st.setTransposeSemitones(m.semis);
		k.pos = (t * 7919) % int(loop.l.size());
		k.in_l.resize(max_in);
		k.in_r.resize(max_in);
		k.out_l.resize(block);
		k.out_r.resize(block);
	}
	int blocks = int(kAudioSec * kRate / block);
	int warm = int(kWarmSec * kRate / block);
	std::vector<double> times;
	times.reserve(blocks);
	double acc = 0;  // дробная часть входа
	int n = int(loop.l.size());
	for (int b = 0; b < blocks; ++b) {
		acc += block * m.rate;
		int in_n = int(acc);
		acc -= in_n;
		auto t0 = std::chrono::steady_clock::now();
		for (Track& k : tr) {
			for (int i = 0; i < in_n; ++i) {
				k.in_l[i] = loop.l[k.pos];
				k.in_r[i] = loop.r[k.pos];
				if (++k.pos >= n) k.pos = 0;
			}
			float* ins[2] = {k.in_l.data(), k.in_r.data()};
			float* outs[2] = {k.out_l.data(), k.out_r.data()};
			k.st.process(ins, in_n, outs, block);
		}
		auto t1 = std::chrono::steady_clock::now();
		if (b >= warm) times.push_back(std::chrono::duration<double>(t1 - t0).count());
	}
	double blk_sec = double(block) / kRate, sum = 0;
	for (double x : times) sum += x;
	std::vector<double> s = times;
	std::sort(s.begin(), s.end());
	Result r;
	r.avg = 100.0 * sum / (blk_sec * double(times.size()));
	r.p99 = 100.0 * s[size_t(double(s.size() - 1) * 0.99)] / blk_sec;
	r.max = 100.0 * s.back() / blk_sec;
	// не даём компилятору выкинуть обработку
	volatile float sink = tr[0].out_l[block / 2];
	(void)sink;
	return r;
}

}  // namespace

int main(int argc, char** argv) {
	int block = argc > 1 ? std::atoi(argv[1]) : 256;
	if (block < 16 || block > 4096) block = 256;
	Loop loop;
	const Preset presets[] = {
		{"default", 0},
		{"default+split", 1},
		{"cheaper", 2},
	};
	const Mode modes[] = {
		{"pitch +7", 7.0f, 1.0f},
		{"stretch 0.8x", 0.0f, 0.8f},
		{"stretch 1.25x", 0.0f, 1.25f},
		{"pitch+stretch", 7.0f, 1.25f},
	};
	const int counts[] = {1, 2, 4, 6};

	std::printf("Eka Tape: проба Signalsmith Stretch, 48 kHz, блок %d кадров (%.2f мс)\n", block,
	            1000.0 * block / kRate);
	std::printf("Нагрузка в %% одного ядра: avg — в среднем; p99 и max — худшие блоки (>100%% = xrun)\n\n");
	for (const Preset& p : presets) {
		Stretch probe;
		if (p.kind == 0) probe.presetDefault(2, kRate, false);
		else if (p.kind == 1) probe.presetDefault(2, kRate, true);
		else probe.presetCheaper(2, kRate, true);
		double lat = 1000.0 * (probe.inputLatency() + probe.outputLatency()) / kRate;
		std::printf("== %s (задержка %.0f мс)\n", p.name, lat);
		std::printf("%-14s", "");
		for (int c : counts) std::printf(" | дорожек: %-7d", c);
		std::printf("\n%-14s", "");
		for (size_t i = 0; i < sizeof(counts) / sizeof(counts[0]); ++i) std::printf(" | %5s %5s %5s", "avg", "p99", "max");
		std::printf("\n");
		for (const Mode& m : modes) {
			std::printf("%-14s", m.name);
			for (int c : counts) {
				Result r = run_case(loop, p, m, c, block);
				std::printf(" | %5.1f %5.0f %5.0f", r.avg, r.p99, r.max);
				std::fflush(stdout);
			}
			std::printf("\n");
		}
		std::printf("\n");
	}
	return 0;
}
