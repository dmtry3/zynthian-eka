/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/dsp/tracker.h"
#include "../src/harmony/scale.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
static unsigned checking, allocations;
void *__real_malloc(size_t); void *__real_calloc(size_t,size_t); void *__real_realloc(void *,size_t);
void *__wrap_malloc(size_t n) { if(checking) ++allocations; return __real_malloc(n); }
void *__wrap_calloc(size_t n,size_t s) { if(checking) ++allocations; return __real_calloc(n,s); }
void *__wrap_realloc(void *p,size_t n) { if(checking) ++allocations; return __real_realloc(p,n); }
int main(void) {
    unsigned rates[] = {8000,44100,48000,96000,192000};
    double frequencies[] = {82.406889,110,220,440,880,2093.0045};
    double max_error=0;
    for(unsigned r=0;r<5;++r) {
        EkaTracker *p=eka_tracker_create(rates[r]); if(!p)return 1;
        for(unsigned f=0;f<6;++f) {
            if(frequencies[f]>rates[r]/8.0)continue;
            EkaScale scale;
            checking=1; eka_tracker_reset(p); eka_scale_reset(&scale);
            float input[128];
            for(unsigned offset=0;offset<rates[r];offset+=128) {
                for(unsigned i=0;i<128;++i) input[i]=(float)(.2*sin(6.283185307179586*frequencies[f]*(offset+i)/rates[r]));
                eka_tracker_process(p,input,128,0);
            }
            float note,confidence,hz;
            int valid=eka_tracker_read(p,0,&note,&confidence,&hz);
            float harmony[4]; int degrees[] = {-7,2,4,7};
            int harmony_valid = eka_scale_targets(&scale, f%12, f%EKA_SCALE_COUNT, note, degrees, harmony);
            checking=0;
            if (!harmony_valid) return 1;
            double error=fabs(1200*log2(hz/frequencies[f]));
            if(!valid || error>(rates[r]==8000 ? 12 : 8) || allocations) { fprintf(stderr,"rate=%u freq=%g error=%g heap=%u\n",rates[r],frequencies[f],error,allocations);return 1; }
            if(error>max_error)max_error=error;
        }
        eka_tracker_destroy(p);
    }
    printf("tracker/Scale: five rates, up to six notes, reset/process/read/targets heap=%u max_cents=%.3f\n",allocations,max_error);
    return 0;
}
