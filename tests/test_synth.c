#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#include "rtpsynth.h"

static void inline
taint(char *p) {

    __asm__ volatile ("" : : "m" (*p));
}

int main() {
    double tdur = 1.0;
    uint64_t i = 0;
    clock_t start, end;
    double cpu_time_used;
    
    void *rs = rsynth_ctor(8000, 30);
    start = clock();

    while (1) {
        void *rp = rsynth_next_pkt(rs, 170, 0);
        taint(rp);
        i += 1;
        
        if (i % 100000 == 0) {
            end = clock();
            cpu_time_used = ((double) (end - start)) / CLOCKS_PER_SEC;
            if (cpu_time_used >= tdur) {
                break;
            }
        }

        rsynth_pkt_free(rp);
    }

    rsynth_dtor(rs);

    cpu_time_used = ((double) (end - start)) / CLOCKS_PER_SEC;
    double Mi = (double)(i) / (double)(1000000);
    double Mpps = Mi / cpu_time_used;
    printf("Generated %.2fM packets in %.3f seconds, %.2fM packets per second\n", Mi, cpu_time_used, Mpps);

    return 0;
}
