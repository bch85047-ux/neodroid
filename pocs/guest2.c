// NeoDroid P2 guest: 真 libc 程序，验证 malloc/文件/stat。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

int main(int argc, char **argv) {
    printf("[guest] P2 start, argc=%d\n", argc);

    char *heap = (char *)malloc(1 << 20);
    if (!heap) { printf("[guest] malloc failed\n"); return 1; }
    memset(heap, 0x5A, 1 << 20);
    unsigned long sum = 0;
    for (int i = 0; i < (1 << 20); i += 4096) sum += (unsigned char)heap[i];
    printf("[guest] malloc 1MiB ok, checksum=%lu\n", sum);
    free(heap);

    const char *path = "/tmp/neodroid_p2.txt";
    const char *payload = "NeoDroid P2 file roundtrip\n";
    int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) { printf("[guest] open(w) failed: %d\n", fd); return 2; }
    ssize_t n = write(fd, payload, strlen(payload));
    printf("[guest] wrote %zd bytes\n", n);
    close(fd);

    fd = open(path, O_RDONLY);
    if (fd < 0) { printf("[guest] open(r) failed: %d\n", fd); return 3; }
    char buf[128] = {0};
    n = read(fd, buf, sizeof(buf) - 1);
    printf("[guest] read %zd bytes: %s", n, buf);

    struct stat st;
    if (fstat(fd, &st) == 0) {
        printf("[guest] fstat size=%lld mode=0%o\n", (long long)st.st_size, st.st_mode & 0777);
    } else {
        printf("[guest] fstat failed\n");
    }
    close(fd);

    char cwd[256] = {0};
    if (getcwd(cwd, sizeof(cwd))) printf("[guest] cwd=%s\n", cwd);

    printf("[guest] P2 done\n");
    return 0;
}