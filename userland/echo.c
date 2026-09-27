#include "syscall.h"

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) print_str(" ");
        print_str(argv[i]);
    }
    print_str("\n");
    return 0;
}
