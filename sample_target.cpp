#include <iostream>
#include <thread>
#include <chrono>

volatile int watched = 0;

int main() {
    std::cout << "sample_target: start\n";
    // Increased iterations so baseline timing is measurable and both direct and
    // traced runs operate on the same workload.
    const int ITER = 2000000; // 2 million
    for (int i = 0; i < ITER; ++i) {
        watched = i;
        int r = watched;
        (void)r;
        if ((i & 0x3FFF) == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    std::cout << "sample_target: done. final watched = " << watched << "\n";
    return 0;
}