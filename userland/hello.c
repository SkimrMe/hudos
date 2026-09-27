#include "syscall.h"

int main(int argc, char **argv) {
    print_str("Hello from hudos-server userland!\n");
    print_str("argv[0] = ");
    if (argc > 0 && argv[0]) print_str(argv[0]);
    print_str("\nargc = ");
    print_dec(argc);
    print_str("\n");
    return 0;
}
