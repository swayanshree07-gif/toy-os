/* /bin/mem: physical memory, kernel heap, RAM FS and scheduler statistics. */
#include "tern.h"

int main(void) {
    struct tern_sysinfo si;
    int rc = sysinfo(&si);
    if (rc) { printf("mem: error %d\n", rc); return 1; }
    unsigned long total = si.frames_total * 4, freek = si.frames_free * 4;
    printf("phys: %lu KiB total, %lu KiB used, %lu KiB free\n", total, total - freek, freek);
    printf("heap: %lu KiB mapped, %lu KiB used, %lu KiB free\n",
           (unsigned long)si.heap_mapped / 1024, (unsigned long)si.heap_used / 1024,
           (unsigned long)si.heap_free / 1024);
    printf("ramfs: %lu nodes, %lu bytes of file data\n", (unsigned long)si.vfs_nodes,
           (unsigned long)si.vfs_bytes);
    printf("sched: %lu processes, %lu threads, %lu switches, %lu preemptions\n",
           (unsigned long)si.procs, (unsigned long)si.threads,
           (unsigned long)si.switches, (unsigned long)si.preemptions);
    printf("pipes: %lu open, %lu created, %lu bytes moved\n", (unsigned long)si.pipes_live,
           (unsigned long)si.pipes_created, (unsigned long)si.pipe_bytes);
    printf("fork: %lu forks, %lu pages shared, %lu copied, %lu reused, %lu frames shared now\n",
           (unsigned long)si.forks, (unsigned long)si.cow_shared, (unsigned long)si.cow_copies,
           (unsigned long)si.cow_reuses, (unsigned long)si.frames_shared);
    return 0;
}
