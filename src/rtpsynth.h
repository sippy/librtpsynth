void *rsynth_ctor(int srate, int ptime);
void *rsynth_next_pkt(void *ri, int plen, int pt);
int rsynth_next_pkt_pa(void *ri, int plen, int pt, void *buf, unsigned int blen, int pa);
void rsynth_pkt_free(void *rnp);
void rsynth_dtor(void *ri);
