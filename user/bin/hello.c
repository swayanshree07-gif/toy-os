/* /bin/hello: the first ELF user program. Checks argv and its data/bss segments,
 * prints a greeting and exits with 42. */
#include "tern.h"

static int counter = 5;                 /* .data */
static char zeroed[8192];               /* .bss, spans pages: must be zero-filled */
static const char greeting[] = "hello from an ELF program";   /* .rodata */

int main(int argc, char **argv) {
    for (unsigned i = 0; i < sizeof zeroed; i++)
        if (zeroed[i]) return 10;
    zeroed[sizeof zeroed - 1] = 1;      /* bss is writable */
    counter++;
    if (counter != 6) return 11;
    if (argc < 1 || strcmp(argv[0], "hello") || argv[argc] != NULL) return 12;
    printf("[TEST] elf hello ok: %s, pid %d, argc %d", greeting, getpid(), argc);
    for (int i = 1; i < argc; i++) printf(" '%s'", argv[i]);
    printf("\n");
    return 42;
}
