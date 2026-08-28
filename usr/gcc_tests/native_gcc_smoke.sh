#!/bin/sh
set -eu

work=/tmp

echo "[gcc-smoke] compiler identities"
gcc --version
g++ --version

cat > "$work/hello.c" <<'EOF'
#include <stdio.h>

static int square(int value) {
    return value * value;
}

int main(void) {
    printf("native C: square(9)=%d\n", square(9));
    return square(9) == 81 ? 0 : 1;
}
EOF

echo "[gcc-smoke] C compile, link, and execute"
gcc -O2 -Wall -Wextra "$work/hello.c" -o "$work/hello-c"
"$work/hello-c"

cat > "$work/hello.cpp" <<'EOF'
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

int main() {
    std::vector<int> values{4, 1, 3, 2};
    std::sort(values.begin(), values.end());
    int worker_value = 0;
    std::thread worker([&] { worker_value = values.back() * 10; });
    worker.join();
    try {
        throw std::runtime_error("exceptions work");
    } catch (const std::exception &error) {
        std::cout << "native C++: " << error.what()
                  << ", worker=" << worker_value << '\n';
    }
    return worker_value == 40 ? 0 : 1;
}
EOF

echo "[gcc-smoke] C++ standard library, exception, and pthread support"
g++ -O2 -Wall -Wextra -pthread "$work/hello.cpp" -o "$work/hello-cxx"
"$work/hello-cxx"

echo "[gcc-smoke] native LTO plugin"
gcc -O2 -flto "$work/hello.c" -o "$work/hello-lto"
"$work/hello-lto"

echo "[gcc-smoke] PASS"
