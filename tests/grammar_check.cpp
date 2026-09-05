// grammar_check — hashes Pattern::Generate output over a grid of (nSteps, v,
// seeds). Build once against the PRISTINE core (no -DHAS_MODE, 6-arg Generate)
// and once against this branch (-DHAS_MODE): mode-0 hashes must be identical
// = Classic is byte-identical. Also validates tiling coverage (cells exactly
// cover [0,nSteps)) and source ranges, for both layers in Collins mode.
//
//   g++ -O2 -std=c++17 -I../../Audiothurgist/core tests/grammar_check.cpp -o /tmp/gc_pristine
//   g++ -O2 -std=c++17 -DHAS_MODE -Icore     tests/grammar_check.cpp -o /tmp/gc_branch
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include "bb_pattern.h"

static uint64_t h64(uint64_t h, uint64_t x)
{
    return h ^ (x + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2));
}

int main(int argc, char** argv)
{
    int mode = argc > 1 ? atoi(argv[1]) : 0;
    static bb::Pattern p;
    uint8_t cls[64];
    for(int i = 0; i < 64; i++) cls[i] = (i % 4 == 0) ? 0 : (i % 4 == 2) ? 1 : 2;
    uint64_t H = 0;
    int badsrc = 0, coverErr = 0, cells = 0, odd = 0, rests = 0, rolls = 0;
    const int nsList[3] = {16, 32, 64};
    for(int ni = 0; ni < 3; ni++)
        for(int vi = 0; vi <= 20; vi++)
            for(int seed = 1; seed <= 25; seed++)
            {
                int   ns = nsList[ni];
                float v  = vi / 20.f;
#ifdef HAS_MODE
                p.Generate(ns, 4, cls, v, (uint64_t)seed * 7919ULL,
                           (uint64_t)seed * 104729ULL + 3, mode);
#else
                p.Generate(ns, 4, cls, v, (uint64_t)seed * 7919ULL,
                           (uint64_t)seed * 104729ULL + 3);
#endif
                int c = 0;
                while(c < ns)
                {
                    int L = p.len[c] > 0 ? p.len[c] : 1;
                    if(c + L > ns) { coverErr++; break; }
                    if(p.src[c] >= ns) badsrc++;
                    if(p.src[c] == -2) rests++;
                    if(p.src[c] >= 0 && L == 1) { /* fine */ }
                    if(L == 3 || L == 5 || L == 6) odd++;
                    cells++;
                    c += L;
                }
                if(c != ns) coverErr++;
                for(int i = 0; i < ns; i++)
                {
                    H = h64(H, (uint8_t)p.src[i]);
                    H = h64(H, (uint8_t)p.len[i]);
                    H = h64(H, p.rev[i]);
                    H = h64(H, p.rat[i]);
                }
#ifdef HAS_MODE
                for(int i = 0; i < ns; i++) if(p.ratN[i] > 1) rolls++;
                if(mode == 1 && p.fillStart >= 0)
                {
                    int c2 = p.fillStart;
                    while(c2 < ns)
                    {
                        int L = p.flen[c2] > 0 ? p.flen[c2] : 1;
                        if(c2 + L > ns) { coverErr++; break; }
                        if(p.fsrc[c2] >= ns) badsrc++;
                        c2 += L;
                    }
                    if(c2 != ns) coverErr++;
                }
#endif
            }
    printf("mode %d  hash %016llx  cells %d  odd %d  rests %d  geoRolls %d  badsrc %d  coverErr %d\n",
           mode, (unsigned long long)H, cells, odd, rests, rolls, badsrc, coverErr);
    return (badsrc || coverErr) ? 1 : 0;
}
