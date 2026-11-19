/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "backend.h"
#include "bungee/Stream.h"
#include <cmath>
#include <new>
#ifndef EKA_BUNGEE_GRANULARITY
#define EKA_BUNGEE_GRANULARITY 0
#endif
struct EkaBackend {
    Bungee::Stretcher<Bungee::Basic> stretcher;
    Bungee::Stream<Bungee::Basic> stream;
    double ratio=1;
    unsigned hop;
    EkaBackend(unsigned rate):stretcher({(int)rate,(int)rate},1,EKA_BUNGEE_GRANULARITY),stream(stretcher,128,1) {
        hop=(unsigned)(stretcher.maxInputFrameCount()-1)/32;
        stretcher.enableInstrumentation(false);
        float silence[128]={},out[128];const float *in[]={silence};float *outputs[]={out};
        for(int i=0;i<64;i++)stream.process(in,outputs,128,128,1.5);
        stream.resetLive();
    }
};
extern "C" EkaBackend *eka_backend_create(unsigned rate) {
    try { return new EkaBackend(rate); } catch(const std::bad_alloc&) { return nullptr; }
}
extern "C" void eka_backend_destroy(EkaBackend *p) { delete p; }
extern "C" void eka_backend_reset(EkaBackend *p) { p->stream.resetLive();p->ratio=1; }
extern "C" void eka_backend_pitch(EkaBackend *p,double ratio) { p->ratio=ratio; }
extern "C" unsigned eka_backend_block(const EkaBackend*) { return 128; }
extern "C" unsigned eka_backend_delay(const EkaBackend *p) {
    return (unsigned)std::lround(4*p->hop+3*p->hop/p->ratio);
}
extern "C" unsigned eka_backend_faults(const EkaBackend*) { return 0; }
extern "C" void eka_backend_process(EkaBackend *p,const float *in,float *out) {
    const float *inputs[]={in};float *outputs[]={out};p->stream.process(inputs,outputs,128,128,p->ratio);
}

extern "C" unsigned eka_backend_tracking_age(const EkaBackend *p) { return 4 * p->hop + 128; }
