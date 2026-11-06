/* /bin/child: helper for the process tests (spawntest).
 *   child exit N     exit with code N
 *   child ppid P     exit 0 if getppid() == P, else 1
 *   child sleep MS   sleep, then exit 7
 *   child orphan MS  spawn "child sleep MS" and exit at once (orphaning it) */
#include "tern.h"

int main(int argc, char **argv) {
    if (argc < 3) return 100;
    int n = atoi(argv[2]);
    if (!strcmp(argv[1], "exit")) return n;
    if (!strcmp(argv[1], "ppid")) return getppid() == n ? 0 : 1;
    if (!strcmp(argv[1], "sleep")) { sleep_ms((uint64_t)n); return 7; }
    if (!strcmp(argv[1], "orphan")) {
        const char *av[] = { "child", "sleep", argv[2], NULL };
        return spawn("/bin/child", av) > 0 ? 0 : 2;
    }
    return 101;
}
