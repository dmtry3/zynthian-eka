/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Probe the actual Splice backend's reset, automation and process allocations. */
#include "../src/dsp/backend.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <initializer_list>
static bool realtime = false;
static unsigned allocations = 0;
// Linux/glibc test harness: count C allocation too (Eigen bypasses C++ new).
extern "C" void *__libc_malloc(std::size_t);
extern "C" void *__libc_calloc(std::size_t,std::size_t);
extern "C" void *__libc_realloc(void*,std::size_t);
extern "C" void *malloc(std::size_t n) noexcept { if(realtime) ++allocations; return __libc_malloc(n); }
extern "C" void *calloc(std::size_t a,std::size_t b) noexcept { if(realtime) ++allocations; return __libc_calloc(a,b); }
extern "C" void *realloc(void *p,std::size_t n) noexcept { if(realtime) ++allocations; return __libc_realloc(p,n); }
void *operator new(std::size_t n) {
    if (void *p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
int main() {
    for (unsigned rate : {48000u}) {
        EkaBackend *p = eka_backend_create(rate);
        if (!p || eka_backend_block(p) != 128) return 1;
        float in[128], out[128];
        realtime = true;
        for (unsigned k = 0; k < 120000; ++k) {
            if (k == 0) eka_backend_reset(p);
            eka_backend_pitch(p, std::pow(2.0, ((k/1500)%2 ? 7.0 + .08*std::sin(k*.003351) : -5.0 + .08*std::sin(k*.003351)) / 12));
            for (unsigned i = 0; i < 128; ++i)
                in[i] = .2f * std::sin(6.283185307179586 * 110 * (k*128+i) / rate);
            eka_backend_process(p, in, out);
            for (float x : out) if (!std::isfinite(x) || std::abs(x) > 2) return 1;
        }
        if (eka_backend_faults(p)) { std::printf("stream faults=%u rate=%u\n",eka_backend_faults(p),rate);return 1; }
        realtime = false;
        eka_backend_destroy(p);
    }
    std::printf("Backend reset/process/automation heap allocations=%u (320 seconds, interval changes and drift)\n", allocations);
    return allocations ? 1 : 0;
}
