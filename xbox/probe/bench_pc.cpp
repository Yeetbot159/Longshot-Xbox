// The probe's CPU benchmark on a PC, to set the console's figures against (xbox/README.md).
#include <cstdio>
#include "bench.h"

int main() {
    auto r = bench::run();
    std::printf("one thread %.0f ms; %u threads at once %.0f ms (each thread the same work)\n", r.one_ms, r.threads, r.all_ms);
    return 0;
}
