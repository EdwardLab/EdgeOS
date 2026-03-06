#include <unistd.h>

int main(void) {
    const char msg[] = "hello from edgebox";
    write(1, msg, sizeof(msg) - 1);
    return 0;
}
