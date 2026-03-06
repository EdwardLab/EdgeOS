#include <stdio.h>
#include <unistd.h>

int main(void) {
    int uid = getuid();
    int gid = getgid();
    int euid = geteuid();
    int egid = getegid();
    printf("uid=%d gid=%d euid=%d egid=%d\n", uid, gid, euid, egid);
    return 0;
}
