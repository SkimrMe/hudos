#include "syscall.h"

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    long a = 6, b = 7;
    long sum = a + b;
    long prod = a * b;
    print_str("calc: 6+7 = "); print_dec(sum);
    print_str(", 6*7 = "); print_dec(prod);
    print_str("\n");
    return 0;
}
