#include "neodroid_guest.h"
#include <cstdio>
#include <fstream>
#include <vector>
int main(int argc, char **argv) {
    if (argc < 2) { std::printf("用法: %s <guest.elf>\n", argv[0]); return 2; }
    neodroid::set_sandbox_paths("/tmp", "/tmp");
    std::ifstream in(argv[1], std::ios::binary | std::ios::ate);
    std::streamsize n = in.tellg(); in.seekg(0);
    std::vector<std::byte> img((size_t)n);
    in.read(reinterpret_cast<char *>(img.data()), n);
    auto r = neodroid::run_guest_elf(img);
    if (!r.ok) { std::printf("[FAIL] %s\n", r.error.c_str()); return 1; }
    std::printf("[OK] exit=%d\n%s", r.exit_code, r.output.c_str());
    return r.exit_code;
}
