/* /bin/sync: write cached FAT data of every disk volume and flush the disk caches. */
#include "tern.h"

int main(void) {
    int n = sync();
    if (n < 0) { printf("sync: error %d\n", n); return 1; }
    printf("sync: %d volume%s flushed\n", n, n == 1 ? "" : "s");
    return 0;
}
