#pragma once

#define SEC(x)      ((x)->tv_sec)
#define NSEC(x)     ((x)->tv_nsec)
#define NSEC_IN_SEC 1000000000L

#ifdef timespecsub
#undef timespecsub
#endif
#define timespecsub(vvp, uvp)           \
    do {                                \
        SEC(vvp) -= SEC(uvp);           \
        NSEC(vvp) -= NSEC(uvp);         \
        if (NSEC(vvp) < 0) {            \
            SEC(vvp)--;                 \
            NSEC(vvp) += NSEC_IN_SEC;   \
        }                               \
    } while (0)

#define timespec2un64time(s) (((uint64_t)SEC(s) * NSEC_IN_SEC) + \
  (uint64_t)NSEC(s))

