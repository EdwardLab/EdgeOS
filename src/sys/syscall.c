#include "sys/syscall.h"

#include "console.h"
#include "dev/fbdev.h"
#include "fb_console.h"
#include "elf/elf_loader.h"
#include "io_ports.h"
#include "isr.h"
#include "keyboard.h"
#include "drivers/e1000.h"
#include "net/lwip_stack.h"
#include "stdio.h"
#include "string.h"
#include "sys/bootlog.h"
#include "sys/boottime.h"
#include "sys/meminfo.h"
#include "sys/process.h"
#include "sys/scheduler.h"
#include "sys/user_exec.h"
#include "vfs/vfs.h"
#include "block/block.h"
#include "lwip/udp.h"
#include "lwip/tcp.h"
#include "lwip/ip_addr.h"
#include "lwip/ip6_addr.h"
#include "lwip/err.h"
#include "lwip/pbuf.h"

#define ENOSYS 38
#define EPERM 1
#define ENOENT 2
#define ESRCH 3
#define EINTR 4
#define EIO 5
#define ENXIO 6
#define ENOEXEC 8
#define ENOMEM 12
#define EACCES 13
#define EINVAL 22
#define EFAULT 14
#define EBADF 9
#define EBUSY 16
#define ERANGE 34
#define ENOTDIR 20
#define EISDIR 21
#define EAGAIN 11
#define EINPROGRESS 115
#define ECHILD 10
#define ENOTTY 25
#define EEXIST 17
#define ENOTEMPTY 39
#define EPIPE 32
#define EXDEV 18
#define EPROTONOSUPPORT 93
#define EAFNOSUPPORT 97
#define ENOTSOCK 88
#define ENOTCONN 107
#define ECONNREFUSED 111
#define ETIMEDOUT 110
#define EOPNOTSUPP 95
#define EFBIG 27
#define EOVERFLOW 75
#define EADDRNOTAVAIL 99
#define EADDRINUSE 98
#define ENETUNREACH 101
#define EHOSTUNREACH 113
#define EMFILE 24

#define USER_MIN_ADDR 0x0000000000001000ULL
#define USER_MAX_ADDR 0x0000000040000000ULL
#define USER_LOW_BASE_ADDR 0x0000000000400000ULL
#define USER_LOW_SIZE_ADDR (4ULL * 1024ULL * 1024ULL)
#define USER_TEXT_BASE_ADDR 0x0000000020000000ULL
#define USER_TEXT_SIZE_ADDR (2ULL * 1024ULL * 1024ULL)
#define BUSYBOX_CRASH_PAGE_LO 0x0000000000449000ULL
#define BUSYBOX_CRASH_PAGE_HI 0x000000000044A000ULL
#define SHELL_HEAP_PROBE_LO   0x00000000223FF000ULL
#define SHELL_HEAP_PROBE_HI   0x0000000022400000ULL
#define BB_CRASH_ADDR 0x000000000044949AULL
#define BB_CRASH_PROBE_LO (BB_CRASH_ADDR - 8ULL)
#define BB_CRASH_PROBE_LEN 24
#define USER_HEAP_BASE_ADDR 0x0000000020400000ULL
#define USER_HEAP_LIMIT_ADDR (USER_HEAP_BASE_ADDR + USER_HEAP_MAX_DELTA)
#define PAGE_SIZE 4096ULL
#define EDGE_SIGTRAMP_ADDR 0x00000000007FF000ULL

#define LINUX_AT_FDCWD (-100)
#define LINUX_O_ACCMODE 0x3
#define LINUX_O_WRONLY 0x1
#define LINUX_O_RDWR 0x2
#define LINUX_O_CREAT 0x40
#define LINUX_O_TRUNC 0x200
#define LINUX_O_APPEND 0x400
#define LINUX_O_NONBLOCK 0x800
#define LINUX_O_CLOEXEC 0x80000

#define LINUX_SEEK_SET 0
#define LINUX_SEEK_CUR 1
#define LINUX_SEEK_END 2

#define LINUX_DT_UNKNOWN 0
#define LINUX_DT_FIFO 1
#define LINUX_DT_CHR 2
#define LINUX_DT_DIR 4
#define LINUX_DT_BLK 6
#define LINUX_DT_REG 8

#define LINUX_F_DUPFD 0
#define LINUX_F_GETFD 1
#define LINUX_F_SETFD 2
#define LINUX_F_GETFL 3
#define LINUX_F_SETFL 4
#define LINUX_F_GETLK 5
#define LINUX_F_SETLK 6
#define LINUX_F_SETLKW 7
#define LINUX_FD_CLOEXEC 1
#define LINUX_F_DUPFD_CLOEXEC 1030
#define LINUX_F_RDLCK 0
#define LINUX_F_WRLCK 1
#define LINUX_F_UNLCK 2
#define LINUX_MAP_ANONYMOUS 0x20
#define LINUX_MAP_ANON LINUX_MAP_ANONYMOUS
#define LINUX_MAP_PRIVATE 0x02
#define LINUX_MAP_FIXED 0x10
#define LINUX_S_IFIFO 0x1000
#define LINUX_S_IFCHR 0x2000
#define LINUX_S_IFDIR 0x4000
#define LINUX_S_IFBLK 0x6000
#define LINUX_S_IFREG 0x8000
#define LINUX_WNOHANG 1
#define LINUX_WUNTRACED 2
#define LINUX_WCONTINUED 8
#define LINUX_PROT_READ 0x1
#define LINUX_PROT_WRITE 0x2
#define LINUX_PROT_EXEC 0x4
#define LINUX_SIGINT 2
#define LINUX_SIGCHLD 17
#define LINUX_SIGALRM 14
#define LINUX_SIGKILL 9
#define LINUX_SIGSTOP 19
#define LINUX_SIGTERM 15
#define LINUX_SIG_DFL 0ULL
#define LINUX_SIG_IGN 1ULL
#define LINUX_SIG_BLOCK 0
#define LINUX_SIG_UNBLOCK 1
#define LINUX_SIG_SETMASK 2
#define LINUX_TCGETS 0x5401u
#define LINUX_TCSETS 0x5402u
#define LINUX_TCSETSW 0x5403u
#define LINUX_TCSETSF 0x5404u
#define LINUX_TIOCGPGRP 0x540Fu
#define LINUX_TIOCSPGRP 0x5410u
#define LINUX_TIOCGWINSZ 0x5413u
#define LINUX_TIOCSWINSZ 0x5414u
#define LINUX_TIOCGPTN 0x80045430u
#define LINUX_TIOCSPTLCK 0x40045431u
#define LINUX_TIOCSCTTY 0x540Eu
#define LINUX_TIOCNOTTY 0x5422u
#define LINUX_FIONREAD  0x541Bu
#define LINUX_FIONBIO   0x5421u
#define LINUX_OPOST 0x0001u
#define LINUX_ONLCR 0x0004u
#define LINUX_ICANON 0x0002u
#define LINUX_ECHO 0x0008u
#define LINUX_ISIG 0x0001u
#define LINUX_POLLIN  0x0001
#define LINUX_POLLPRI 0x0002
#define LINUX_POLLOUT 0x0004
#define LINUX_POLLERR 0x0008
#define LINUX_POLLHUP 0x0010
#define LINUX_POLLNVAL 0x0020
#define TIMER_ABSTIME 1

#define LINUX_FUTEX_WAIT 0
#define LINUX_FUTEX_WAKE 1
#define LINUX_FUTEX_WAIT_BITSET 9
#define LINUX_FUTEX_WAKE_BITSET 10
#define LINUX_FUTEX_PRIVATE_FLAG 128
#define LINUX_FUTEX_CLOCK_REALTIME 256
#define LINUX_FUTEX_CMD_MASK (~(LINUX_FUTEX_PRIVATE_FLAG | LINUX_FUTEX_CLOCK_REALTIME))

#define LINUX_RLIMIT_NOFILE 7
#define LINUX_RLIMIT_STACK 3
#define LINUX_RLIM_INFINITY 0x7fffffffffffffffULL
#define LINUX_RUSAGE_SELF 0
#define LINUX_RUSAGE_CHILDREN (-1)
#define LINUX_MREMAP_MAYMOVE 1
#define LINUX_MREMAP_FIXED 2
#define LINUX_SS_ONSTACK 1
#define LINUX_SS_DISABLE 2
#define LINUX_MINSIGSTKSZ 2048
#define LINUX_PRIO_PROCESS 0
#define LINUX_PR_SET_PDEATHSIG 1
#define LINUX_PR_GET_PDEATHSIG 2
#define LINUX_PR_GET_DUMPABLE 3
#define LINUX_PR_SET_DUMPABLE 4
#define LINUX_PR_SET_NAME 15
#define LINUX_PR_GET_NAME 16
#define LINUX_PR_CAPBSET_READ 23
#define LINUX_PR_SET_NO_NEW_PRIVS 38
#define LINUX_PR_GET_NO_NEW_PRIVS 39
#define LINUX_AT_SYMLINK_NOFOLLOW 0x100
#define LINUX_AT_EACCESS 0x200
#define LINUX_AT_EMPTY_PATH 0x1000
#define LINUX_WAITID_P_ALL 0
#define LINUX_WAITID_P_PID 1
#define LINUX_WAITID_P_PGID 2
#define LINUX_WEXITED 0x00000004
#define LINUX_WNOWAIT 0x01000000
#define LINUX_CLD_EXITED 1
#define LINUX_SCHED_OTHER 0
#define LINUX_SCHED_FIFO 1
#define LINUX_SCHED_RR 2

#define ARCH_SET_FS 0x1002
#define ARCH_GET_FS 0x1003

#define SYS_stat 4
#define SYS_poll 7
#define SYS_sched_yield 24
#define SYS_mremap 25
#define SYS_madvise 28
#define SYS_lseek 8
#define SYS_fstat 5
#define SYS_lstat 6
#define SYS_mmap 9
#define SYS_mprotect 10
#define SYS_munmap 11
#define SYS_getcwd 79
#define SYS_chdir 80
#define SYS_fchdir 81
#define SYS_mkdir 83
#define SYS_rename 82
#define SYS_rmdir 84
#define SYS_link 86
#define SYS_unlink 87
#define SYS_gettimeofday 96
#define SYS_getrlimit 97
#define SYS_getrusage 98
#define SYS_sysinfo 99
#define SYS_times 100
#define SYS_sigaltstack 131
#define SYS_umask 95
#define SYS_chmod 90
#define SYS_chown 92
#define SYS_fchmod 91
#define SYS_fchown 93
#define SYS_readlink 89
#define SYS_readv 19
#define SYS_writev 20
#define SYS_rt_sigaction 13
#define SYS_rt_sigprocmask 14
#define SYS_rt_sigreturn 15
#define SYS_ioctl 16
#define SYS_socket 41
#define SYS_sendfile 40
#define SYS_connect 42
#define SYS_accept 43
#define SYS_sendto 44
#define SYS_recvfrom 45
#define SYS_sendmsg 46
#define SYS_recvmsg 47
#define SYS_shutdown 48
#define SYS_bind 49
#define SYS_listen 50
#define SYS_getsockname 51
#define SYS_getpeername 52
#define SYS_socketpair 53
#define SYS_setsockopt 54
#define SYS_getsockopt 55
#define SYS_access 21
#define SYS_pipe 22
#define SYS_dup 32
#define SYS_dup2 33
#define SYS_nanosleep 35
#define SYS_getitimer 36
#define SYS_alarm 37
#define SYS_setitimer 38
#define SYS_clone 56
#define SYS_fork 57
#define SYS_vfork 58
#define SYS_wait4 61
#define SYS_exit 60
#define SYS_kill 62
#define SYS_uname 63
#define SYS_sethostname 170
#define SYS_setrlimit 160
#define SYS_fcntl 72
#define SYS_fsync 74
#define SYS_fdatasync 75
#define SYS_truncate 76
#define SYS_ftruncate 77
#define SYS_statfs 137
#define SYS_fstatfs 138
#define SYS_getpriority 140
#define SYS_setpriority 141
#define SYS_sched_getparam 143
#define SYS_sched_setscheduler 144
#define SYS_getdents64 217
#define SYS_gettid 186
#define SYS_futex 202
#define SYS_sched_setaffinity 203
#define SYS_sched_getaffinity 204
#define SYS_pidfd_send_signal 424
#define SYS_io_uring_setup 425
#define SYS_io_uring_enter 426
#define SYS_io_uring_register 427
#define SYS_set_tid_address 218
#define SYS_clock_gettime 228
#define SYS_clock_nanosleep 230
#define SYS_exit_group 231
#define SYS_tgkill 234
#define SYS_waitid 247
#define SYS_openat 257
#define SYS_renameat 264
#define SYS_linkat 265
#define SYS_symlinkat 266
#define SYS_readlinkat 267
#define SYS_newfstatat 262
#define SYS_utimensat 280
#define SYS_unshare 272
#define SYS_set_robust_list 273
#define SYS_splice 275
#define SYS_tee 276
#define SYS_vmsplice 278
#define SYS_prlimit64 302
#define SYS_syncfs 306
#define SYS_setns 308
#define SYS_getcpu 309
#define SYS_dup3 292
#define SYS_pipe2 293
#define SYS_renameat2 316
#define SYS_getrandom 318
#define SYS_execveat 322
#define SYS_copy_file_range 326
#define SYS_statx 332
#define SYS_getuid 102
#define SYS_setuid 105
#define SYS_setgid 106
#define SYS_getgid 104
#define SYS_geteuid 107
#define SYS_getegid 108
#define SYS_getppid 110
#define SYS_setpgid 109
#define SYS_getpgrp 111
#define SYS_setsid 112
#define SYS_setreuid 113
#define SYS_setregid 114
#define SYS_getgroups 115
#define SYS_setgroups 116
#define SYS_setresuid 117
#define SYS_getresuid 118
#define SYS_setresgid 119
#define SYS_getresgid 120
#define SYS_getpgid 121
#define SYS_setfsuid 122
#define SYS_setfsgid 123
#define SYS_getsid 124
#define SYS_capget 125
#define SYS_capset 126
#define SYS_pivot_root 155
#define SYS_prctl 157
#define SYS_arch_prctl 158
#define SYS_chroot 161
#define SYS_sync 162
#define SYS_mount 165
#define SYS_select 23
#define SYS_pselect6 270
#define SYS_ppoll 271
#define SYS_signalfd 282
#define SYS_timerfd_create 283
#define SYS_eventfd 284
#define SYS_fallocate 285
#define SYS_timerfd_settime 286
#define SYS_timerfd_gettime 287
#define SYS_accept4 288
#define SYS_signalfd4 289
#define SYS_eventfd2 290
#define SYS_epoll_create1 291
#define SYS_openat2 437
#define SYS_faccessat2 439
#define SYS_pidfd_open 434
#define SYS_clone3 435
#define SYS_epoll_create 213
#define SYS_epoll_wait 232
#define SYS_epoll_ctl 233

#define LINUX_AF_UNIX 1
#define LINUX_AF_INET 2
#define LINUX_AF_INET6 10
#define LINUX_SOCK_STREAM 1
#define LINUX_SOCK_DGRAM 2
#define LINUX_SOCK_RAW 3
#define LINUX_SOCK_NONBLOCK 0x800
#define LINUX_SOCK_CLOEXEC 0x80000
#define LINUX_CLONE_PIDFD 0x00001000ull
#define LINUX_CLONE_VM 0x00000100ull
#define LINUX_CLONE_VFORK 0x00004000ull
#define LINUX_CLONE_THREAD 0x00010000ull
#define LINUX_CLONE_SIGHAND 0x00000800ull
#define LINUX_CLONE_FILES 0x00000400ull
#define LINUX_CLONE_FS 0x00000200ull
#define LINUX_CLONE_PARENT_SETTID 0x00100000ull
#define LINUX_CLONE_CHILD_SETTID 0x01000000ull
#define LINUX_CLONE_SETTLS 0x00080000ull
#define LINUX_CLONE_CHILD_CLEARTID 0x00200000ull
#define LINUX_CAP_VERSION_1 0x19980330u
#define LINUX_CAP_VERSION_2 0x20071026u
#define LINUX_CAP_VERSION_3 0x20080522u
#define LINUX_EFD_SEMAPHORE 1
#define LINUX_EFD_CLOEXEC LINUX_O_CLOEXEC
#define LINUX_EFD_NONBLOCK LINUX_O_NONBLOCK
#define LINUX_TFD_NONBLOCK LINUX_O_NONBLOCK
#define LINUX_TFD_CLOEXEC LINUX_O_CLOEXEC
#define LINUX_TFD_TIMER_ABSTIME 1
#define LINUX_SFD_NONBLOCK LINUX_O_NONBLOCK
#define LINUX_SFD_CLOEXEC LINUX_O_CLOEXEC
#define LINUX_EPOLL_CLOEXEC LINUX_O_CLOEXEC
#define LINUX_EPOLL_CTL_ADD 1
#define LINUX_EPOLL_CTL_DEL 2
#define LINUX_EPOLL_CTL_MOD 3
#define LINUX_EPOLLIN 0x001u
#define LINUX_EPOLLOUT 0x004u
#define LINUX_EPOLLERR 0x008u
#define LINUX_EPOLLHUP 0x010u
#define LINUX_IPPROTO_ICMP 1
#define LINUX_IPPROTO_TCP 6
#define LINUX_IPPROTO_UDP 17
#define LINUX_IPPROTO_ICMPV6 58

#define LINUX_SIOCGIFCONF 0x8912u
#define LINUX_SIOCGIFFLAGS 0x8913u
#define LINUX_SIOCSIFFLAGS 0x8914u
#define LINUX_SIOCGIFADDR 0x8915u
#define LINUX_SIOCSIFADDR 0x8916u
#define LINUX_SIOCGIFDSTADDR 0x8917u
#define LINUX_SIOCSIFDSTADDR 0x8918u
#define LINUX_SIOCGIFBRDADDR 0x8919u
#define LINUX_SIOCSIFBRDADDR 0x891au
#define LINUX_SIOCGIFNETMASK 0x891bu
#define LINUX_SIOCSIFNETMASK 0x891cu
#define LINUX_SIOCGIFMETRIC 0x891du
#define LINUX_SIOCGIFMTU 0x8921u
#define LINUX_SIOCSIFMTU 0x8922u
#define LINUX_SIOCGIFHWADDR 0x8927u
#define LINUX_SIOCGIFINDEX 0x8933u
#define LINUX_SIOCGIFTXQLEN 0x8942u
#define LINUX_SIOCSIFTXQLEN 0x8943u

#define LINUX_IFF_UP 0x1u
#define LINUX_IFF_BROADCAST 0x2u
#define LINUX_IFF_LOOPBACK 0x8u
#define LINUX_IFF_RUNNING 0x40u
#define LINUX_IFF_MULTICAST 0x1000u

#define LINUX_ARPHRD_ETHER 1u
#define LINUX_ARPHRD_LOOPBACK 772u

#define LINUX_SOL_SOCKET 1
#define LINUX_SO_ERROR 4
#define LINUX_SO_BINDTODEVICE 25
#define LINUX_SO_RCVBUF 8
#define LINUX_SO_BROADCAST 6
#define LINUX_SO_RCVTIMEO 20
#define LINUX_GRND_NONBLOCK 0x0001
#define LINUX_SOL_IP 0
#define LINUX_IP_TTL 2
#define LINUX_IP_MULTICAST_TTL 33
#define LINUX_SOL_RAW 255
#define LINUX_IPV6_CHECKSUM 7
#define LINUX_SOL_IPV6 41
#define LINUX_IPV6_HOPLIMIT 52

#define LINUX_ITIMER_REAL 0

#define EDGE_MAX_FD 64
#define EDGE_MAX_FD_PROCS PROC_MAX_TASKS
#define EDGE_MAX_PIPES 32
#define EDGE_PIPE_SIZE 4096
#define EDGE_MAX_SOCKETS 32
#define EDGE_SOCK_ACCEPTQ 8
#define EDGE_MAX_PTYS 16
#define EDGE_PTY_BUF_SIZE 4096
#define EDGE_SELECT_FD_MAX 1024
#define EDGE_SELECT_FD_BYTES (EDGE_SELECT_FD_MAX / 8)
#define PTE_PRESENT 0x001ULL
#define PTE_WRITE 0x002ULL
#define PTE_USER 0x004ULL
#define PTE_PS 0x080ULL

struct edge_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct edge_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct edge_linux_rlimit {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

struct edge_linux_stack_t {
    uint64_t ss_sp;
    int32_t ss_flags;
    int32_t __pad;
    uint64_t ss_size;
};

struct edge_linux_rusage {
    struct edge_timeval ru_utime;
    struct edge_timeval ru_stime;
    int64_t ru_maxrss;
    int64_t ru_ixrss;
    int64_t ru_idrss;
    int64_t ru_isrss;
    int64_t ru_minflt;
    int64_t ru_majflt;
    int64_t ru_nswap;
    int64_t ru_inblock;
    int64_t ru_oublock;
    int64_t ru_msgsnd;
    int64_t ru_msgrcv;
    int64_t ru_nsignals;
    int64_t ru_nvcsw;
    int64_t ru_nivcsw;
};

struct edge_linux_sigaction {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
};

struct edge_itimerval {
    struct edge_timeval it_interval;
    struct edge_timeval it_value;
};

struct edge_linux_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    struct edge_timespec st_atim;
    struct edge_timespec st_mtim;
    struct edge_timespec st_ctim;
    int64_t __unused[3];
};

struct edge_linux_sysinfo {
    int64_t uptime;
    uint64_t loads[3];
    uint64_t totalram;
    uint64_t freeram;
    uint64_t sharedram;
    uint64_t bufferram;
    uint64_t totalswap;
    uint64_t freeswap;
    uint16_t procs;
    uint16_t pad;
    uint64_t totalhigh;
    uint64_t freehigh;
    uint32_t mem_unit;
    uint8_t _f[0];
};

struct edge_linux_fsid_t {
    int32_t val[2];
};

struct edge_linux_statfs {
    int64_t f_type;
    int64_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    struct edge_linux_fsid_t f_fsid;
    int64_t f_namelen;
    int64_t f_frsize;
    int64_t f_flags;
    int64_t f_spare[4];
};

struct edge_linux_tms {
    int64_t tms_utime;
    int64_t tms_stime;
    int64_t tms_cutime;
    int64_t tms_cstime;
};

struct edge_linux_sched_param {
    int32_t sched_priority;
};

struct edge_linux_siginfo_min {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t _pad0;
    int32_t si_pid;
    int32_t si_uid;
    int32_t si_status;
    int32_t _pad1;
    uint8_t _rest[128 - 32];
};

struct edge_linux_statx_timestamp {
    int64_t tv_sec;
    uint32_t tv_nsec;
    int32_t __reserved;
};

struct edge_linux_statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t __spare0;
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    struct edge_linux_statx_timestamp stx_atime;
    struct edge_linux_statx_timestamp stx_btime;
    struct edge_linux_statx_timestamp stx_ctime;
    struct edge_linux_statx_timestamp stx_mtime;
    uint32_t stx_rdev_major;
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major;
    uint32_t stx_dev_minor;
    uint64_t stx_mnt_id;
    uint64_t stx_dio_mem_align;
    uint64_t stx_dio_offset_align;
    uint64_t __spare3[12];
};

struct edge_linux_open_how {
    uint64_t flags;
    uint64_t mode;
    uint64_t resolve;
};

struct edge_linux_clone_args {
    uint64_t flags;
    uint64_t pidfd;
    uint64_t child_tid;
    uint64_t parent_tid;
    uint64_t exit_signal;
    uint64_t stack;
    uint64_t stack_size;
    uint64_t tls;
    uint64_t set_tid;
    uint64_t set_tid_size;
    uint64_t cgroup;
};

struct edge_linux_cap_user_header {
    uint32_t version;
    int32_t pid;
};

struct edge_linux_cap_user_data {
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
};

struct edge_itimerspec {
    struct edge_timespec it_interval;
    struct edge_timespec it_value;
};

struct edge_linux_epoll_event {
    uint32_t events;
    uint32_t _pad;
    uint64_t data;
};

struct edge_linux_signalfd_siginfo {
    uint32_t ssi_signo;
    int32_t ssi_errno;
    int32_t ssi_code;
    uint32_t ssi_pid;
    uint32_t ssi_uid;
    int32_t ssi_fd;
    uint32_t ssi_tid;
    uint32_t ssi_band;
    uint32_t ssi_overrun;
    uint32_t ssi_trapno;
    int32_t ssi_status;
    int32_t ssi_int;
    uint64_t ssi_ptr;
    uint64_t ssi_utime;
    uint64_t ssi_stime;
    uint64_t ssi_addr;
    uint8_t pad[48];
};

struct edge_linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
};

struct edge_linux_flock {
    int16_t l_type;
    int16_t l_whence;
    int64_t l_start;
    int64_t l_len;
    int32_t l_pid;
};

struct edge_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

struct edge_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

struct edge_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[19];
};

#define LINUX_VINTR 0
#define LINUX_VERASE 2
#define LINUX_VKILL 3
#define LINUX_VEOF 4
#define LINUX_VTIME 5
#define LINUX_VMIN 6

struct edge_iovec {
    uint64_t iov_base;
    uint64_t iov_len;
};

struct edge_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

struct edge_linux_pselect_sigset {
    uint64_t sigmask_u;
    uint64_t sigsetsize;
};

typedef enum {
    FD_NONE = 0,
    FD_CONSOLE,
    FD_VFS,
    FD_PIPE_R,
    FD_PIPE_W,
    FD_SOCKET,
    FD_PTY_MASTER,
    FD_PTY_SLAVE,
    FD_EVENTFD,
    FD_TIMERFD,
    FD_SIGNALFD,
    FD_EPOLL,
    FD_PIDFD,
} edge_fd_kind_t;

typedef struct {
    int used;
    edge_fd_kind_t kind;
    int file_ref;
    int flags;
    int fd_flags;
    uint64_t pos;
    vfs_inode_t inode;
    vfs_superblock_t *sb;
    char path[256];
    int pipe_id;
} edge_fd_t;

#define EDGE_MAX_FILE_REFS 1024
typedef struct {
    int used;
    int refs;
} edge_file_ref_t;

typedef struct {
    int pid;
    edge_fd_t fds[EDGE_MAX_FD];
} edge_fd_proc_t;

typedef struct {
    int used;
    int readers;
    int writers;
    uint32_t rpos;
    uint32_t wpos;
    uint32_t count;
    uint8_t buf[EDGE_PIPE_SIZE];
} edge_pipe_t;

struct edge_sockaddr {
    uint16_t sa_family;
    char sa_data[14];
};

struct edge_sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    uint8_t sin_zero[8];
};

struct edge_sockaddr_in6 {
    uint16_t sin6_family;
    uint16_t sin6_port;
    uint32_t sin6_flowinfo;
    uint8_t sin6_addr[16];
    uint32_t sin6_scope_id;
};

struct edge_linux_ifreq {
    char ifr_name[16];
    union {
        struct edge_sockaddr ifru_addr;
        struct edge_sockaddr ifru_dstaddr;
        struct edge_sockaddr ifru_broadaddr;
        struct edge_sockaddr ifru_netmask;
        struct edge_sockaddr ifru_hwaddr;
        int32_t ifru_flags;
        int32_t ifru_ivalue;
        int32_t ifru_mtu;
        int32_t ifru_ifindex;
        int32_t ifru_qlen;
    } ifr_ifru;
};

struct edge_linux_ifconf {
    int32_t ifc_len;
    int32_t __pad;
    uint64_t ifc_buf;
};

struct edge_linux_iovec {
    uint64_t iov_base;
    uint64_t iov_len;
};

struct edge_linux_msghdr {
    uint64_t msg_name;
    uint32_t msg_namelen;
    uint32_t __pad0;
    uint64_t msg_iov;
    int32_t msg_iovlen;
    int32_t __pad1;
    uint64_t msg_control;
    uint32_t msg_controllen;
    uint32_t __pad2;
    int32_t msg_flags;
    int32_t __pad3;
};

typedef struct {
    int up;
    char name[16];
    int ifindex;
    uint32_t flags;
    uint8_t mac[6];
    uint32_t mtu;
    uint32_t ipv4_addr_be;
    uint32_t ipv4_netmask_be;
    uint32_t ipv4_bcast_be;
    uint32_t ipv4_dst_be;
} edge_netif_t;

typedef struct {
    int used;
    int refs;
    int domain;
    int type;
    int protocol;
    int nonblock;
    uint64_t recv_timeout_us;
    int connected;
    int connect_in_progress;
    int connect_error;
    int closed;
    uint8_t peer_addr[28];
    uint32_t peer_len;
    uint8_t bind_addr[28];
    uint32_t bind_len;
    uint8_t rx_buf[32768];
    uint32_t rx_len;
    uint8_t rx_peer[28];
    uint32_t rx_peer_len;
    int ping_hw;
    uint16_t ping_id_be;
    uint8_t ping_req[256];
    uint32_t ping_req_len;
    uint8_t ping_peer[28];
    uint32_t ping_peer_len;
    uint16_t ping_next_seq_be;
    uint8_t ip_ttl;
    void *lwip_pcb;
    uint16_t local_port_be;
    int unix_peer_id;
    int listening;
    int backlog;
    int pending_head;
    int pending_tail;
    int pending_count;
    int pending_sock_ids[EDGE_SOCK_ACCEPTQ];
} edge_socket_t;

#define EDGE_MAX_EVENTFDS 64
#define EDGE_MAX_TIMERFDS 64
#define EDGE_MAX_SIGNALFDS 64
#define EDGE_MAX_EPOLLS 32
#define EDGE_EPOLL_MAX_WATCH 64

typedef struct {
    int used;
    int refs;
    uint64_t counter;
    uint8_t semaphore;
} edge_eventfd_t;

typedef struct {
    int used;
    int refs;
    int clockid;
    uint8_t active;
    uint64_t next_us;
    uint64_t interval_us;
} edge_timerfd_t;

typedef struct {
    int used;
    int refs;
    uint64_t mask;
} edge_signalfd_t;

typedef struct {
    int fd;
    uint32_t events;
    uint64_t data;
} edge_epoll_watch_t;

typedef struct {
    int used;
    int refs;
    int nwatch;
    edge_epoll_watch_t watch[EDGE_EPOLL_MAX_WATCH];
} edge_epoll_t;

typedef struct {
    int used;
    int refs_master;
    int refs_slave;
    int unlocked;
    int fg_pgid;
    struct edge_termios termios;
    struct edge_winsize winsz;
    uint32_t m2s_rpos;
    uint32_t m2s_wpos;
    uint32_t m2s_count;
    uint8_t m2s_buf[EDGE_PTY_BUF_SIZE];
    uint32_t s2m_rpos;
    uint32_t s2m_wpos;
    uint32_t s2m_count;
    uint8_t s2m_buf[EDGE_PTY_BUF_SIZE];
} edge_pty_t;

static edge_fd_proc_t g_fd_procs[EDGE_MAX_FD_PROCS];
static edge_file_ref_t g_file_refs[EDGE_MAX_FILE_REFS];
static edge_pipe_t g_pipes[EDGE_MAX_PIPES];
static edge_socket_t g_sockets[EDGE_MAX_SOCKETS];
static edge_pty_t g_ptys[EDGE_MAX_PTYS];
static edge_eventfd_t g_eventfds[EDGE_MAX_EVENTFDS];
static edge_timerfd_t g_timerfds[EDGE_MAX_TIMERFDS];
static edge_signalfd_t g_signalfds[EDGE_MAX_SIGNALFDS];
static edge_epoll_t g_epolls[EDGE_MAX_EPOLLS];
static uint16_t g_next_ephemeral_port = 49152;
static uint64_t g_getrandom_state = 0xA5A5C3C35A5A3C3CULL;
static struct edge_termios g_tty_termios;
static int g_tty_foreground_pgid;
static char g_tty_linebuf[512];
static int g_tty_line_len;
static int g_tty_line_pos;
static int g_tty_read_log_pids[64];
static int g_tty_read_log_count;
static int g_tty_fg_fix_log_pids[64];
static int g_tty_fg_fix_log_count;
static int g_tty_ioctl_log_pids[64];
static int g_tty_ioctl_log_count;
static int g_fb_console_hold_count;
static edge_netif_t g_if_lo;
static edge_netif_t g_if_eth0;
#if EDGE_BB_FD_TRACE
static int g_bb_fd_trace_budget = 64;
#endif

static void termios_init_sane(struct edge_termios *t) {
    if (!t) return;
    memset(t, 0, sizeof(*t));
    t->c_oflag = LINUX_OPOST | LINUX_ONLCR;
    t->c_lflag = LINUX_ICANON | LINUX_ECHO | LINUX_ISIG;
    t->c_cc[LINUX_VINTR] = 3;
    t->c_cc[LINUX_VERASE] = 127;
    t->c_cc[LINUX_VKILL] = 21;
    t->c_cc[LINUX_VEOF] = 4;
    t->c_cc[LINUX_VTIME] = 0;
    t->c_cc[LINUX_VMIN] = 1;
}

static void fd_ensure_stdio(edge_fd_proc_t *p);
static int fd_proc_has_pty_fd(int pid);
static uint64_t do_sys_close(uint64_t fd_u);
static uint64_t do_sys_sleep(uint64_t ms);
static void pipe_drop_reader(int pipe_id);
static void pipe_drop_writer(int pipe_id);
static void socket_drop_ref(int sock_id);
static edge_fd_proc_t *fd_proc_with_stdio(void);
static edge_fd_t *fd_get(edge_fd_proc_t *p, int fd);
static uint64_t do_sys_sendto(uint64_t fd_u, uint64_t buf_u, uint64_t len_u, uint64_t flags_u, uint64_t addr_u, uint64_t addrlen_u);
static uint64_t do_sys_recvfrom(uint64_t fd_u, uint64_t buf_u, uint64_t len_u, uint64_t flags_u, uint64_t addr_u, uint64_t addrlen_u);
static uint64_t do_sys_socketpair(uint64_t domain_u, uint64_t type_u, uint64_t protocol_u, uint64_t sv_u);
static uint64_t do_sys_listen(uint64_t fd_u, uint64_t backlog_u);
static uint64_t do_sys_accept(uint64_t fd_u, uint64_t addr_u, uint64_t len_u);
static uint64_t do_sys_accept4(uint64_t fd_u, uint64_t addr_u, uint64_t len_u, uint64_t flags_u);
static uint64_t do_sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t off);
static uint64_t do_sys_munmap(uint64_t addr, uint64_t len);
static uint64_t do_sys_prlimit64(uint64_t pid, uint64_t resource, uint64_t new_u, uint64_t old_u);
static void fd_log_lifecycle(const char *ev, int pid, int fd, const edge_fd_t *e, int extra);
static void fd_debug_slot_once(const char *tag, int pid, int fd, const edge_fd_t *e);
static int file_ref_alloc(void);
static int file_ref_get(int id);
static int file_ref_put(int id);
static int path_is_console_tty(const char *path);
static int path_is_mouse_input(const char *path);
static int pty_alloc(void);
static void pty_add_ref(int pty_id, int is_master);
static void pty_drop_ref(int pty_id, int is_master);
static uint64_t do_sys_fd_read(uint64_t fd_u, uint64_t buf_u, uint64_t len_u);
static uint64_t do_sys_fd_write(uint64_t fd_u, uint64_t buf_u, uint64_t len_u);
static int alloc_special_fd(edge_fd_kind_t kind, int obj_id, int flags);

static int eventfd_alloc_obj(uint64_t initval, int semaphore) {
    for (int i = 0; i < EDGE_MAX_EVENTFDS; ++i) {
        if (!g_eventfds[i].used) {
            memset(&g_eventfds[i], 0, sizeof(g_eventfds[i]));
            g_eventfds[i].used = 1;
            g_eventfds[i].refs = 1;
            g_eventfds[i].counter = initval;
            g_eventfds[i].semaphore = semaphore ? 1 : 0;
            return i;
        }
    }
    return -1;
}
static void eventfd_add_ref(int id) { if (id >= 0 && id < EDGE_MAX_EVENTFDS && g_eventfds[id].used) g_eventfds[id].refs++; }
static void eventfd_drop_ref(int id) {
    if (id < 0 || id >= EDGE_MAX_EVENTFDS || !g_eventfds[id].used) return;
    if (--g_eventfds[id].refs <= 0) memset(&g_eventfds[id], 0, sizeof(g_eventfds[id]));
}
static int timerfd_alloc_obj(int clockid) {
    for (int i = 0; i < EDGE_MAX_TIMERFDS; ++i) {
        if (!g_timerfds[i].used) {
            memset(&g_timerfds[i], 0, sizeof(g_timerfds[i]));
            g_timerfds[i].used = 1;
            g_timerfds[i].refs = 1;
            g_timerfds[i].clockid = clockid;
            return i;
        }
    }
    return -1;
}
static void timerfd_add_ref(int id) { if (id >= 0 && id < EDGE_MAX_TIMERFDS && g_timerfds[id].used) g_timerfds[id].refs++; }
static void timerfd_drop_ref(int id) {
    if (id < 0 || id >= EDGE_MAX_TIMERFDS || !g_timerfds[id].used) return;
    if (--g_timerfds[id].refs <= 0) memset(&g_timerfds[id], 0, sizeof(g_timerfds[id]));
}
static int signalfd_alloc_obj(uint64_t mask) {
    for (int i = 0; i < EDGE_MAX_SIGNALFDS; ++i) {
        if (!g_signalfds[i].used) {
            memset(&g_signalfds[i], 0, sizeof(g_signalfds[i]));
            g_signalfds[i].used = 1;
            g_signalfds[i].refs = 1;
            g_signalfds[i].mask = mask;
            return i;
        }
    }
    return -1;
}
static void signalfd_add_ref(int id) { if (id >= 0 && id < EDGE_MAX_SIGNALFDS && g_signalfds[id].used) g_signalfds[id].refs++; }
static void signalfd_drop_ref(int id) {
    if (id < 0 || id >= EDGE_MAX_SIGNALFDS || !g_signalfds[id].used) return;
    if (--g_signalfds[id].refs <= 0) memset(&g_signalfds[id], 0, sizeof(g_signalfds[id]));
}
static int epoll_alloc_obj(void) {
    for (int i = 0; i < EDGE_MAX_EPOLLS; ++i) {
        if (!g_epolls[i].used) {
            memset(&g_epolls[i], 0, sizeof(g_epolls[i]));
            g_epolls[i].used = 1;
            g_epolls[i].refs = 1;
            return i;
        }
    }
    return -1;
}
static void epoll_add_ref(int id) { if (id >= 0 && id < EDGE_MAX_EPOLLS && g_epolls[id].used) g_epolls[id].refs++; }
static void epoll_drop_ref(int id) {
    if (id < 0 || id >= EDGE_MAX_EPOLLS || !g_epolls[id].used) return;
    if (--g_epolls[id].refs <= 0) memset(&g_epolls[id], 0, sizeof(g_epolls[id]));
}

#ifndef EDGE_SYSCALL_DEBUG
#define EDGE_SYSCALL_DEBUG 0
#endif
#ifndef EDGE_BB_FD_TRACE
#define EDGE_BB_FD_TRACE 0
#endif
#ifndef EDGE_TTY_DEBUG
#define EDGE_TTY_DEBUG 0
#endif
#ifndef EDGE_TTY_JOBCONTROL_COMPAT
#define EDGE_TTY_JOBCONTROL_COMPAT 1
#endif
#ifndef EDGE_FD_FORK_DEBUG
#define EDGE_FD_FORK_DEBUG 0
#endif
#ifndef EDGE_SSH_IO_DEBUG
#define EDGE_SSH_IO_DEBUG 0
#endif
#ifndef EDGE_USER_TEXT_WRITE_DEBUG
#define EDGE_USER_TEXT_WRITE_DEBUG 0
#endif

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1

static inline uint64_t page_align_up(uint64_t v) {
    return (v + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

static uint64_t g_syscall_debug_nr;
static uint64_t g_syscall_debug_rip;
static int g_syscall_debug_pid;
static int g_syscall_text_write_log_budget = 32;

static inline uint64_t read_cr3_local(void) {
    uint64_t v;
    __asm__ __volatile__("mov %%cr3, %0" : "=r"(v));
    return v;
}

static uint64_t user_pte_flags(uint64_t va) {
    uint64_t cr3 = read_cr3_local() & ~0xFFFULL;
    uint64_t *pml4 = (uint64_t *)(uintptr_t)cr3;
    uint64_t pml4e = pml4[(va >> 39) & 0x1FF];
    if (!(pml4e & PTE_PRESENT)) return 0;

    uint64_t *pdpt = (uint64_t *)(uintptr_t)(pml4e & ~0xFFFULL);
    uint64_t pdpte = pdpt[(va >> 30) & 0x1FF];
    if (!(pdpte & PTE_PRESENT)) return 0;
    if (pdpte & PTE_PS) return pdpte & 0xFFFULL;

    uint64_t *pd = (uint64_t *)(uintptr_t)(pdpte & ~0xFFFULL);
    uint64_t pde = pd[(va >> 21) & 0x1FF];
    if (!(pde & PTE_PRESENT)) return 0;
    if (pde & PTE_PS) return pde & 0xFFFULL;

    uint64_t *pt = (uint64_t *)(uintptr_t)(pde & ~0xFFFULL);
    uint64_t pte = pt[(va >> 12) & 0x1FF];
    if (!(pte & PTE_PRESENT)) return 0;
    return pte & 0xFFFULL;
}

static int user_range_ok(uint64_t addr, uint64_t len) {
    if (addr < USER_MIN_ADDR) return 0;
    if (addr >= USER_MAX_ADDR) return 0;
    if (len == 0) return 1;
    if (addr + len < addr) return 0;
    if (addr + len > USER_MAX_ADDR) return 0;
    return 1;
}

static int copy_from_user(void *dst, uint64_t src_u, uint64_t len) {
    if (!user_range_ok(src_u, len)) return -1;
    memcpy(dst, (const void *)(uintptr_t)src_u, (uint32_t)len);
    return 0;
}

static int copy_to_user(uint64_t dst_u, const void *src, uint64_t len) {
    if (!user_range_ok(dst_u, len)) return -1;
#if EDGE_USER_TEXT_WRITE_DEBUG
    if (len && g_syscall_text_write_log_budget > 0) {
        uint64_t text_lo = USER_TEXT_BASE_ADDR;
        uint64_t text_hi = USER_TEXT_BASE_ADDR + USER_TEXT_SIZE_ADDR;
        uint64_t low_lo = USER_LOW_BASE_ADDR;
        uint64_t low_hi = USER_LOW_BASE_ADDR + USER_LOW_SIZE_ADDR;
        uint64_t bb_lo = BUSYBOX_CRASH_PAGE_LO;
        uint64_t bb_hi = BUSYBOX_CRASH_PAGE_HI;
        uint64_t hp_lo = SHELL_HEAP_PROBE_LO;
        uint64_t hp_hi = SHELL_HEAP_PROBE_HI;
        uint64_t end = dst_u + len;
        int hit_text;
        int hit_low;
        int hit_bb;
        int hit_heap_probe;
        if (end < dst_u) end = ~0ULL;
        hit_text = !(end <= text_lo || dst_u >= text_hi);
        hit_low  = !(end <= low_lo  || dst_u >= low_hi);
        hit_bb   = !(end <= bb_lo   || dst_u >= bb_hi);
        hit_heap_probe = !(end <= hp_lo || dst_u >= hp_hi);
        if (hit_text || hit_bb || hit_heap_probe) {
            task_t *t = process_current_task();
            const uint8_t *b = (const uint8_t *)src;
            printf("[uwrite-user] pid=%d task=%s nr=%d user_rip=0x%x dst=0x%x len=0x%x zone=%s%s%s%s src=%x %x %x %x\n",
                   t ? t->pid : g_syscall_debug_pid,
                   t ? t->name : "?",
                   (int)g_syscall_debug_nr,
                   (uint32)g_syscall_debug_rip,
                   (uint32)dst_u,
                   (uint32)len,
                   hit_bb ? "bb" : "",
                   hit_text ? (hit_bb ? "+text" : "text") : "",
                   hit_heap_probe ? ((hit_bb || hit_text) ? "+heap" : "heap") : "",
                   (!hit_bb && !hit_text && hit_low) ? "low" : "",
                   (len > 0 && b) ? (uint32)b[0] : 0,
                   (len > 1 && b) ? (uint32)b[1] : 0,
                   (len > 2 && b) ? (uint32)b[2] : 0,
                   (len > 3 && b) ? (uint32)b[3] : 0);
            g_syscall_text_write_log_budget--;
        }
    }
#endif
    memcpy((void *)(uintptr_t)dst_u, src, (uint32_t)len);
    return 0;
}

static int copy_user_cstr(char *dst, int dst_sz, uint64_t src_u) {
    if (!dst || dst_sz <= 1) return -1;
    if (!src_u) {
        dst[0] = 0;
        return 0;
    }
    for (int i = 0; i < dst_sz - 1; ++i) {
        char c = 0;
        if (copy_from_user(&c, src_u + (uint64_t)i, 1) < 0) return -1;
        dst[i] = c;
        if (!c) return 0;
    }
    dst[dst_sz - 1] = 0;
    return -1;
}

static uint64_t timeval_to_us(const struct edge_timeval *tv) {
    if (!tv) return 0;
    if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000) return 0;
    return (uint64_t)tv->tv_sec * 1000000ull + (uint64_t)tv->tv_usec;
}

static uint64_t timespec_to_us_checked(const struct edge_timespec *ts, int *ok) {
    if (ok) *ok = 0;
    if (!ts) return 0;
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000ll) return 0;
    if (ok) *ok = 1;
    return (uint64_t)ts->tv_sec * 1000000ull + (uint64_t)(ts->tv_nsec / 1000ll);
}

static uint64_t rdtsc64_local(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wait_blocking_step(void) {
    /* Prevent one blocking syscall from starving the rest of userspace. */
    scheduler_yield();
    __asm__ __volatile__("sti; hlt");
}

static uint64_t rand_mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27;
    x *= 0x94D049BB133111EBULL;
    x ^= x >> 31;
    return x;
}

static uint8_t kernel_random_byte(void) {
    uint64_t tsc = rdtsc64_local();
    uint64_t irq = keyboard_entropy_irq_count();
    uint64_t last = keyboard_entropy_last_tsc();
    uint64_t now = boottime_monotonic_us();
    g_getrandom_state ^= rand_mix64(tsc ^ (irq << 1) ^ (last << 7) ^ (now << 13));
    g_getrandom_state ^= g_getrandom_state << 7;
    g_getrandom_state ^= g_getrandom_state >> 9;
    g_getrandom_state ^= g_getrandom_state << 8;
    return (uint8_t)g_getrandom_state;
}

static void us_to_timeval(uint64_t us, struct edge_timeval *tv) {
    if (!tv) return;
    tv->tv_sec = (int64_t)(us / 1000000ull);
    tv->tv_usec = (int64_t)(us % 1000000ull);
}

static void task_timer_poll(task_t *t) {
    uint64_t now;
    if (!t || !t->itimer_real_active) return;
    now = boottime_monotonic_us();
    if (now < t->itimer_real_next_us) return;
    t->sigalrm_pending = 1;
    if (t->itimer_real_interval_us > 0) {
        do {
            t->itimer_real_next_us += t->itimer_real_interval_us;
        } while (t->itimer_real_next_us <= now);
    } else {
        t->itimer_real_active = 0;
        t->itimer_real_next_us = 0;
    }
}

static uint32_t edge_bswap32(uint32_t v) {
    return ((v & 0x000000FFu) << 24) |
           ((v & 0x0000FF00u) << 8) |
           ((v & 0x00FF0000u) >> 8) |
           ((v & 0xFF000000u) >> 24);
}

static int fdset_test(const uint8_t *set, int fd) {
    if (!set || fd < 0 || fd >= EDGE_SELECT_FD_MAX) return 0;
    return (set[fd >> 3] & (uint8_t)(1u << (fd & 7))) != 0;
}

static void fdset_set(uint8_t *set, int fd) {
    if (!set || fd < 0 || fd >= EDGE_SELECT_FD_MAX) return;
    set[fd >> 3] |= (uint8_t)(1u << (fd & 7));
}

static uint16_t edge_bswap16(uint16_t v) {
    return (uint16_t)(((v & 0x00FFu) << 8) | ((v & 0xFF00u) >> 8));
}

static uint16_t edge_cksum16(const void *data, uint32_t len) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t acc = 0;
    for (uint32_t i = 0; i + 1 < len; i += 2) acc += ((uint32_t)p[i] << 8) | p[i + 1];
    if (len & 1u) acc += (uint32_t)p[len - 1] << 8;
    while (acc >> 16) acc = (acc & 0xFFFFu) + (acc >> 16);
    return (uint16_t)~acc;
}

static void edge_ip6_to_bytes(const ip6_addr_t *a, uint8_t out[16]) {
    if (!a || !out) return;
    for (int i = 0; i < 4; ++i) {
        uint32_t w = lwip_htonl(a->addr[i]);
        out[i * 4 + 0] = (uint8_t)((w >> 24) & 0xFFu);
        out[i * 4 + 1] = (uint8_t)((w >> 16) & 0xFFu);
        out[i * 4 + 2] = (uint8_t)((w >> 8) & 0xFFu);
        out[i * 4 + 3] = (uint8_t)(w & 0xFFu);
    }
}

static void net_init_defaults(void) {
    memset(&g_if_lo, 0, sizeof(g_if_lo));
    strcpy(g_if_lo.name, "lo");
    g_if_lo.up = 1;
    g_if_lo.ifindex = 1;
    g_if_lo.flags = LINUX_IFF_UP | LINUX_IFF_RUNNING | LINUX_IFF_LOOPBACK;
    g_if_lo.mtu = 65536;
    g_if_lo.ipv4_addr_be = edge_bswap32(0x7F000001u);
    g_if_lo.ipv4_netmask_be = edge_bswap32(0xFF000000u);
    g_if_lo.ipv4_bcast_be = edge_bswap32(0x7FFFFFFFu);
    g_if_lo.ipv4_dst_be = g_if_lo.ipv4_addr_be;

    memset(&g_if_eth0, 0, sizeof(g_if_eth0));
    strcpy(g_if_eth0.name, "eth0");
    g_if_eth0.up = 1;
    g_if_eth0.ifindex = 2;
    g_if_eth0.flags = LINUX_IFF_UP | LINUX_IFF_RUNNING | LINUX_IFF_BROADCAST | LINUX_IFF_MULTICAST;
    g_if_eth0.mtu = 1500;
    g_if_eth0.mac[0] = 0x52;
    g_if_eth0.mac[1] = 0x54;
    g_if_eth0.mac[2] = 0x00;
    g_if_eth0.mac[3] = 0x12;
    g_if_eth0.mac[4] = 0x34;
    g_if_eth0.mac[5] = 0x56;
    g_if_eth0.ipv4_addr_be = edge_bswap32(0x0A00020Fu);     /* 10.0.2.15 */
    g_if_eth0.ipv4_netmask_be = edge_bswap32(0xFFFFFF00u);  /* /24 */
    g_if_eth0.ipv4_bcast_be = edge_bswap32(0x0A0002FFu);    /* 10.0.2.255 */
    g_if_eth0.ipv4_dst_be = edge_bswap32(0x0A000202u);      /* 10.0.2.2 */
}

static edge_netif_t *netif_by_name(const char *name) {
    if (!name || !name[0]) return &g_if_eth0;
    if (strcmp(name, g_if_lo.name) == 0) return &g_if_lo;
    if (strcmp(name, g_if_eth0.name) == 0) return &g_if_eth0;
    return 0;
}

static edge_netif_t *netif_by_index(int ifindex) {
    if (ifindex == g_if_lo.ifindex) return &g_if_lo;
    if (ifindex == g_if_eth0.ifindex) return &g_if_eth0;
    return 0;
}

static int socket_alloc(void) {
    for (int i = 0; i < EDGE_MAX_SOCKETS; ++i) {
        if (!g_sockets[i].used) {
            memset(&g_sockets[i], 0, sizeof(g_sockets[i]));
            g_sockets[i].used = 1;
            g_sockets[i].refs = 1;
            return i;
        }
    }
    return -1;
}

static uint16_t socket_alloc_ephemeral_port_be(void) {
    uint16_t p = g_next_ephemeral_port;
    g_next_ephemeral_port++;
    if (g_next_ephemeral_port < 49152) {
        g_next_ephemeral_port = 49152;
    }
    return edge_bswap16(p);
}

static void socket_set_bind_inet(edge_socket_t *s, uint32_t addr_be, uint16_t port_be) {
    struct edge_sockaddr_in sin;
    if (!s) return;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = LINUX_AF_INET;
    sin.sin_port = port_be;
    sin.sin_addr = addr_be;
    memcpy(s->bind_addr, &sin, sizeof(sin));
    s->bind_len = sizeof(sin);
}

static void socket_set_bind_inet6(edge_socket_t *s, const uint8_t addr[16], uint16_t port_be, uint32_t scope_id) {
    struct edge_sockaddr_in6 sin6;
    if (!s || !addr) return;
    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = LINUX_AF_INET6;
    sin6.sin6_port = port_be;
    sin6.sin6_scope_id = scope_id;
    memcpy(sin6.sin6_addr, addr, 16);
    memcpy(s->bind_addr, &sin6, sizeof(sin6));
    s->bind_len = sizeof(sin6);
}

static void socket_autobind_inet(edge_socket_t *s) {
    if (!s) return;
    if (s->bind_len >= sizeof(struct edge_sockaddr_in)) return;
    socket_set_bind_inet(s, g_if_eth0.ipv4_addr_be, socket_alloc_ephemeral_port_be());
    if (s->domain == LINUX_AF_INET &&
        s->type == LINUX_SOCK_DGRAM &&
        s->lwip_pcb && s->local_port_be == 0) {
        struct udp_pcb *up = (struct udp_pcb *)s->lwip_pcb;
        ip_addr_t any;
        ip_addr_set_zero_ip4(&any);
        if (udp_bind(up, &any, edge_bswap16(((struct edge_sockaddr_in *)s->bind_addr)->sin_port)) == ERR_OK) {
            s->local_port_be = ((struct edge_sockaddr_in *)s->bind_addr)->sin_port;
        }
    }
}

static void socket_autobind_inet6(edge_socket_t *s) {
    uint8_t any6[16];
    if (!s) return;
    if (s->bind_len >= sizeof(struct edge_sockaddr_in6)) return;
    memset(any6, 0, sizeof(any6));
    socket_set_bind_inet6(s, any6, socket_alloc_ephemeral_port_be(), 0);
    if (s->domain == LINUX_AF_INET6 &&
        s->type == LINUX_SOCK_DGRAM &&
        s->lwip_pcb && s->local_port_be == 0) {
        struct udp_pcb *up = (struct udp_pcb *)s->lwip_pcb;
        ip_addr_t any;
        ip_addr_set_zero_ip6(&any);
        if (udp_bind(up, &any, edge_bswap16(((struct edge_sockaddr_in6 *)s->bind_addr)->sin6_port)) == ERR_OK) {
            s->local_port_be = ((struct edge_sockaddr_in6 *)s->bind_addr)->sin6_port;
        }
    }
}

static int sockaddr_in_from_user(uint64_t addr_u, uint32_t len, struct edge_sockaddr_in *sin) {
    if (!sin || !addr_u || len < sizeof(*sin)) return -1;
    if (copy_from_user(sin, addr_u, sizeof(*sin)) < 0) return -1;
    if (sin->sin_family != LINUX_AF_INET) return -1;
    return 0;
}

static int sockaddr_in_from_buf(const uint8_t *buf, uint32_t len, struct edge_sockaddr_in *sin) {
    if (!sin || !buf || len < sizeof(*sin)) return -1;
    memcpy(sin, buf, sizeof(*sin));
    if (sin->sin_family != LINUX_AF_INET) return -1;
    return 0;
}

static int sockaddr_in6_from_user(uint64_t addr_u, uint32_t len, struct edge_sockaddr_in6 *sin6) {
    if (!sin6 || !addr_u || len < sizeof(*sin6)) return -1;
    if (copy_from_user(sin6, addr_u, sizeof(*sin6)) < 0) return -1;
    if (sin6->sin6_family != LINUX_AF_INET6) return -1;
    return 0;
}

static int sockaddr_in6_from_buf(const uint8_t *buf, uint32_t len, struct edge_sockaddr_in6 *sin6) {
    if (!sin6 || !buf || len < sizeof(*sin6)) return -1;
    memcpy(sin6, buf, sizeof(*sin6));
    if (sin6->sin6_family != LINUX_AF_INET6) return -1;
    return 0;
}

static void sockaddr_in_to_user_peer(edge_socket_t *s, uint32_t src_ip_be, uint16_t src_port_be) {
    struct edge_sockaddr_in sin;
    if (!s) return;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = LINUX_AF_INET;
    sin.sin_addr = src_ip_be;
    sin.sin_port = src_port_be;
    memcpy(s->rx_peer, &sin, sizeof(sin));
    s->rx_peer_len = sizeof(sin);
}

static void sockaddr_in6_to_user_peer(edge_socket_t *s, const uint8_t src_ip6[16], uint16_t src_port_be, uint32_t scope_id) {
    struct edge_sockaddr_in6 sin6;
    if (!s || !src_ip6) return;
    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = LINUX_AF_INET6;
    sin6.sin6_port = src_port_be;
    sin6.sin6_scope_id = scope_id;
    memcpy(sin6.sin6_addr, src_ip6, 16);
    memcpy(s->rx_peer, &sin6, sizeof(sin6));
    s->rx_peer_len = sizeof(sin6);
}

static void edge_udp_recv_cb(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port) {
    edge_socket_t *s = (edge_socket_t *)arg;
    uint16_t n;
    (void)pcb;
    if (!s || !p || !addr) {
        if (p) pbuf_free(p);
        return;
    }
    if (s->rx_len > 0) {
        pbuf_free(p);
        return;
    }
    n = (p->tot_len > sizeof(s->rx_buf)) ? (uint16_t)sizeof(s->rx_buf) : (uint16_t)p->tot_len;
    if (pbuf_copy_partial(p, s->rx_buf, n, 0) != n) {
        pbuf_free(p);
        return;
    }
    s->rx_len = n;
    if (IP_IS_V4(addr)) {
        sockaddr_in_to_user_peer(s, ip4_addr_get_u32(ip_2_ip4(addr)), edge_bswap16(port));
    } else if (IP_IS_V6(addr)) {
        uint8_t src6[16];
        edge_ip6_to_bytes(ip_2_ip6(addr), src6);
        sockaddr_in6_to_user_peer(s, src6, edge_bswap16(port), 0);
    }
    pbuf_free(p);
}

static err_t edge_tcp_recv_cb(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) {
    edge_socket_t *s = (edge_socket_t *)arg;
    if (!s) return ERR_OK;
    if (!p) {
        s->closed = 1;
        return ERR_OK;
    }
    if (err != ERR_OK) {
        pbuf_free(p);
        return err;
    }
    if (s->rx_len < sizeof(s->rx_buf)) {
        uint16_t room = (uint16_t)(sizeof(s->rx_buf) - s->rx_len);
        uint16_t n = (p->tot_len > room) ? room : (uint16_t)p->tot_len;
        if (n > 0 && pbuf_copy_partial(p, s->rx_buf + s->rx_len, n, 0) == n) s->rx_len += n;
    }
    tcp_recved(tpcb, p->tot_len);
    pbuf_free(p);
    return ERR_OK;
}

static void edge_tcp_err_cb(void *arg, err_t err) {
    edge_socket_t *s = (edge_socket_t *)arg;
    if (!s) return;
    s->connect_error = (err == ERR_OK) ? 0 : (int)err;
    s->connect_in_progress = 0;
    s->closed = 1;
    s->lwip_pcb = 0;
}

static err_t edge_tcp_connected_cb(void *arg, struct tcp_pcb *tpcb, err_t err) {
    edge_socket_t *s = (edge_socket_t *)arg;
    (void)tpcb;
    if (!s) return ERR_OK;
    s->connect_in_progress = 0;
    s->connect_error = (err == ERR_OK) ? 0 : (int)err;
    if (err == ERR_OK) s->connected = 1;
    return ERR_OK;
}

static int socket_pending_enqueue(edge_socket_t *listener, int sock_id) {
    if (!listener) return -1;
    if (listener->pending_count >= EDGE_SOCK_ACCEPTQ) return -1;
    listener->pending_sock_ids[listener->pending_tail] = sock_id;
    listener->pending_tail = (listener->pending_tail + 1) % EDGE_SOCK_ACCEPTQ;
    listener->pending_count++;
    return 0;
}

static int socket_pending_dequeue(edge_socket_t *listener) {
    int sock_id;
    if (!listener || listener->pending_count <= 0) return -1;
    sock_id = listener->pending_sock_ids[listener->pending_head];
    listener->pending_head = (listener->pending_head + 1) % EDGE_SOCK_ACCEPTQ;
    listener->pending_count--;
    return sock_id;
}

static err_t edge_tcp_accept_cb(void *arg, struct tcp_pcb *newpcb, err_t err) {
    edge_socket_t *listener = (edge_socket_t *)arg;
    int sid;
    edge_socket_t *child;

    if (!listener || !newpcb || err != ERR_OK) {
        if (newpcb) tcp_abort(newpcb);
        return ERR_ABRT;
    }
    if (!listener->listening) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    sid = socket_alloc();
    if (sid < 0) {
        tcp_abort(newpcb);
        return ERR_ABRT;
    }

    child = &g_sockets[sid];
    child->domain = listener->domain;
    child->type = listener->type;
    child->protocol = listener->protocol;
    child->ip_ttl = listener->ip_ttl;
    child->connected = 1;
    child->lwip_pcb = newpcb;
    child->local_port_be = edge_bswap16(newpcb->local_port);

    if (child->domain == LINUX_AF_INET) {
        uint32_t lip = ip4_addr_get_u32(ip_2_ip4(&newpcb->local_ip));
        uint32_t rip = ip4_addr_get_u32(ip_2_ip4(&newpcb->remote_ip));
        socket_set_bind_inet(child, lip, edge_bswap16(newpcb->local_port));
        sockaddr_in_to_user_peer(child, rip, edge_bswap16(newpcb->remote_port));
        memcpy(child->peer_addr, child->rx_peer, child->rx_peer_len);
        child->peer_len = child->rx_peer_len;
        socket_set_bind_inet(listener, lip, edge_bswap16(newpcb->local_port));
        listener->local_port_be = edge_bswap16(newpcb->local_port);
    } else if (child->domain == LINUX_AF_INET6) {
        uint8_t lip6[16];
        uint8_t rip6[16];
        edge_ip6_to_bytes(ip_2_ip6(&newpcb->local_ip), lip6);
        edge_ip6_to_bytes(ip_2_ip6(&newpcb->remote_ip), rip6);
        socket_set_bind_inet6(child, lip6, edge_bswap16(newpcb->local_port), 0);
        sockaddr_in6_to_user_peer(child, rip6, edge_bswap16(newpcb->remote_port), 0);
        memcpy(child->peer_addr, child->rx_peer, child->rx_peer_len);
        child->peer_len = child->rx_peer_len;
        socket_set_bind_inet6(listener, lip6, edge_bswap16(newpcb->local_port), 0);
        listener->local_port_be = edge_bswap16(newpcb->local_port);
    }

    tcp_arg(newpcb, child);
    tcp_recv(newpcb, edge_tcp_recv_cb);
    tcp_err(newpcb, edge_tcp_err_cb);

    if (socket_pending_enqueue(listener, sid) < 0) {
        socket_drop_ref(sid);
        return ERR_ABRT;
    }
    return ERR_OK;
}

static void socket_add_ref(int sock_id) {
    if (sock_id < 0 || sock_id >= EDGE_MAX_SOCKETS) return;
    if (!g_sockets[sock_id].used) return;
    g_sockets[sock_id].refs++;
}

static void socket_drop_ref(int sock_id) {
    if (sock_id < 0 || sock_id >= EDGE_MAX_SOCKETS) return;
    if (!g_sockets[sock_id].used) return;
    if (g_sockets[sock_id].refs > 1) {
        g_sockets[sock_id].refs--;
        return;
    }
    if (g_sockets[sock_id].listening) {
        while (g_sockets[sock_id].pending_count > 0) {
            int psid = socket_pending_dequeue(&g_sockets[sock_id]);
            if (psid >= 0) socket_drop_ref(psid);
        }
    }
    if (g_sockets[sock_id].domain == LINUX_AF_UNIX &&
        g_sockets[sock_id].unix_peer_id >= 0 &&
        g_sockets[sock_id].unix_peer_id < EDGE_MAX_SOCKETS &&
        g_sockets[g_sockets[sock_id].unix_peer_id].used) {
        g_sockets[g_sockets[sock_id].unix_peer_id].closed = 1;
        g_sockets[g_sockets[sock_id].unix_peer_id].unix_peer_id = -1;
    }
    if (g_sockets[sock_id].lwip_pcb) {
        if ((g_sockets[sock_id].domain == LINUX_AF_INET || g_sockets[sock_id].domain == LINUX_AF_INET6) &&
            g_sockets[sock_id].type == LINUX_SOCK_DGRAM) {
            udp_remove((struct udp_pcb *)g_sockets[sock_id].lwip_pcb);
        } else if ((g_sockets[sock_id].domain == LINUX_AF_INET || g_sockets[sock_id].domain == LINUX_AF_INET6) &&
                   g_sockets[sock_id].type == LINUX_SOCK_STREAM) {
            struct tcp_pcb *tp = (struct tcp_pcb *)g_sockets[sock_id].lwip_pcb;
            tcp_arg(tp, 0);
            if (g_sockets[sock_id].listening) {
                tcp_accept(tp, 0);
            } else {
                tcp_recv(tp, 0);
                tcp_err(tp, 0);
            }
            if (tcp_close(tp) != ERR_OK) tcp_abort(tp);
        }
    }
    memset(&g_sockets[sock_id], 0, sizeof(g_sockets[sock_id]));
}

static int mode_to_dtype(uint16_t mode) {
    uint16_t mt = (uint16_t)(mode & 0xF000u);
    if (mt == VFS_INODE_DIR) return LINUX_DT_DIR;
    if (mt == VFS_INODE_FILE) return LINUX_DT_REG;
    if (mt == VFS_INODE_CHR) return LINUX_DT_CHR;
    if (mt == VFS_INODE_BLK) return LINUX_DT_BLK;
    return LINUX_DT_UNKNOWN;
}

static int build_at_path(int dirfd, const char *path_in, char *out, int out_sz) {
    if (!path_in || !out || out_sz <= 1) return -1;
    if (path_in[0] == '/') {
        strncpy(out, path_in, (uint32_t)(out_sz - 1));
        out[out_sz - 1] = 0;
        return 0;
    }

    const char *base = 0;
    if (dirfd == LINUX_AT_FDCWD) {
        base = vfs_getcwd();
        if (!base || !base[0]) base = "/";
    } else {
        edge_fd_proc_t *p = fd_proc_with_stdio();
        edge_fd_t *e = fd_get(p, dirfd);
        if (!e || e->kind != FD_VFS) return -1;
        if ((e->inode.mode & 0xF000) != VFS_INODE_DIR) return -1;
        if (!e->path[0] || e->path[0] != '/') return -1;
        base = e->path;
    }

    int bi = 0;
    if (base[0] != '/') return -1;
    out[bi++] = '/';
    if (!(base[0] == '/' && base[1] == 0)) {
        for (int i = 1; base[i] && bi < out_sz - 1; ++i) out[bi++] = base[i];
    }
    if (bi > 1 && out[bi - 1] != '/' && bi < out_sz - 1) out[bi++] = '/';
    for (int i = 0; path_in[i] && bi < out_sz - 1; ++i) out[bi++] = path_in[i];
    out[bi] = 0;
    if (path_in[0] && out[bi - 1] == '/' && bi > 1) out[bi - 1] = 0;
    return 0;
}

static void fill_kstat(const vfs_inode_t *ino, struct edge_linux_stat *st) {
    memset(st, 0, sizeof(*st));
    if (!ino) return;
    st->st_dev = 1;
    st->st_ino = ino->ino;
    st->st_nlink = 1;
    st->st_mode = ino->mode;
    st->st_uid = ino->uid;
    st->st_gid = ino->gid;
    st->st_rdev = 0;
    st->st_size = (int64_t)ino->size;
    st->st_blksize = 4096;
    st->st_blocks = ((int64_t)ino->size + 511) / 512;
}

static void fill_kstat_mode_size(uint16_t mode, uint32_t size, struct edge_linux_stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_dev = 1;
    st->st_ino = 0;
    st->st_nlink = 1;
    st->st_mode = mode;
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_rdev = 0;
    st->st_size = (int64_t)size;
    st->st_blksize = 4096;
    st->st_blocks = ((int64_t)size + 511) / 512;
}

static task_t *task_by_pid_mutable_local(int pid) {
    if (pid <= 0) return process_current_task();
    for (int i = 0; i < PROC_MAX_TASKS; ++i) {
        const task_t *t = process_task_by_index(i);
        if (!t || t->state == TASK_UNUSED) continue;
        if (t->pid == pid) return (task_t *)(uintptr_t)t;
    }
    return 0;
}

static int truncate_vfs_path_to_len(const char *path, uint64_t new_len) {
    static uint8_t tmp[131072];
    int n;
    if (!path || !path[0]) return -EINVAL;
    if (new_len > sizeof(tmp)) return -EFBIG;
    n = vfs_read_file(path, tmp, sizeof(tmp));
    if (n < 0) return -EIO;
    if ((uint64_t)n > sizeof(tmp)) return -EIO;
    if ((uint64_t)n < new_len) memset(tmp + n, 0, (size_t)(new_len - (uint64_t)n));
    if (vfs_write_file(path, tmp, (uint32_t)new_len) < 0) return -EIO;
    return 0;
}

static edge_fd_proc_t *fd_proc_for_pid(int pid, int create) {
    edge_fd_proc_t *slot = 0;
    for (int i = 0; i < EDGE_MAX_FD_PROCS; ++i) {
        if (g_fd_procs[i].pid == pid) return &g_fd_procs[i];
        if (create && g_fd_procs[i].pid == 0 && !slot) slot = &g_fd_procs[i];
    }
    if (!create || !slot) return 0;
    memset(slot, 0, sizeof(*slot));
    slot->pid = pid;
    fd_ensure_stdio(slot);
    return slot;
}

static uint64_t do_sys_kill(uint64_t pid, uint64_t sig);

static edge_fd_proc_t *fd_proc_with_stdio(void) {
    edge_fd_proc_t *p = fd_proc_for_pid(process_getpid(), 1);
    if (!p) return 0;
    fd_ensure_stdio(p);
    return p;
}

static int file_ref_alloc(void) {
    for (int i = 1; i < EDGE_MAX_FILE_REFS; ++i) {
        if (!g_file_refs[i].used) {
            g_file_refs[i].used = 1;
            g_file_refs[i].refs = 1;
            return i;
        }
    }
    return 0;
}

static int file_ref_get(int id) {
    if (id <= 0 || id >= EDGE_MAX_FILE_REFS) return -1;
    if (!g_file_refs[id].used || g_file_refs[id].refs <= 0) return -1;
    g_file_refs[id].refs++;
    return 0;
}

static int file_ref_put(int id) {
    if (id <= 0 || id >= EDGE_MAX_FILE_REFS) return -1;
    if (!g_file_refs[id].used || g_file_refs[id].refs <= 0) return -1;
    g_file_refs[id].refs--;
    if (g_file_refs[id].refs == 0) {
        g_file_refs[id].used = 0;
    }
    return g_file_refs[id].refs;
}

static void fd_proc_release(int pid) {
    task_t *t = task_by_pid_mutable_local(pid);
    edge_fd_proc_t *p = fd_proc_for_pid(pid, 0);
    if (!p) return;
    {
        int reset_console_tty = 1; /* single global console tty: reset on any process exit */
        int reset_pty_ids[EDGE_MAX_PTYS];
        int reset_pty_count = 0;
        memset(reset_pty_ids, 0, sizeof(reset_pty_ids));

        if (t) {
            if (t->ctty_kind == PROCESS_CTTY_CONSOLE) {
                reset_console_tty = 1;
            } else if (t->ctty_kind == PROCESS_CTTY_PTY) {
                int pty_id = t->ctty_id;
                if (pty_id >= 0 && pty_id < EDGE_MAX_PTYS) reset_pty_ids[pty_id] = 1;
            }
        }

        for (int i = 0; i < EDGE_MAX_FD; ++i) {
            edge_fd_t *e = &p->fds[i];
            if (!e->used) continue;
            if (e->kind == FD_CONSOLE) reset_console_tty = 1;
            if (e->kind == FD_VFS && path_is_console_tty(e->path)) reset_console_tty = 1;
            if ((e->kind == FD_PTY_MASTER || e->kind == FD_PTY_SLAVE) &&
                e->pipe_id >= 0 && e->pipe_id < EDGE_MAX_PTYS) {
                reset_pty_ids[e->pipe_id] = 1;
            }
        }

        if (reset_console_tty) {
            termios_init_sane(&g_tty_termios);
            g_tty_line_len = 0;
            g_tty_line_pos = 0;
            if (t && t->pgid > 0 && g_tty_foreground_pgid == t->pgid) {
                g_tty_foreground_pgid = 0;
            }
        }
        for (int pty_id = 0; pty_id < EDGE_MAX_PTYS; ++pty_id) {
            if (!reset_pty_ids[pty_id]) continue;
            if (!g_ptys[pty_id].used) continue;
            if (t && t->pgid > 0 && g_ptys[pty_id].fg_pgid == t->pgid) {
                g_ptys[pty_id].fg_pgid = 0;
            }
            termios_init_sane(&g_ptys[pty_id].termios);
            reset_pty_count++;
        }
    }
    for (int i = 0; i < EDGE_MAX_FD; ++i) {
        if (!p->fds[i].used) continue;
        if (p->fds[i].file_ref > 0) (void)file_ref_put(p->fds[i].file_ref);
        if (p->fds[i].kind == FD_PIPE_R) pipe_drop_reader(p->fds[i].pipe_id);
        if (p->fds[i].kind == FD_PIPE_W) pipe_drop_writer(p->fds[i].pipe_id);
        if (p->fds[i].kind == FD_SOCKET) socket_drop_ref(p->fds[i].pipe_id);
        if (p->fds[i].kind == FD_PTY_MASTER) pty_drop_ref(p->fds[i].pipe_id, 1);
        if (p->fds[i].kind == FD_PTY_SLAVE) pty_drop_ref(p->fds[i].pipe_id, 0);
        if (p->fds[i].kind == FD_EVENTFD) eventfd_drop_ref(p->fds[i].pipe_id);
        if (p->fds[i].kind == FD_TIMERFD) timerfd_drop_ref(p->fds[i].pipe_id);
        if (p->fds[i].kind == FD_SIGNALFD) signalfd_drop_ref(p->fds[i].pipe_id);
        if (p->fds[i].kind == FD_EPOLL) epoll_drop_ref(p->fds[i].pipe_id);
    }
    memset(p, 0, sizeof(*p));
}

static int fd_proc_has_pty_fd(int pid) {
    edge_fd_proc_t *p = fd_proc_for_pid(pid, 0);
    if (!p) return 0;
    for (int i = 0; i < EDGE_MAX_FD; ++i) {
        edge_fd_t *e = &p->fds[i];
        if (!e->used) continue;
        if (e->kind == FD_PTY_MASTER || e->kind == FD_PTY_SLAVE) return 1;
    }
    return 0;
}


void syscall_release_process_fds(int pid) {
    fd_proc_release(pid);
}

static void fd_ensure_stdio(edge_fd_proc_t *p) {
    int ref;
    if (!p) return;
    if (!p->fds[0].used) {
        ref = file_ref_alloc();
        if (!ref) return;
        p->fds[0].used = 1;
        p->fds[0].kind = FD_CONSOLE;
        p->fds[0].file_ref = ref;
        p->fds[0].flags = 0;
        p->fds[0].fd_flags = 0;
        p->fds[0].pipe_id = -1;
    }
    if (!p->fds[1].used) {
        ref = file_ref_alloc();
        if (!ref) return;
        p->fds[1].used = 1;
        p->fds[1].kind = FD_CONSOLE;
        p->fds[1].file_ref = ref;
        p->fds[1].flags = LINUX_O_WRONLY;
        p->fds[1].fd_flags = 0;
        p->fds[1].pipe_id = -1;
    }
    if (!p->fds[2].used) {
        ref = file_ref_alloc();
        if (!ref) return;
        p->fds[2].used = 1;
        p->fds[2].kind = FD_CONSOLE;
        p->fds[2].file_ref = ref;
        p->fds[2].flags = LINUX_O_WRONLY;
        p->fds[2].fd_flags = 0;
        p->fds[2].pipe_id = -1;
    }
}

static int fd_alloc(edge_fd_proc_t *p, int minfd) {
    if (!p) return -1;
    if (minfd < 0) minfd = 0;
    for (int i = minfd; i < EDGE_MAX_FD; ++i) {
        if (!p->fds[i].used) {
            memset(&p->fds[i], 0, sizeof(p->fds[i]));
            p->fds[i].used = 1;
            return i;
        }
    }
    return -1;
}

static edge_fd_t *fd_get(edge_fd_proc_t *p, int fd) {
    if (!p || fd < 0 || fd >= EDGE_MAX_FD) return 0;
    if (!p->fds[fd].used) return 0;
    return &p->fds[fd];
}

static int pipe_alloc(void) {
    for (int i = 0; i < EDGE_MAX_PIPES; ++i) {
        if (!g_pipes[i].used) {
            memset(&g_pipes[i], 0, sizeof(g_pipes[i]));
            g_pipes[i].used = 1;
            return i;
        }
    }
    return -1;
}

static void pipe_drop_reader(int pipe_id) {
    if (pipe_id < 0 || pipe_id >= EDGE_MAX_PIPES) return;
    edge_pipe_t *pp = &g_pipes[pipe_id];
    if (!pp->used) return;
    if (pp->readers > 0) pp->readers--;
    if (pp->readers == 0 && pp->writers == 0) memset(pp, 0, sizeof(*pp));
}

static void pipe_drop_writer(int pipe_id) {
    if (pipe_id < 0 || pipe_id >= EDGE_MAX_PIPES) return;
    edge_pipe_t *pp = &g_pipes[pipe_id];
    if (!pp->used) return;
    if (pp->writers > 0) pp->writers--;
    if (pp->readers == 0 && pp->writers == 0) memset(pp, 0, sizeof(*pp));
}

static void fd_clone_after_fork(int parent_pid, int child_pid) {
    edge_fd_proc_t *parent = fd_proc_for_pid(parent_pid, 0);
    edge_fd_proc_t *child;
    if (!parent || child_pid <= 0) return;
    child = fd_proc_for_pid(child_pid, 1);
    if (!child) return;
    fd_debug_slot_once("fork-parent-pre", parent_pid, 10, &parent->fds[10]);
    memset(child->fds, 0, sizeof(child->fds));
    /* Clone all descriptors, not just stdio. */
    for (int i = 0; i < EDGE_MAX_FD; ++i) {
        if (!parent->fds[i].used) continue;
        child->fds[i] = parent->fds[i];
        if (child->fds[i].file_ref > 0 && file_ref_get(child->fds[i].file_ref) < 0) {
            memset(&child->fds[i], 0, sizeof(child->fds[i]));
            continue;
        }
        fd_log_lifecycle("fork-clone-parent", parent_pid, i, &parent->fds[i], child_pid);
        fd_log_lifecycle("fork-clone-child", child_pid, i, &child->fds[i], parent_pid);
        if (child->fds[i].kind == FD_PIPE_R &&
            child->fds[i].pipe_id >= 0 &&
            child->fds[i].pipe_id < EDGE_MAX_PIPES) {
            g_pipes[child->fds[i].pipe_id].readers++;
        }
        if (child->fds[i].kind == FD_PIPE_W &&
            child->fds[i].pipe_id >= 0 &&
            child->fds[i].pipe_id < EDGE_MAX_PIPES) {
            g_pipes[child->fds[i].pipe_id].writers++;
        }
        if (child->fds[i].kind == FD_SOCKET &&
            child->fds[i].pipe_id >= 0 &&
            child->fds[i].pipe_id < EDGE_MAX_SOCKETS) {
            socket_add_ref(child->fds[i].pipe_id);
        }
        if (child->fds[i].kind == FD_PTY_MASTER &&
            child->fds[i].pipe_id >= 0 &&
            child->fds[i].pipe_id < EDGE_MAX_PTYS) {
            pty_add_ref(child->fds[i].pipe_id, 1);
        }
        if (child->fds[i].kind == FD_PTY_SLAVE &&
            child->fds[i].pipe_id >= 0 &&
            child->fds[i].pipe_id < EDGE_MAX_PTYS) {
            pty_add_ref(child->fds[i].pipe_id, 0);
        }
        if (child->fds[i].kind == FD_EVENTFD) eventfd_add_ref(child->fds[i].pipe_id);
        if (child->fds[i].kind == FD_TIMERFD) timerfd_add_ref(child->fds[i].pipe_id);
        if (child->fds[i].kind == FD_SIGNALFD) signalfd_add_ref(child->fds[i].pipe_id);
        if (child->fds[i].kind == FD_EPOLL) epoll_add_ref(child->fds[i].pipe_id);
    }
    fd_debug_slot_once("fork-child-post", child_pid, 10, &child->fds[10]);
}

static void tty_reset_defaults(void) {
    termios_init_sane(&g_tty_termios);
    g_tty_line_len = 0;
    g_tty_line_pos = 0;
    g_tty_foreground_pgid = 0;
    g_tty_read_log_count = 0;
    g_tty_fg_fix_log_count = 0;
    g_tty_ioctl_log_count = 0;
}

static int tty_seen_pid(int *arr, int *count, int pid) {
    if (!arr || !count || pid <= 0) return 1;
    for (int i = 0; i < *count; ++i) {
        if (arr[i] == pid) return 1;
    }
    if (*count < 64) arr[(*count)++] = pid;
    return 0;
}

static const char *fd_kind_name(edge_fd_kind_t kind) {
    switch (kind) {
        case FD_CONSOLE: return "console";
        case FD_VFS: return "vfs";
        case FD_PIPE_R: return "pipe_r";
        case FD_PIPE_W: return "pipe_w";
        case FD_SOCKET: return "socket";
        case FD_PTY_MASTER: return "pty_master";
        case FD_PTY_SLAVE: return "pty_slave";
        default: return "none";
    }
}

static int path_is_console_tty(const char *path) {
    if (!path || !path[0]) return 0;
    if (strcmp(path, "/dev/tty") == 0) return 1;
    if (strcmp(path, "/dev/console") == 0) return 1;
    if (strncmp(path, "/dev/tty", 8) == 0) {
        const char *n = path + 8;
        if (!n[0]) return 0;
        for (const char *p = n; *p; ++p) {
            if (*p < '0' || *p > '9') return 0;
        }
        return 1;
    }
    return 0;
}

static int path_is_mouse_input(const char *path) {
    if (!path) return 0;
    return strcmp(path, "/dev/input/mice") == 0 || strcmp(path, "/dev/input/mouse0") == 0;
}

static int pty_alloc(void) {
    for (int i = 0; i < EDGE_MAX_PTYS; ++i) {
        edge_pty_t *pty = &g_ptys[i];
        if (pty->used) continue;
        memset(pty, 0, sizeof(*pty));
        pty->used = 1;
        pty->refs_master = 1;
        pty->refs_slave = 0;
        pty->unlocked = 0;
        pty->fg_pgid = 0;
        termios_init_sane(&pty->termios);
        pty->winsz.ws_row = 25;
        pty->winsz.ws_col = 80;
        return i;
    }
    return -1;
}

static void pty_add_ref(int pty_id, int is_master) {
    if (pty_id < 0 || pty_id >= EDGE_MAX_PTYS) return;
    if (!g_ptys[pty_id].used) return;
    if (is_master) g_ptys[pty_id].refs_master++;
    else g_ptys[pty_id].refs_slave++;
}

static int pty_ring_push(uint8_t *buf, uint32_t *wpos, uint32_t *count, uint8_t c) {
    if (!buf || !wpos || !count) return -1;
    if (*count >= EDGE_PTY_BUF_SIZE) return -1;
    buf[*wpos] = c;
    *wpos = (*wpos + 1) % EDGE_PTY_BUF_SIZE;
    (*count)++;
    return 0;
}

static void pty_echo_to_master(edge_pty_t *pty, uint8_t c) {
    if (!pty) return;
    (void)pty_ring_push(pty->s2m_buf, &pty->s2m_wpos, &pty->s2m_count, c);
}

static void pty_echo_seq_to_master(edge_pty_t *pty, const char *s, int n) {
    if (!pty || !s || n <= 0) return;
    for (int i = 0; i < n; ++i) {
        if (pty_ring_push(pty->s2m_buf, &pty->s2m_wpos, &pty->s2m_count, (uint8_t)s[i]) < 0) break;
    }
}

static int pty_slave_input_have_canonical_line(const edge_pty_t *pty) {
    uint32_t cnt, pos;
    uint8_t eofc;
    if (!pty) return 0;
    cnt = pty->m2s_count;
    pos = pty->m2s_rpos;
    eofc = pty->termios.c_cc[LINUX_VEOF];
    while (cnt > 0) {
        uint8_t c = pty->m2s_buf[pos];
        if (c == '\n' || c == eofc) return 1;
        pos = (pos + 1) % EDGE_PTY_BUF_SIZE;
        cnt--;
    }
    return 0;
}

static uint64_t pty_slave_read_limit(const edge_pty_t *pty, uint64_t req, uint64_t avail) {
    uint32_t cnt, pos;
    uint8_t eofc;
    uint64_t n = 0;
    if (!pty) return 0;
    if ((pty->termios.c_lflag & LINUX_ICANON) == 0) {
        if (req > avail) req = avail;
        return req;
    }
    cnt = pty->m2s_count;
    pos = pty->m2s_rpos;
    eofc = pty->termios.c_cc[LINUX_VEOF];
    if (req > avail) req = avail;
    while (cnt > 0 && n < req) {
        uint8_t c = pty->m2s_buf[pos];
        n++;
        pos = (pos + 1) % EDGE_PTY_BUF_SIZE;
        cnt--;
        if (c == '\n' || c == eofc) break;
    }
    return n;
}

static void pty_drop_ref(int pty_id, int is_master) {
    edge_pty_t *pty;
    if (pty_id < 0 || pty_id >= EDGE_MAX_PTYS) return;
    pty = &g_ptys[pty_id];
    if (!pty->used) return;
    if (is_master) {
        if (pty->refs_master > 0) pty->refs_master--;
    } else {
        if (pty->refs_slave > 0) pty->refs_slave--;
    }
    if (pty->refs_master <= 0 && pty->refs_slave <= 0) {
        memset(pty, 0, sizeof(*pty));
    }
}

static int fd_is_tty(const edge_fd_t *e) {
    if (!e) return 0;
    if (e->kind == FD_CONSOLE) return 1;
    if (e->kind == FD_PTY_MASTER || e->kind == FD_PTY_SLAVE) return 1;
    if (e->kind == FD_VFS && path_is_console_tty(e->path)) return 1;
    return 0;
}

static int tty_ioctl_cmd_is_traced(uint32_t cmd) {
    return (cmd == LINUX_TCGETS ||
            cmd == LINUX_TCSETS || cmd == LINUX_TCSETSW || cmd == LINUX_TCSETSF ||
            cmd == LINUX_TIOCGPGRP || cmd == LINUX_TIOCSPGRP);
}

static int tty_ioctl_cmd_requires_tty(uint32_t cmd) {
    return (cmd == LINUX_TCGETS ||
            cmd == LINUX_TCSETS || cmd == LINUX_TCSETSW || cmd == LINUX_TCSETSF ||
            cmd == LINUX_TIOCGPGRP || cmd == LINUX_TIOCSPGRP ||
            cmd == LINUX_TIOCGWINSZ || cmd == LINUX_TIOCSWINSZ ||
            cmd == LINUX_TIOCSCTTY || cmd == LINUX_TIOCNOTTY);
}

static const char *tty_ioctl_cmd_name(uint32_t cmd) {
    switch (cmd) {
        case LINUX_TCGETS: return "TCGETS";
        case LINUX_TCSETS: return "TCSETS";
        case LINUX_TCSETSW: return "TCSETSW";
        case LINUX_TCSETSF: return "TCSETSF";
        case LINUX_TIOCGPGRP: return "TIOCGPGRP";
        case LINUX_TIOCSPGRP: return "TIOCSPGRP";
        default: return "OTHER";
    }
}

static void tty_log_ioctl_once(task_t *cur, int fd, uint32_t cmd, const edge_fd_t *e, const char *result) {
#if EDGE_TTY_DEBUG
    int pid = cur ? cur->pid : process_getpid();
    const char *name = (cur && cur->name[0]) ? cur->name : "?";
    if (pid <= 0 || !result) return;
    if (tty_seen_pid(g_tty_ioctl_log_pids, &g_tty_ioctl_log_count, pid)) return;
    printf("[tty][ioctl] pid=%d task=%s cmd=%s fd=%d ftype=%s path=%s res=%s\n",
           pid,
           name,
           tty_ioctl_cmd_name(cmd),
           fd,
           e ? fd_kind_name(e->kind) : "none",
           (e && e->path[0]) ? e->path : "-",
           result);
#else
    (void)cur; (void)fd; (void)cmd; (void)e; (void)result;
#endif
}

static int fd_trace_interesting(const edge_fd_t *e, int fd) {
    if (fd >= 0 && fd <= 2) return 1;
    if (!e) return 0;
    if (e->kind == FD_CONSOLE) return 1;
    if (e->kind == FD_PTY_MASTER || e->kind == FD_PTY_SLAVE) return 1;
    if (e->kind == FD_VFS && path_is_console_tty(e->path)) return 1;
    return 0;
}

static void fd_log_lifecycle(const char *ev, int pid, int fd, const edge_fd_t *e, int extra) {
#if EDGE_TTY_DEBUG
    if (!ev || !e) return;
    if (!fd_trace_interesting(e, fd)) return;
    printf("[fd] %s pid=%d fd=%d ftype=%s path=%s extra=%d\n",
           ev, pid, fd, fd_kind_name(e->kind), e->path[0] ? e->path : "-", extra);
#else
    (void)ev; (void)pid; (void)fd; (void)e; (void)extra;
#endif
}

static void fd_debug_slot_once(const char *tag, int pid, int fd, const edge_fd_t *e) {
#if EDGE_FD_FORK_DEBUG
    if (!tag || fd < 0) return;
    if (!e || !e->used) {
        printf("[fd][forkdbg] %s pid=%d fd=%d used=0\n", tag, pid, fd);
        return;
    }
    printf("[fd][forkdbg] %s pid=%d fd=%d used=1 kind=%s ref=%d path=%s\n",
           tag, pid, fd, fd_kind_name(e->kind), e->file_ref, e->path[0] ? e->path : "-");
#else
    (void)tag; (void)pid; (void)fd; (void)e;
#endif
}

static int tty_pgrp_alive(int pgid) {
    if (pgid <= 0) return 0;
    for (int i = 0; i < PROC_MAX_TASKS; ++i) {
        const task_t *t = process_task_by_index(i);
        if (!t || t->state == TASK_UNUSED || t->state == TASK_ZOMBIE) continue;
        if (t->pgid == pgid) return 1;
    }
    return 0;
}

static void tty_log_read_once(task_t *cur, int fg_pgrp) {
#if EDGE_TTY_DEBUG
    if (!cur) return;
    if (tty_seen_pid(g_tty_read_log_pids, &g_tty_read_log_count, cur->pid)) return;
    printf("[tty] read pid=%d pgid=%d sid=%d fg_pgrp=%d st=%d onrq=%d\n",
           cur->pid, cur->pgid, cur->sid, fg_pgrp, (int)cur->state, (int)cur->on_runqueue);
#else
    (void)cur; (void)fg_pgrp;
#endif
}

static void tty_log_fg_fix_once(task_t *cur, int old_fg, int new_fg, const char *why) {
#if EDGE_TTY_DEBUG
    if (!cur || !why) return;
    if (tty_seen_pid(g_tty_fg_fix_log_pids, &g_tty_fg_fix_log_count, cur->pid)) return;
    printf("[tty] fg-fix pid=%d pgid=%d sid=%d fg:%d->%d reason=%s (SIGTTIN bypass compat)\n",
           cur->pid, cur->pgid, cur->sid, old_fg, new_fg, why);
#else
    (void)cur; (void)old_fg; (void)new_fg; (void)why;
#endif
}

static uint64_t tty_interrupt_current_ret(void) {
    /* Emulate EINTR delivery point; caller decides process behavior. */
    return (uint64_t)-EINTR;
}

static int signal_pending_interrupt(void) {
    task_t *t = process_current_task();
    task_timer_poll(t);
    if (!t) return 0;
    if (t->sigint_pending) {
        if (t->sigint_handler == LINUX_SIG_IGN) t->sigint_pending = 0;
        else return 1;
    }
    if (t->sigterm_pending) {
        if (t->sigterm_handler == LINUX_SIG_IGN) t->sigterm_pending = 0;
        else return 1;
    }
    if (t->sigchld_pending) {
        if (t->sigchld_handler == LINUX_SIG_IGN || t->sigchld_handler == LINUX_SIG_DFL) {
            t->sigchld_pending = 0;
        } else {
            return 1;
        }
    }
    if (t->sigalrm_pending) {
        if (t->sigalrm_handler == LINUX_SIG_IGN) {
            t->sigalrm_pending = 0;
            return 0;
        }
        return t->sigalrm_handler != LINUX_SIG_DFL;
    }
    return 0;
}

static uint64_t do_sys_write(uint64_t fd, uint64_t buf, uint64_t len) {
    (void)fd;
    if (!buf) return (uint64_t)-EINVAL;
    if (len == 0) return 0;

    char chunk[128];
    uint64_t done = 0;
    while (done < len) {
        uint64_t n = len - done;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        if (copy_from_user(chunk, buf + done, n) < 0) return (uint64_t)-EFAULT;
        for (uint64_t i = 0; i < n; ++i) {
            char c = chunk[i];
            if (c == '\n' &&
                (g_tty_termios.c_oflag & LINUX_OPOST) != 0 &&
                (g_tty_termios.c_oflag & LINUX_ONLCR) != 0) {
                console_putchar('\r');
            }
            console_putchar(c);
        }
        done += n;
    }
    return len;
}

static int ssh_trace_task(task_t *t) {
#if EDGE_SSH_IO_DEBUG
    if (!t) return 0;
    if (strcmp(t->name, "dropbear") == 0) return 1;
    if (strcmp(t->name, "sh") == 0) return 1;
    if (strcmp(t->name, "busybox") == 0) return 1;
#else
    (void)t;
#endif
    return 0;
}

static int trace_path_interesting(const char *p) {
    if (!p) return 0;
    return strstr(p, "dropbear") || strstr(p, "/bin/sh") || strstr(p, "busybox") ||
           strstr(p, "/bin/ls") || strstr(p, "/usr/bin/ls");
}

static int trace_exec_context(task_t *t, const char *kpath, char **kargv, int argc) {
    (void)t; (void)kargv; (void)argc;
    if (kpath && strstr(kpath, "python3")) return 1;
    return 0;
}

static void dbg_dump_user_bytes_exec(const char *tag, uint64_t va) {
    if (!user_range_ok(va, BB_CRASH_PROBE_LEN)) return;
    printf("%s va=0x%x bytes=", tag, (uint32)va);
    for (int i = 0; i < BB_CRASH_PROBE_LEN; ++i) {
        const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)(va + (uint64_t)i);
        printf("%x", (uint32)(*p));
        if (i + 1 < BB_CRASH_PROBE_LEN) printf(" ");
    }
    printf("\n");
}

static uint64_t do_sys_execve(uint64_t path, uint64_t argv, uint64_t envp) {
    char kpath[256];
    char *kargv[33];
    char *kenvp[33];
    int argc = 0;
    int envc = 0;
    char **uargv = (char **)(uintptr_t)argv;
    char **uenvp = (char **)(uintptr_t)envp;
    task_t *cur = process_current_task();
    vfs_inode_t exec_ino;

    if (!cur) return (uint64_t)-EINVAL;

    if (!path) return (uint64_t)-EINVAL;
    if (!user_range_ok(path, 1)) return (uint64_t)-EFAULT;

    int i = 0;
    for (; i < (int)sizeof(kpath) - 1; ++i) {
        char c = 0;
        if (copy_from_user(&c, path + (uint64_t)i, 1) < 0) return (uint64_t)-EFAULT;
        kpath[i] = c;
        if (!c) break;
    }
    if (i == (int)sizeof(kpath) - 1) return (uint64_t)-EINVAL;

    if (uargv) {
        while (argc < 64) {
            uint64_t p = 0;
            uint64_t slot = argv + (uint64_t)argc * sizeof(uint64_t);
            if (!user_range_ok(slot, sizeof(uint64_t))) return (uint64_t)-EFAULT;
            if (copy_from_user(&p, slot, sizeof(uint64_t)) < 0) return (uint64_t)-EFAULT;
            if (!p) break;
            if (!user_range_ok(p, 1)) return (uint64_t)-EFAULT;
            ++argc;
        }
    }
    if (uenvp) {
        while (envc < 64) {
            uint64_t p = 0;
            uint64_t slot = envp + (uint64_t)envc * sizeof(uint64_t);
            if (!user_range_ok(slot, sizeof(uint64_t))) return (uint64_t)-EFAULT;
            if (copy_from_user(&p, slot, sizeof(uint64_t)) < 0) return (uint64_t)-EFAULT;
            if (!p) break;
            if (!user_range_ok(p, 1)) return (uint64_t)-EFAULT;
            ++envc;
        }
    }
    if (argc > 32) argc = 32;
    if (envc > 32) envc = 32;
    for (int i = 0; i < argc; ++i) {
        uint64_t p = 0;
        uint64_t slot = argv + (uint64_t)i * sizeof(uint64_t);
        if (copy_from_user(&p, slot, sizeof(uint64_t)) < 0) return (uint64_t)-EFAULT;
        if (!p) {
            argc = i;
            break;
        }
        if (copy_user_cstr(cur->start_argbuf[i], (int)sizeof(cur->start_argbuf[i]), p) < 0) return (uint64_t)-EFAULT;
        kargv[i] = cur->start_argbuf[i];
    }
    kargv[argc] = 0;
    for (int i = 0; i < envc; ++i) {
        uint64_t p = 0;
        uint64_t slot = envp + (uint64_t)i * sizeof(uint64_t);
        if (copy_from_user(&p, slot, sizeof(uint64_t)) < 0) return (uint64_t)-EFAULT;
        if (!p) {
            envc = i;
            break;
        }
        if (copy_user_cstr(cur->start_envbuf[i], (int)sizeof(cur->start_envbuf[i]), p) < 0) return (uint64_t)-EFAULT;
        kenvp[i] = cur->start_envbuf[i];
    }
    kenvp[envc] = 0;

    if (trace_exec_context(cur, kpath, kargv, argc)) {
        printf("[exec] pid=%d ppid=%d task=%s path=%s argc=%d envc=%d argv_u=0x%x envp_u=0x%x\n",
               cur->pid, cur->ppid, cur->name, kpath, argc, envc, (uint32)argv, (uint32)envp);
        for (int ai = 0; ai < argc && ai < 4; ++ai) {
            if (!kargv[ai]) break;
            printf("[exec]   argv[%d]=%s\n", ai, kargv[ai]);
        }
        for (int ei = 0; ei < envc && ei < 4; ++ei) {
            if (!kenvp[ei]) break;
            printf("[exec]   envp[%d]=%s\n", ei, kenvp[ei]);
        }
    }

    {
        const char *name_src = (argc > 0 && kargv[0] && kargv[0][0]) ? kargv[0] : kpath;
        const char *base = name_src;
        for (const char *p = name_src; *p; ++p) {
            if (*p == '/') base = p + 1;
        }
        if (!base[0]) base = "proc";
        strncpy(cur->name, base, TASK_NAME_MAX - 1);
        cur->name[TASK_NAME_MAX - 1] = 0;
    }

    /* Preflight before destructive exec reset: execvp() may probe multiple PATH
     * candidates and expects failed execve() to return without corrupting the
     * current image. */
    if (vfs_resolve(kpath, &exec_ino, 0, 0, 0) < 0) {
        return (uint64_t)-ENOENT;
    }
    if ((exec_ino.mode & 0xF000u) == VFS_INODE_DIR) {
        return (uint64_t)-EACCES;
    }
    if (elf_loader_probe(kpath) < 0) {
        return (uint64_t)-ENOEXEC;
    }

    {
        edge_fd_proc_t *p = fd_proc_for_pid(process_getpid(), 0);
        if (p) {
            for (int fd = 0; fd < EDGE_MAX_FD; ++fd) {
                if (!p->fds[fd].used) continue;
            
                if ((p->fds[fd].fd_flags & LINUX_FD_CLOEXEC) != 0) {
                    if (fd >= 0 && fd <= 2) {
                        p->fds[fd].fd_flags &= ~LINUX_FD_CLOEXEC; 
                        continue;
                    }
                    
                    (void)do_sys_close((uint64_t)fd);
                }
            }
        }
    }

    /* Exec replaces the whole userspace image; clear stale heap/stack metadata. */
    if (process_prepare_exec_current() < 0) {
        return (uint64_t)-EINVAL;
    }

    edge_elf_image_t elf_img;
    if (elf_loader_exec(kpath, &elf_img) < 0) {
        /* Image has been reset at this point and cannot be safely resumed. */
        fd_proc_release(process_getpid());
        scheduler_kill_current_and_yield(127);
        return (uint64_t)-EIO;
    }

    if (trace_exec_context(cur, kpath, kargv, argc)) {
        printf("[exec] pid=%d entry=0x%x at_base=0x%x at_entry=0x%x at_phdr=0x%x phnum=%d brk=0x%x\n",
               cur->pid, (uint32)elf_img.entry_rip, (uint32)elf_img.at_base, (uint32)elf_img.at_entry,
               (uint32)elf_img.at_phdr, (int)elf_img.at_phnum, (uint32)cur->user_heap_base);
        /* Keep heavy byte-probe logging disabled in normal debug builds. */
    }

    user_exec_image_t img;
    img.entry = elf_img.entry_rip;
    img.user_stack_top = cur->user_stack_top;
    img.user_heap_base = cur->user_heap_base;
    img.at_phdr = elf_img.at_phdr;
    img.at_phnum = elf_img.at_phnum;
    img.at_entry = elf_img.at_entry;
    img.at_base = elf_img.at_base;
    cur->user_heap_limit = USER_HEAP_BASE_ADDR + USER_HEAP_DEFAULT_DELTA;
    if (exec_ino.size > (4U * 1024U * 1024U)) {
        cur->user_heap_limit += USER_HEAP_PY_EXTRA_DELTA;
    }
    cur->user_mmap_next = cur->user_heap_limit;

    if (elf_img.main_load_hi > cur->user_heap_base &&
        elf_img.main_load_hi < cur->user_heap_limit) {
        uint64_t new_heap = page_align_up(elf_img.main_load_hi + PAGE_SIZE);
        if (new_heap < USER_HEAP_BASE_ADDR || new_heap > cur->user_heap_limit) {
            fd_proc_release(process_getpid());
            scheduler_kill_current_and_yield(127);
            return (uint64_t)-ENOMEM;
        }
        cur->user_heap_base = new_heap;
    }
    cur->user_brk = cur->user_heap_base;

    if (trace_exec_context(cur, kpath, kargv, argc)) {
        printf("[uexec] pid=%d task=%s argc=%d envc=%d entry=0x%x stack_top=0x%x heap=0x%x mmap_next=0x%x\n",
               cur->pid, cur->name, argc, envc, (uint32)img.entry, (uint32)img.user_stack_top,
               (uint32)cur->user_brk, (uint32)cur->user_mmap_next);
    }

    user_exec_run(&img, argc, kargv, envc, kenvp);
    return 0;
}

static uint64_t do_sys_spawn(uint64_t path, uint64_t argv, uint64_t envp) {
    (void)envp;
    char kpath[256];
    int argc = 0;
    char **uargv = (char **)(uintptr_t)argv;

    if (!path) return (uint64_t)-EINVAL;
    if (!user_range_ok(path, 1)) return (uint64_t)-EFAULT;

    int i = 0;
    for (; i < (int)sizeof(kpath) - 1; ++i) {
        char c = 0;
        if (copy_from_user(&c, path + (uint64_t)i, 1) < 0) return (uint64_t)-EFAULT;
        kpath[i] = c;
        if (!c) break;
    }
    if (i == (int)sizeof(kpath) - 1) return (uint64_t)-EINVAL;

    if (uargv) {
        while (argc < 64) {
            uint64_t p = 0;
            uint64_t slot = argv + (uint64_t)argc * sizeof(uint64_t);
            if (!user_range_ok(slot, sizeof(uint64_t))) return (uint64_t)-EFAULT;
            if (copy_from_user(&p, slot, sizeof(uint64_t)) < 0) return (uint64_t)-EFAULT;
            if (!p) break;
            if (!user_range_ok(p, 1)) return (uint64_t)-EFAULT;
            ++argc;
        }
    }

    /* Global pending SIGINT can leak across commands in this simplified model.
     * Drop any stale pending state before creating a new child process. */
    (void)keyboard_take_sigint_pending();
    return (uint64_t)process_spawn_exec(kpath, argc, (char **)uargv);
}

static uint64_t do_sys_read(uint64_t fd, uint64_t buf, uint64_t len) {
    (void)fd;
    task_t *cur = process_current_task();
    int cur_pgid = cur ? cur->pgid : process_getpgid(0);
    if (!buf) return (uint64_t)-EINVAL;
    if (len == 0) return 0;
    if (!user_range_ok(buf, len)) return (uint64_t)-EFAULT;

    if (g_tty_foreground_pgid == 0) {
        int pg = cur_pgid;
        if (pg > 0) g_tty_foreground_pgid = pg;
    }
    tty_log_read_once(cur, g_tty_foreground_pgid);

#if EDGE_TTY_JOBCONTROL_COMPAT
    if (cur && cur_pgid > 0 && g_tty_foreground_pgid != cur_pgid) {
        int old_fg = g_tty_foreground_pgid;
        g_tty_foreground_pgid = cur_pgid;
        tty_log_fg_fix_once(cur, old_fg, g_tty_foreground_pgid, "read-foreground-mismatch");
    }
#endif

    if ((g_tty_termios.c_lflag & LINUX_ICANON) != 0) {
        uint64_t copied = 0;
        while (copied < len) {
            if (g_tty_line_pos < g_tty_line_len) {
                char c = g_tty_linebuf[g_tty_line_pos++];
                if (copy_to_user(buf + copied, &c, 1) < 0) return (uint64_t)-EFAULT;
                copied++;
                if (c == '\n') break;
                continue;
            }

            g_tty_line_pos = 0;
            g_tty_line_len = 0;
            for (;;) {
                int ch = keyboard_getchar();
                if (ch == -1) ch = keyboard_pollchar();
                if (ch == -1 || ch == 0) {
                    wait_blocking_step();
                    continue;
                }

                if (ch == '\r') ch = '\n';

                if ((g_tty_termios.c_lflag & LINUX_ISIG) && ch == 3) {
                    (void)keyboard_take_sigint_pending();
                    if ((g_tty_termios.c_lflag & LINUX_ECHO) != 0) {
                        console_putstr("^C\n");
                    }
                    if (g_tty_foreground_pgid > 0) {
                        (void)do_sys_kill((uint64_t)(int64_t)(-g_tty_foreground_pgid), LINUX_SIGINT);
                    }
                    return (uint64_t)-EINTR;
                }

                if (ch == '\b' || ch == 127) {
                    if (g_tty_line_len > 0) {
                        g_tty_line_len--;
                        if ((g_tty_termios.c_lflag & LINUX_ECHO) != 0) {
                            console_putchar('\b');
                            console_putchar(' ');
                            console_putchar('\b');
                        }
                    }
                    continue;
                }

                if (g_tty_line_len < (int)sizeof(g_tty_linebuf) - 1) {
                    g_tty_linebuf[g_tty_line_len++] = (char)ch;
                    if ((g_tty_termios.c_lflag & LINUX_ECHO) != 0) {
                        console_putchar((char)ch);
                    }
                }
                if (ch == '\n') break;
            }
        }
        return copied;
    }

    {
        uint64_t count = 0;
        while (count < len) {
            int ch = -1;
            for (;;) {
                ch = keyboard_getchar();
                if (ch == -1) ch = keyboard_pollchar();
                if (ch != -1 && ch != 0) break;
                if (count > 0) return count;
                wait_blocking_step();
            }
            if (ch == '\r') ch = '\n';
            if ((g_tty_termios.c_lflag & LINUX_ISIG) && ch == 3) {
                (void)keyboard_take_sigint_pending();
                if (g_tty_foreground_pgid > 0) {
                    (void)do_sys_kill((uint64_t)(int64_t)(-g_tty_foreground_pgid), LINUX_SIGINT);
                }
                return count ? count : (uint64_t)-EINTR;
            }
            if ((g_tty_termios.c_lflag & LINUX_ECHO) != 0) console_putchar((char)ch);
            {
                char out = (char)ch;
                if (copy_to_user(buf + count, &out, 1) < 0) return (uint64_t)-EFAULT;
            }
            count++;
            if (len == 1) break;
        }
        return count;
    }
}

static uint64_t do_sys_getcwd(uint64_t buf, uint64_t size) {
    const char *cwd = vfs_getcwd();
    uint64_t n = (uint64_t)strlen(cwd) + 1;
    if (!buf || size == 0) return (uint64_t)-EINVAL;
    if (size < n) return (uint64_t)-ERANGE;
    if (copy_to_user(buf, cwd, n) < 0) return (uint64_t)-EFAULT;
    return n;
}

static int resolve_user_path(uint64_t path_u, char *path, int path_sz, vfs_inode_t *ino, vfs_superblock_t **sb) {
    if (!path_u || !path || path_sz < 2) return -EINVAL;
    if (copy_user_cstr(path, path_sz, path_u) < 0) return -EFAULT;
    if (vfs_resolve(path, ino, sb, 0, 0) < 0) return -ENOENT;
    return 0;
}

static uint64_t do_sys_chdir(uint64_t path_u) {
    char path[256];
    vfs_inode_t ino;
    int rc = resolve_user_path(path_u, path, sizeof(path), &ino, 0);
    if (rc < 0) return (uint64_t)rc;
    if ((ino.mode & 0xF000u) != VFS_INODE_DIR) return (uint64_t)-ENOTDIR;
    return vfs_chdir(path) == 0 ? 0 : (uint64_t)-EINVAL;
}

static uint64_t do_sys_ls(uint64_t path_u, uint64_t longf) {
    char path[256];
    if (!path_u) {
        vfs_list(0, (int)longf);
        return 0;
    }
    if (copy_user_cstr(path, sizeof(path), path_u) < 0) return (uint64_t)-EFAULT;
    vfs_list(path, (int)longf);
    return 0;
}

static uint64_t do_sys_mkdir(uint64_t path_u) {
    char path[256];
    vfs_inode_t ino;
    if (!path_u) return (uint64_t)-EINVAL;
    if (copy_user_cstr(path, sizeof(path), path_u) < 0) return (uint64_t)-EFAULT;
    if (vfs_resolve(path, &ino, 0, 0, 0) == 0) return (uint64_t)-EEXIST;
    return vfs_mkdir(path) == 0 ? 0 : (uint64_t)-ENOENT;
}

static uint64_t do_sys_rmdir(uint64_t path_u) {
    char path[256];
    vfs_inode_t ino;
    vfs_superblock_t *sb = 0;
    if (!path_u) return (uint64_t)-EINVAL;
    if (copy_user_cstr(path, sizeof(path), path_u) < 0) return (uint64_t)-EFAULT;
    if (strcmp(path, "/") == 0) return (uint64_t)-EBUSY;
    if (vfs_resolve(path, &ino, &sb, 0, 0) < 0) return (uint64_t)-ENOENT;
    if ((ino.mode & 0xF000u) != VFS_INODE_DIR) return (uint64_t)-ENOTDIR;
    if (sb && sb->ops && sb->ops->readdir) {
        char name[VFS_NAME_MAX];
        vfs_inode_t child;
        for (uint32_t i = 0;; ++i) {
            if (sb->ops->readdir(sb, &ino, i, name, &child) < 0) break;
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
            return (uint64_t)-ENOTEMPTY;
        }
    }
    return vfs_unlink(path) == 0 ? 0 : (uint64_t)-EIO;
}

static uint64_t do_sys_touch(uint64_t path_u) {
    char path[256];
    if (!path_u) return (uint64_t)-EINVAL;
    if (copy_user_cstr(path, sizeof(path), path_u) < 0) return (uint64_t)-EFAULT;
    return vfs_touch(path) == 0 ? 0 : (uint64_t)-EINVAL;
}

static uint64_t do_sys_unlink(uint64_t path_u) {
    char path[256];
    vfs_inode_t ino;
    if (!path_u) return (uint64_t)-EINVAL;
    if (copy_user_cstr(path, sizeof(path), path_u) < 0) return (uint64_t)-EFAULT;
    if (vfs_resolve(path, &ino, 0, 0, 0) < 0) return (uint64_t)-ENOENT;
    if ((ino.mode & 0xF000u) == VFS_INODE_DIR) return (uint64_t)-EISDIR;
    return vfs_unlink(path) == 0 ? 0 : (uint64_t)-EIO;
}

static uint64_t do_sys_cat(uint64_t path_u) {
    char path[256];
    static char buf[65536];
    if (!path_u) return (uint64_t)-EINVAL;
    if (copy_user_cstr(path, sizeof(path), path_u) < 0) return (uint64_t)-EFAULT;
    int n = vfs_read_file(path, buf, sizeof(buf));
    if (n < 0) return (uint64_t)-EINVAL;
    for (int i = 0; i < n; ++i) console_putchar(buf[i]);
    if (n == 0 || buf[n - 1] != '\n') console_putchar('\n');
    return 0;
}

static uint64_t do_sys_statfs(uint64_t path_u, uint64_t total_u, uint64_t used_u) {
    char path[256];
    uint32_t total_kb = 0, used_kb = 0;
    const char *p = 0;
    if (path_u) {
        if (copy_user_cstr(path, sizeof(path), path_u) < 0) return (uint64_t)-EFAULT;
        p = path;
    }
    if (vfs_statfs_path(p ? p : "", &total_kb, &used_kb) < 0) return (uint64_t)-EINVAL;
    if (total_u && copy_to_user(total_u, &total_kb, sizeof(total_kb)) < 0) return (uint64_t)-EFAULT;
    if (used_u && copy_to_user(used_u, &used_kb, sizeof(used_kb)) < 0) return (uint64_t)-EFAULT;
    return 0;
}

static uint64_t do_sys_meminfo(uint64_t total_u, uint64_t used_u, uint64_t free_u) {
    uint64_t t = meminfo_total_bytes();
    uint64_t u = meminfo_used_bytes();
    uint64_t f = meminfo_free_bytes();
    if (total_u && copy_to_user(total_u, &t, sizeof(t)) < 0) return (uint64_t)-EFAULT;
    if (used_u && copy_to_user(used_u, &u, sizeof(u)) < 0) return (uint64_t)-EFAULT;
    if (free_u && copy_to_user(free_u, &f, sizeof(f)) < 0) return (uint64_t)-EFAULT;
    return 0;
}

static int statfs_type_from_sb(const vfs_superblock_t *sb) {
    if (!sb) return 0;
    if (strcmp(sb->fs_name, "ext4") == 0) return 0xEF53;
    if (strcmp(sb->fs_name, "ext2") == 0) return 0xEF53;
    if (strcmp(sb->fs_name, "proc") == 0) return 0x9FA0;
    if (strcmp(sb->fs_name, "fat32") == 0) return 0x4d44;
    return 0;
}

static int fill_linux_statfs_from_path(const char *path, struct edge_linux_statfs *st) {
    vfs_inode_t ino;
    vfs_superblock_t *sb = 0;
    uint32_t total_kb = 0;
    uint32_t used_kb = 0;

    if (!path || !st) return -1;
    if (vfs_resolve(path, &ino, &sb, 0, 0) < 0) return -1;
    if (vfs_statfs_path(path, &total_kb, &used_kb) < 0) return -1;

    memset(st, 0, sizeof(*st));
    st->f_type = (int64_t)statfs_type_from_sb(sb);
    st->f_bsize = 1024;
    st->f_blocks = (uint64_t)total_kb;
    st->f_bfree = (total_kb >= used_kb) ? (uint64_t)(total_kb - used_kb) : 0;
    st->f_bavail = st->f_bfree;
    st->f_files = 0;
    st->f_ffree = 0;
    st->f_fsid.val[0] = (int32_t)(ino.ino & 0x7fffffffU);
    st->f_fsid.val[1] = (int32_t)((ino.ino >> 16) & 0x7fffffffU);
    st->f_namelen = VFS_NAME_MAX - 1;
    st->f_frsize = 1024;
    st->f_flags = 0;
    return 0;
}

static uint64_t do_sys_statfs_linux(uint64_t path_u, uint64_t buf_u) {
    char path[256];
    struct edge_linux_statfs st;
    if (!path_u || !buf_u) return (uint64_t)-EINVAL;
    if (copy_user_cstr(path, sizeof(path), path_u) < 0) return (uint64_t)-EFAULT;
    if (fill_linux_statfs_from_path(path, &st) < 0) return (uint64_t)-ENOSYS;
    if (copy_to_user(buf_u, &st, sizeof(st)) < 0) return (uint64_t)-EFAULT;
    return 0;
}

static uint64_t do_sys_fstatfs_linux(uint64_t fd_u, uint64_t buf_u) {
    int fd = (int)fd_u;
    struct edge_linux_statfs st;
    edge_fd_proc_t *p = fd_proc_with_stdio();
    edge_fd_t *e = fd_get(p, fd);
    if (!buf_u) return (uint64_t)-EINVAL;
    if (!e) return (uint64_t)-EBADF;
    if (e->kind != FD_VFS) return (uint64_t)-ENOSYS;
    if (!e->path[0]) return (uint64_t)-ENOSYS;
    if (fill_linux_statfs_from_path(e->path, &st) < 0) return (uint64_t)-ENOSYS;
    if (copy_to_user(buf_u, &st, sizeof(st)) < 0) return (uint64_t)-EFAULT;
    return 0;
}

static uint64_t do_sys_sysinfo(uint64_t info_u) {
    struct edge_linux_sysinfo si;
    uint64_t total = meminfo_total_bytes();
    uint64_t free = meminfo_free_bytes();
    uint64_t up_s = boottime_monotonic_us() / 1000000ull;
    uint16_t procs = 0;

    if (!info_u) return (uint64_t)-EINVAL;
    memset(&si, 0, sizeof(si));

    for (int i = 0; i < PROC_MAX_TASKS; ++i) {
        const task_t *t = process_task_by_index(i);
        if (!t || t->state == TASK_UNUSED || t->state == TASK_ZOMBIE) continue;
        procs++;
    }

    si.uptime = (int64_t)up_s;
    {
        uint32_t running = 0;
        for (int i = 0; i < PROC_MAX_TASKS; ++i) {
            const task_t *t = process_task_by_index(i);
            if (!t || t->state == TASK_UNUSED || t->state == TASK_ZOMBIE) continue;
            if (t->state == TASK_RUNNING || t->state == TASK_RUNNABLE) running++;
        }
        if (running == 0) running = 1;
        si.loads[0] = running << 16;
        si.loads[1] = running << 16;
        si.loads[2] = running << 16;
    }
    si.totalram = total;
    si.freeram = free;
    si.sharedram = 0;
    si.bufferram = 0;
    si.totalswap = 0;
    si.freeswap = 0;
    si.procs = procs;
    si.pad = 0;
    si.totalhigh = 0;
    si.freehigh = 0;
    si.mem_unit = 1;

    if (copy_to_user(info_u, &si, sizeof(si)) < 0) return (uint64_t)-EFAULT;
    return 0;
}

static uint64_t do_sys_mounts(void) {
    vfs_list_mounts();
    return 0;
}

static uint64_t do_sys_mount(uint64_t src_u, uint64_t target_u, uint64_t fs_u) {
    char src[256];
    char target[256];
    char fsname[32];
    vfs_inode_t ino;
    block_device_t *bdev = 0;

    if (!src_u || !target_u) return (uint64_t)-EINVAL;
    if (copy_user_cstr(src, sizeof(src), src_u) < 0) return (uint64_t)-EFAULT;
    if (copy_user_cstr(target, sizeof(target), target_u) < 0) return (uint64_t)-EFAULT;
    if (fs_u) {
        if (copy_user_cstr(fsname, sizeof(fsname), fs_u) < 0) return (uint64_t)-EFAULT;
    } else {
        strcpy(fsname, "ext4");
    }

    if (vfs_resolve(src, &ino, 0, 0, 0) < 0) return (uint64_t)-EINVAL;
    if (vfs_inode_get_block_device(&ino, &bdev) < 0) return (uint64_t)-EINVAL;
    (void)vfs_mkdir(target);
    return vfs_mount_blockdev(bdev, target, fsname) == 0 ? 0 : (uint64_t)-EINVAL;
}

static uint64_t do_sys_linux_mount(uint64_t src_u, uint64_t target_u, uint64_t fstype_u, uint64_t flags_u, uint64_t data_u) {
    char src[256], target[256], fsname[32];
    vfs_inode_t ino;
    block_device_t *bdev = 0;
    (void)flags_u;
    (void)data_u;

    src[0] = 0;
    if (src_u && copy_user_cstr(src, sizeof(src), src_u) < 0) return (uint64_t)-EFAULT;
    if (!target_u) return (uint64_t)-EINVAL;
    if (copy_user_cstr(target, sizeof(target), target_u) < 0) return (uint64_t)-EFAULT;
    if (fstype_u) {
        if (copy_user_cstr(fsname, sizeof(fsname), fstype_u) < 0) return (uint64_t)-EFAULT;
    } else {
        fsname[0] = 0;
    }

    if ((fstype_u && strcmp(fsname, "proc") == 0) || (!fstype_u && strcmp(src, "proc") == 0)) {
        (void)vfs_mkdir(target);
        return vfs_mount("proc", target, "proc") == 0 ? 0 : (uint64_t)-EINVAL;
    }

    if (!src[0]) return (uint64_t)-EINVAL;
    if (fsname[0] == 0) strcpy(fsname, "ext4");

    if (vfs_resolve(src, &ino, 0, 0, 0) < 0) return (uint64_t)-ENOENT;
    if (vfs_inode_get_block_device(&ino, &bdev) < 0) return (uint64_t)-EINVAL;
    (void)vfs_mkdir(target);
    return vfs_mount_blockdev(bdev, target, fsname) == 0 ? 0 : (uint64_t)-EINVAL;
}

static uint64_t do_sys_shutdown(void) {
    __asm__ __volatile__("cli");
    for (;;) __asm__ __volatile__("hlt");
    return 0;
}

static uint64_t do_sys_ps(void) {
    process_list_print();
    return 0;
}

static uint64_t do_sys_kill(uint64_t pid, uint64_t sig) {
    int ipid = (int)pid;
    int isig = (int)sig;
    if (ipid == 0) return (uint64_t)-EINVAL;
    if (isig == 0) {
        if (ipid > 0) return process_get_task(ipid) ? 0 : (uint64_t)-ESRCH;
        for (int i = 0; i < PROC_MAX_TASKS; ++i) {
            const task_t *t = process_task_by_index(i);
            if (!t || t->state == TASK_UNUSED) continue;
            if (t->pgid == -ipid) return 0;
        }
        return (uint64_t)-ESRCH;
    }
    if (isig != LINUX_SIGTERM && isig != LINUX_SIGKILL && isig != LINUX_SIGINT) return (uint64_t)-EINVAL;
    if (ipid > 0) {
        int rc = process_send_signal(ipid, isig);
        return rc == 0 ? 0 : (uint64_t)-ESRCH;
    }
    return process_send_signal_pgid(-ipid, isig) == 0 ? 0 : (uint64_t)-ESRCH;
}

static int socket_try_fill_ping_hw_reply(edge_socket_t *s) {
    uint32_t ip_len;
    uint32_t src_ip_be = 0;
    uint8_t src_ip6[16];
    int rc;

    if (!s) return 0;
    if (s->rx_len > 0) return 1;
    if (!(s->domain == LINUX_AF_INET || s->domain == LINUX_AF_INET6)) return 0;
    if (!(s->type == LINUX_SOCK_RAW || s->type == LINUX_SOCK_DGRAM)) return 0;
    if (s->domain == LINUX_AF_INET && !(s->protocol == 0 || s->protocol == LINUX_IPPROTO_ICMP)) return 0;
    if (s->domain == LINUX_AF_INET6 && !(s->protocol == 0 || s->protocol == LINUX_IPPROTO_ICMPV6)) return 0;

    if (s->domain == LINUX_AF_INET && s->ping_hw) {
        ip_len = sizeof(s->rx_buf);
        rc = lwip_stack_recv_icmp_reply_for_id(s->ping_id_be, s->rx_buf, &ip_len, &src_ip_be);
        if (rc <= 0) return 0;

        if (s->type == LINUX_SOCK_DGRAM && ip_len >= 20) {
            uint32_t ihl = (uint32_t)(s->rx_buf[0] & 0x0Fu) * 4u;
            if (ihl >= 20 && ihl <= ip_len) {
                memmove(s->rx_buf, s->rx_buf + ihl, ip_len - ihl);
                ip_len -= ihl;
            }
        }

        s->rx_len = ip_len;
        {
            struct edge_sockaddr_in sin;
            memset(&sin, 0, sizeof(sin));
            sin.sin_family = LINUX_AF_INET;
            sin.sin_addr = src_ip_be;
            memcpy(s->rx_peer, &sin, sizeof(sin));
            s->rx_peer_len = sizeof(sin);
        }
        return 1;
    }

    if (s->domain == LINUX_AF_INET6 && s->ping_hw) {
        ip_len = sizeof(s->rx_buf);
        rc = lwip_stack_recv_icmpv6_reply_for_id(s->ping_id_be, s->rx_buf, &ip_len, src_ip6);
        if (rc <= 0) return 0;

        s->rx_len = ip_len;
        sockaddr_in6_to_user_peer(s, src_ip6, 0, 0);
        return 1;
    }

    if (s->domain == LINUX_AF_INET) {
        ip_len = sizeof(s->rx_buf);
        rc = lwip_stack_recv_icmp_packet(s->rx_buf, &ip_len, &src_ip_be);
        if (rc <= 0) return 0;

        if (s->type == LINUX_SOCK_DGRAM && ip_len >= 20) {
            uint32_t ihl = (uint32_t)(s->rx_buf[0] & 0x0Fu) * 4u;
            if (ihl >= 20 && ihl <= ip_len) {
                memmove(s->rx_buf, s->rx_buf + ihl, ip_len - ihl);
                ip_len -= ihl;
            }
        }

        s->rx_len = ip_len;
        {
            struct edge_sockaddr_in sin;
            memset(&sin, 0, sizeof(sin));
            sin.sin_family = LINUX_AF_INET;
            sin.sin_addr = src_ip_be;
            memcpy(s->rx_peer, &sin, sizeof(sin));
            s->rx_peer_len = sizeof(sin);
        }
        return 1;
    }

    return 0;
}

static uint64_t timerfd_now_us(int clockid) {
    if (clockid == CLOCK_REALTIME) return boottime_realtime_us();
    return boottime_monotonic_us();
}

static uint64_t timerfd_expire_count(edge_timerfd_t *t, int consume) {
    uint64_t now;
    uint64_t cnt = 0;
    if (!t || !t->used || !t->active) return 0;
    now = timerfd_now_us(t->clockid);
    if (now < t->next_us) return 0;
    cnt = 1;
    if (t->interval_us > 0) {
        if (now > t->next_us) cnt += (now - t->next_us) / t->interval_us;
        if (consume) t->next_us += cnt * t->interval_us;
    } else {
        if (consume) t->active = 0;
    }
    return cnt;
}

static int signalfd_peek_current(edge_signalfd_t *sf, int consume, struct edge_linux_signalfd_siginfo *out) {
    task_t *t = process_current_task();
    int sig = 0;
    if (!sf || !t) return 0;
    if ((sf->mask & (1ull << (LINUX_SIGINT - 1))) && t->sigint_pending) {
        sig = LINUX_SIGINT;
        if (consume) t->sigint_pending = 0;
    } else if ((sf->mask & (1ull << (LINUX_SIGTERM - 1))) && t->sigterm_pending) {
        sig = LINUX_SIGTERM;
        if (consume) t->sigterm_pending = 0;
    } else if ((sf->mask & (1ull << (LINUX_SIGALRM - 1))) && t->sigalrm_pending) {
        sig = LINUX_SIGALRM;
        if (consume) t->sigalrm_pending = 0;
    } else if ((sf->mask & (1ull << (LINUX_SIGCHLD - 1))) && t->sigchld_pending) {
        sig = LINUX_SIGCHLD;
        if (consume) t->sigchld_pending = 0;
    }
    if (!sig) return 0;
    if (out) {
        memset(out, 0, sizeof(*out));
        out->ssi_signo = (uint32_t)sig;
        out->ssi_pid = (uint32_t)process_getpid();
        out->ssi_uid = process_getuid();
    }
    return 1;
}

static int poll_fd_revents(edge_fd_t *e, int16_t events) {
    int16_t rev = 0;
    if (!e) return LINUX_POLLNVAL;

    if (events & (LINUX_POLLIN | LINUX_POLLPRI)) {
        if (e->kind == FD_CONSOLE) {
            if (keyboard_haschar()) rev |= LINUX_POLLIN;
        } else if (e->kind == FD_PTY_MASTER || e->kind == FD_PTY_SLAVE) {
            if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_PTYS || !g_ptys[e->pipe_id].used) {
                rev |= LINUX_POLLNVAL;
            } else {
                edge_pty_t *pty = &g_ptys[e->pipe_id];
                uint32_t cnt = (e->kind == FD_PTY_MASTER) ? pty->s2m_count : pty->m2s_count;
                int peer_refs = (e->kind == FD_PTY_MASTER) ? pty->refs_slave : pty->refs_master;
                if (cnt > 0) rev |= LINUX_POLLIN;
                if (peer_refs <= 0) rev |= LINUX_POLLHUP;
            }
        } else if (e->kind == FD_PIPE_R) {
            if (e->pipe_id >= 0 && e->pipe_id < EDGE_MAX_PIPES) {
                edge_pipe_t *pp = &g_pipes[e->pipe_id];
                if (!pp->used) rev |= LINUX_POLLNVAL;
                else {
                    if (pp->count > 0) rev |= LINUX_POLLIN;
                    if (pp->writers == 0) rev |= LINUX_POLLHUP;
                }
            } else {
                rev |= LINUX_POLLNVAL;
            }
        } else if (e->kind == FD_SOCKET) {
            if (e->pipe_id >= 0 && e->pipe_id < EDGE_MAX_SOCKETS) {
                edge_socket_t *s = &g_sockets[e->pipe_id];
                if (!s->used) rev |= LINUX_POLLNVAL;
                else {
                    if (s->listening && s->pending_count > 0) rev |= LINUX_POLLIN;
                    (void)socket_try_fill_ping_hw_reply(s);
                    if (s->rx_len > 0 || s->closed) rev |= LINUX_POLLIN;
                    if (s->closed) rev |= LINUX_POLLHUP;
                }
            } else {
                rev |= LINUX_POLLNVAL;
            }
        } else if (e->kind == FD_EVENTFD) {
            if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_EVENTFDS || !g_eventfds[e->pipe_id].used) rev |= LINUX_POLLNVAL;
            else if (g_eventfds[e->pipe_id].counter > 0) rev |= LINUX_POLLIN;
        } else if (e->kind == FD_TIMERFD) {
            if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_TIMERFDS || !g_timerfds[e->pipe_id].used) rev |= LINUX_POLLNVAL;
            else if (timerfd_expire_count(&g_timerfds[e->pipe_id], 0) > 0) rev |= LINUX_POLLIN;
        } else if (e->kind == FD_SIGNALFD) {
            struct edge_linux_signalfd_siginfo si;
            if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_SIGNALFDS || !g_signalfds[e->pipe_id].used) rev |= LINUX_POLLNVAL;
            else if (signalfd_peek_current(&g_signalfds[e->pipe_id], 0, &si)) rev |= LINUX_POLLIN;
        } else if (e->kind == FD_EPOLL) {
            if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_EPOLLS || !g_epolls[e->pipe_id].used) rev |= LINUX_POLLNVAL;
            else {
                edge_epoll_t *ep = &g_epolls[e->pipe_id];
                edge_fd_proc_t *p = fd_proc_with_stdio();
                for (int i = 0; i < ep->nwatch; ++i) {
                    edge_fd_t *we = fd_get(p, ep->watch[i].fd);
                    if (we && (poll_fd_revents(we, (int16_t)(ep->watch[i].events & 0x7fff)) & (LINUX_POLLIN | LINUX_POLLPRI | LINUX_POLLOUT | LINUX_POLLERR | LINUX_POLLHUP))) {
                        rev |= LINUX_POLLIN;
                        break;
                    }
                }
            }
        } else if (e->kind == FD_PIDFD) {
            const task_t *pt = process_get_task(e->pipe_id);
            if (!pt || pt->state == TASK_ZOMBIE) rev |= LINUX_POLLIN;
        } else {
            if (e->kind == FD_VFS && path_is_console_tty(e->path)) {
                if (keyboard_haschar()) rev |= LINUX_POLLIN;
            } else if (e->kind == FD_VFS && path_is_mouse_input(e->path)) {
                if (keyboard_mouse_pending() > 0) rev |= LINUX_POLLIN;
            } else {
                rev |= LINUX_POLLIN;
            }
        }
    }

    if (events & LINUX_POLLOUT) {
        if (e->kind == FD_PIPE_W) {
            if (e->pipe_id >= 0 && e->pipe_id < EDGE_MAX_PIPES) {
                edge_pipe_t *pp = &g_pipes[e->pipe_id];
                if (!pp->used) rev |= LINUX_POLLNVAL;
                else if (pp->readers == 0) rev |= LINUX_POLLERR;
                else if (pp->count < EDGE_PIPE_SIZE) rev |= LINUX_POLLOUT;
            } else {
                rev |= LINUX_POLLNVAL;
            }
        } else if (e->kind == FD_PTY_MASTER || e->kind == FD_PTY_SLAVE) {
            if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_PTYS || !g_ptys[e->pipe_id].used) {
                rev |= LINUX_POLLNVAL;
            } else {
                edge_pty_t *pty = &g_ptys[e->pipe_id];
                uint32_t cnt = (e->kind == FD_PTY_MASTER) ? pty->m2s_count : pty->s2m_count;
                int peer_refs = (e->kind == FD_PTY_MASTER) ? pty->refs_slave : pty->refs_master;
                if (peer_refs <= 0) rev |= LINUX_POLLERR;
                else if (cnt < EDGE_PTY_BUF_SIZE) rev |= LINUX_POLLOUT;
            }
        } else if (e->kind == FD_EVENTFD) {
            if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_EVENTFDS || !g_eventfds[e->pipe_id].used) rev |= LINUX_POLLNVAL;
            else if (g_eventfds[e->pipe_id].counter < UINT64_MAX - 1) rev |= LINUX_POLLOUT;
        } else {
            if (e->kind == FD_SOCKET && e->pipe_id >= 0 && e->pipe_id < EDGE_MAX_SOCKETS) {
                edge_socket_t *s = &g_sockets[e->pipe_id];
                if (!s->used) rev |= LINUX_POLLNVAL;
                else if (s->domain == LINUX_AF_UNIX && s->type == LINUX_SOCK_STREAM) {
                    if (s->unix_peer_id >= 0 && s->unix_peer_id < EDGE_MAX_SOCKETS) {
                        edge_socket_t *peer = &g_sockets[s->unix_peer_id];
                        if (peer->used && peer->rx_len < sizeof(peer->rx_buf)) rev |= LINUX_POLLOUT;
                    }
                } else if ((s->domain == LINUX_AF_INET || s->domain == LINUX_AF_INET6) &&
                           s->type == LINUX_SOCK_STREAM && s->lwip_pcb && s->connected && !s->closed) {
                    struct tcp_pcb *tp = (struct tcp_pcb *)s->lwip_pcb;
                    if (tcp_sndbuf(tp) > 0) rev |= LINUX_POLLOUT;
                } else {
                    rev |= LINUX_POLLOUT;
                }
            } else {
                rev |= LINUX_POLLOUT;
            }
        }
    }

    return rev;
}

static uint64_t do_sys_poll(uint64_t fds_u, uint64_t nfds_u, uint64_t timeout_u) {
    int nfds = (int)nfds_u;
    int timeout = (int)timeout_u;
    struct edge_pollfd pfds[64];
    uint64_t start_us;

    if (!fds_u) return (uint64_t)-EINVAL;
    if (nfds < 0 || nfds > (int)(sizeof(pfds) / sizeof(pfds[0]))) return (uint64_t)-EINVAL;
    if (nfds == 0) {
        if (timeout > 0) (void)do_sys_sleep((uint64_t)timeout);
        return 0;
    }
    if (copy_from_user(pfds, fds_u, (uint64_t)nfds * sizeof(pfds[0])) < 0) return (uint64_t)-EFAULT;

    start_us = boottime_monotonic_us();
    for (;;) {
        int ready = 0;
        edge_fd_proc_t *p = fd_proc_with_stdio();

        if (signal_pending_interrupt()) return tty_interrupt_current_ret();
        lwip_stack_poll();

        for (int i = 0; i < nfds; ++i) {
            int fd = pfds[i].fd;
            int16_t ev = pfds[i].events;
            int16_t rev = 0;

            if (fd >= 0) {
                edge_fd_t *e = fd_get(p, fd);
                rev = (int16_t)poll_fd_revents(e, ev);
                if (rev != 0) ready++;
            }
            pfds[i].revents = rev;
        }

        if (ready > 0) {
            if (copy_to_user(fds_u, pfds, (uint64_t)nfds * sizeof(pfds[0])) < 0) return (uint64_t)-EFAULT;
            return (uint64_t)ready;
        }

        if (timeout == 0) {
            if (copy_to_user(fds_u, pfds, (uint64_t)nfds * sizeof(pfds[0])) < 0) return (uint64_t)-EFAULT;
            return 0;
        }
        if (timeout > 0) {
            uint64_t now_us = boottime_monotonic_us();
            if (now_us - start_us >= (uint64_t)timeout * 1000ull) {
                if (copy_to_user(fds_u, pfds, (uint64_t)nfds * sizeof(pfds[0])) < 0) return (uint64_t)-EFAULT;
                return 0;
            }
        }
        wait_blocking_step();
    }
}

static uint64_t do_sys_ppoll(uint64_t fds_u, uint64_t nfds_u, uint64_t timeout_u, uint64_t sigmask_u, uint64_t sigsetsize_u) {
    int64_t timeout_ms = -1;
    struct edge_timespec ts;
    (void)sigmask_u;
    (void)sigsetsize_u;
    if (timeout_u) {
        if (copy_from_user(&ts, timeout_u, sizeof(ts)) < 0) return (uint64_t)-EFAULT;
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000LL) return (uint64_t)-EINVAL;
        if (ts.tv_sec > (int64_t)(0x7FFFFFFFLL / 1000LL)) timeout_ms = 0x7FFFFFFFLL;
        else timeout_ms = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
    }
    return do_sys_poll(fds_u, nfds_u, (uint64_t)timeout_ms);
}

static uint64_t do_sys_select(uint64_t n_u, uint64_t rfds_u, uint64_t wfds_u, uint64_t efds_u, uint64_t timeout_u) {
    int n = (int)n_u;
    uint32_t nbytes = (uint32_t)((n + 7) / 8);
    int64_t timeout_ms = -1;
    uint64_t start_us = boottime_monotonic_us();
    struct edge_timeval tv;
    uint8_t rin[EDGE_SELECT_FD_BYTES];
    uint8_t win[EDGE_SELECT_FD_BYTES];
    uint8_t ein[EDGE_SELECT_FD_BYTES];
    uint8_t rout[EDGE_SELECT_FD_BYTES];
    uint8_t wout[EDGE_SELECT_FD_BYTES];
    uint8_t eout[EDGE_SELECT_FD_BYTES];

    if (n < 0 || n > EDGE_SELECT_FD_MAX) return (uint64_t)-EINVAL;
    if (nbytes > EDGE_SELECT_FD_BYTES) return (uint64_t)-EINVAL;
    memset(rin, 0, sizeof(rin));
    memset(win, 0, sizeof(win));
    memset(ein, 0, sizeof(ein));

    if (rfds_u && nbytes && copy_from_user(rin, rfds_u, nbytes) < 0) return (uint64_t)-EFAULT;
    if (wfds_u && nbytes && copy_from_user(win, wfds_u, nbytes) < 0) return (uint64_t)-EFAULT;
    if (efds_u && nbytes && copy_from_user(ein, efds_u, nbytes) < 0) return (uint64_t)-EFAULT;

    if (timeout_u) {
        if (copy_from_user(&tv, timeout_u, sizeof(tv)) < 0) return (uint64_t)-EFAULT;
        if (tv.tv_sec < 0 || tv.tv_usec < 0 || tv.tv_usec >= 1000000LL) return (uint64_t)-EINVAL;
        if (tv.tv_sec > (int64_t)(0x7FFFFFFFLL / 1000LL)) timeout_ms = 0x7FFFFFFFLL;
        else timeout_ms = tv.tv_sec * 1000LL + tv.tv_usec / 1000LL;
    }

    for (;;) {
        int ready = 0;
        edge_fd_proc_t *p = fd_proc_with_stdio();
        memset(rout, 0, sizeof(rout));
        memset(wout, 0, sizeof(wout));
        memset(eout, 0, sizeof(eout));

        if (signal_pending_interrupt()) return tty_interrupt_current_ret();
        lwip_stack_poll();

        for (int fd = 0; fd < n; ++fd) {
            int watched = fdset_test(rin, fd) || fdset_test(win, fd) || fdset_test(ein, fd);
            int events = 0;
            int rev;
            int fd_ready = 0;
            edge_fd_t *e;
            if (!watched) continue;
            e = fd_get(p, fd);
            if (!e) return (uint64_t)-EBADF;
            if (fdset_test(rin, fd)) events |= (LINUX_POLLIN | LINUX_POLLPRI);
            if (fdset_test(win, fd)) events |= LINUX_POLLOUT;
            if (fdset_test(ein, fd)) events |= LINUX_POLLPRI;
            rev = poll_fd_revents(e, (int16_t)events);
            if ((rev & (LINUX_POLLIN | LINUX_POLLHUP | LINUX_POLLERR)) && fdset_test(rin, fd)) {
                fdset_set(rout, fd);
                fd_ready = 1;
            }
            if ((rev & (LINUX_POLLOUT | LINUX_POLLERR)) && fdset_test(win, fd)) {
                fdset_set(wout, fd);
                fd_ready = 1;
            }
            if ((rev & LINUX_POLLPRI) && fdset_test(ein, fd)) {
                fdset_set(eout, fd);
                fd_ready = 1;
            }
            if (fd_ready) ready++;
        }

        if (ready > 0) {
            if (rfds_u && nbytes && copy_to_user(rfds_u, rout, nbytes) < 0) return (uint64_t)-EFAULT;
            if (wfds_u && nbytes && copy_to_user(wfds_u, wout, nbytes) < 0) return (uint64_t)-EFAULT;
            if (efds_u && nbytes && copy_to_user(efds_u, eout, nbytes) < 0) return (uint64_t)-EFAULT;
            return (uint64_t)ready;
        }

        if (timeout_ms == 0) {
            if (rfds_u && nbytes && copy_to_user(rfds_u, rout, nbytes) < 0) return (uint64_t)-EFAULT;
            if (wfds_u && nbytes && copy_to_user(wfds_u, wout, nbytes) < 0) return (uint64_t)-EFAULT;
            if (efds_u && nbytes && copy_to_user(efds_u, eout, nbytes) < 0) return (uint64_t)-EFAULT;
            return 0;
        }
        if (timeout_ms > 0) {
            uint64_t now_us = boottime_monotonic_us();
            if (now_us - start_us >= (uint64_t)timeout_ms * 1000ull) {
                if (rfds_u && nbytes && copy_to_user(rfds_u, rout, nbytes) < 0) return (uint64_t)-EFAULT;
                if (wfds_u && nbytes && copy_to_user(wfds_u, wout, nbytes) < 0) return (uint64_t)-EFAULT;
                if (efds_u && nbytes && copy_to_user(efds_u, eout, nbytes) < 0) return (uint64_t)-EFAULT;
                return 0;
            }
        }
        wait_blocking_step();
    }
}

static uint64_t do_sys_pselect6(uint64_t n_u, uint64_t rfds_u, uint64_t wfds_u, uint64_t efds_u, uint64_t timeout_u, uint64_t sig_u) {
    int n = (int)n_u;
    uint32_t nbytes = (uint32_t)((n + 7) / 8);
    int64_t timeout_ms = -1;
    uint64_t start_us = boottime_monotonic_us();
    struct edge_timespec ts;
    struct edge_linux_pselect_sigset sig;
    uint8_t rin[EDGE_SELECT_FD_BYTES];
    uint8_t win[EDGE_SELECT_FD_BYTES];
    uint8_t ein[EDGE_SELECT_FD_BYTES];
    uint8_t rout[EDGE_SELECT_FD_BYTES];
    uint8_t wout[EDGE_SELECT_FD_BYTES];
    uint8_t eout[EDGE_SELECT_FD_BYTES];

    if (n < 0 || n > EDGE_SELECT_FD_MAX) return (uint64_t)-EINVAL;
    if (nbytes > EDGE_SELECT_FD_BYTES) return (uint64_t)-EINVAL;
    memset(rin, 0, sizeof(rin));
    memset(win, 0, sizeof(win));
    memset(ein, 0, sizeof(ein));

    if (rfds_u && nbytes && copy_from_user(rin, rfds_u, nbytes) < 0) return (uint64_t)-EFAULT;
    if (wfds_u && nbytes && copy_from_user(win, wfds_u, nbytes) < 0) return (uint64_t)-EFAULT;
    if (efds_u && nbytes && copy_from_user(ein, efds_u, nbytes) < 0) return (uint64_t)-EFAULT;

    if (timeout_u) {
        if (copy_from_user(&ts, timeout_u, sizeof(ts)) < 0) return (uint64_t)-EFAULT;
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000LL) return (uint64_t)-EINVAL;
        if (ts.tv_sec > (int64_t)(0x7FFFFFFFLL / 1000LL)) timeout_ms = 0x7FFFFFFFLL;
        else timeout_ms = ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
    }
    if (sig_u) {
        if (copy_from_user(&sig, sig_u, sizeof(sig)) < 0) return (uint64_t)-EFAULT;
        (void)sig;
    }

    for (;;) {
        int ready = 0;
        edge_fd_proc_t *p = fd_proc_with_stdio();
        memset(rout, 0, sizeof(rout));
        memset(wout, 0, sizeof(wout));
        memset(eout, 0, sizeof(eout));

        if (signal_pending_interrupt()) return tty_interrupt_current_ret();
        lwip_stack_poll();

        for (int fd = 0; fd < n; ++fd) {
            int watched = fdset_test(rin, fd) || fdset_test(win, fd) || fdset_test(ein, fd);
            int events = 0;
            int rev;
            int fd_ready = 0;
            edge_fd_t *e;
            if (!watched) continue;
            e = fd_get(p, fd);
            if (!e) return (uint64_t)-EBADF;
            if (fdset_test(rin, fd)) events |= (LINUX_POLLIN | LINUX_POLLPRI);
            if (fdset_test(win, fd)) events |= LINUX_POLLOUT;
            if (fdset_test(ein, fd)) events |= LINUX_POLLPRI;
            rev = poll_fd_revents(e, (int16_t)events);
            if ((rev & (LINUX_POLLIN | LINUX_POLLHUP | LINUX_POLLERR)) && fdset_test(rin, fd)) {
                fdset_set(rout, fd);
                fd_ready = 1;
            }
            if ((rev & (LINUX_POLLOUT | LINUX_POLLERR)) && fdset_test(win, fd)) {
                fdset_set(wout, fd);
                fd_ready = 1;
            }
            if ((rev & LINUX_POLLPRI) && fdset_test(ein, fd)) {
                fdset_set(eout, fd);
                fd_ready = 1;
            }
            if (fd_ready) ready++;
        }

        if (ready > 0) {
            if (rfds_u && nbytes && copy_to_user(rfds_u, rout, nbytes) < 0) return (uint64_t)-EFAULT;
            if (wfds_u && nbytes && copy_to_user(wfds_u, wout, nbytes) < 0) return (uint64_t)-EFAULT;
            if (efds_u && nbytes && copy_to_user(efds_u, eout, nbytes) < 0) return (uint64_t)-EFAULT;
            return (uint64_t)ready;
        }

        if (timeout_ms == 0) {
            if (rfds_u && nbytes && copy_to_user(rfds_u, rout, nbytes) < 0) return (uint64_t)-EFAULT;
            if (wfds_u && nbytes && copy_to_user(wfds_u, wout, nbytes) < 0) return (uint64_t)-EFAULT;
            if (efds_u && nbytes && copy_to_user(efds_u, eout, nbytes) < 0) return (uint64_t)-EFAULT;
            return 0;
        }
        if (timeout_ms > 0) {
            uint64_t now_us = boottime_monotonic_us();
            if (now_us - start_us >= (uint64_t)timeout_ms * 1000ull) {
                if (rfds_u && nbytes && copy_to_user(rfds_u, rout, nbytes) < 0) return (uint64_t)-EFAULT;
                if (wfds_u && nbytes && copy_to_user(wfds_u, wout, nbytes) < 0) return (uint64_t)-EFAULT;
                if (efds_u && nbytes && copy_to_user(efds_u, eout, nbytes) < 0) return (uint64_t)-EFAULT;
                return 0;
            }
        }
        wait_blocking_step();
    }
}

static uint64_t do_sys_sleep(uint64_t ms) {
    uint64_t start_us;
    if (ms == 0) return 0;
    start_us = boottime_monotonic_us();
    for (;;) {
        task_t *cur;
        uint64_t now_us = boottime_monotonic_us();
        if (now_us - start_us >= ms * 1000ull) return 0;
        if (signal_pending_interrupt()) return tty_interrupt_current_ret();
        lwip_stack_poll();
        cur = process_current_task();
        if (cur && cur->state == TASK_ZOMBIE) return (uint64_t)-EINTR;
        wait_blocking_step();
    }
}

static uint64_t do_sys_dmesg(void) {
    const char *p = bootlog_buffer();
    int n = bootlog_buffer_size();
    for (int i = 0; i < n; ++i) console_putchar(p[i]);
    return 0;
}

static uint64_t do_sys_stat(uint64_t path_u) {
    char path[256];
    vfs_inode_t ino;
    if (!path_u) return (uint64_t)-EINVAL;
    if (copy_user_cstr(path, sizeof(path), path_u) < 0) return (uint64_t)-EFAULT;
    if (vfs_resolve(path, &ino, 0, 0, 0) < 0) return (uint64_t)-EINVAL;
    printf("  File: %s\n", path);
    printf("  Size: %d\n", (int)ino.size);
    printf("Inode: %d\n", (int)ino.ino);
    printf(" Mode: %x\n", (int)ino.mode);
    return 0;
}

static uint64_t do_sys_stat_path(uint64_t path_u, uint64_t st_u) {
    char path[256];
    vfs_inode_t ino;
    struct edge_linux_stat st;
    int rc = resolve_user_path(path_u, path, sizeof(path), &ino, 0);
    if (rc < 0) return (uint64_t)rc;
    if (!st_u) return (uint64_t)-EINVAL;
    fill_kstat(&ino, &st);
    if (copy_to_user(st_u, &st, sizeof(st)) < 0) return (uint64_t)-EFAULT;
    return 0;
}

static uint64_t do_sys_lstat_path(uint64_t path_u, uint64_t st_u) {
    return do_sys_stat_path(path_u, st_u);
}

static uint64_t do_sys_mv(uint64_t src_u, uint64_t dst_u) {
    char src[256], dst[256];
    static char tmp[131072];
    int n;
    if (!src_u || !dst_u) return (uint64_t)-EINVAL;
    if (copy_user_cstr(src, sizeof(src), src_u) < 0) return (uint64_t)-EFAULT;
    if (copy_user_cstr(dst, sizeof(dst), dst_u) < 0) return (uint64_t)-EFAULT;
    n = vfs_read_file(src, tmp, sizeof(tmp));
    if (n < 0) return (uint64_t)-EINVAL;
    if (vfs_write_file(dst, tmp, (uint32_t)n) < 0) return (uint64_t)-EINVAL;
    if (vfs_unlink(src) < 0) return (uint64_t)-EINVAL;
    return 0;
}

static uint64_t do_sys_rename(uint64_t old_u, uint64_t new_u) {
    char oldp[256], newp[256];
    static char tmp[131072];
    vfs_inode_t oldino, newino;
    int n;

    if (!old_u || !new_u) return (uint64_t)-EINVAL;
    if (copy_user_cstr(oldp, sizeof(oldp), old_u) < 0) return (uint64_t)-EFAULT;
    if (copy_user_cstr(newp, sizeof(newp), new_u) < 0) return (uint64_t)-EFAULT;
    if (vfs_resolve(oldp, &oldino, 0, 0, 0) < 0) return (uint64_t)-ENOENT;
    if ((oldino.mode & 0xF000u) == VFS_INODE_DIR) return (uint64_t)-EISDIR;
    if (vfs_resolve(newp, &newino, 0, 0, 0) == 0 && (newino.mode & 0xF000u) == VFS_INODE_DIR) return (uint64_t)-EISDIR;

    n = vfs_read_file(oldp, tmp, sizeof(tmp));
    if (n < 0) return (uint64_t)-EIO;
    if (vfs_write_file(newp, tmp, (uint32_t)n) < 0) return (uint64_t)-EXDEV;
    if (vfs_unlink(oldp) < 0) return (uint64_t)-EIO;
    return 0;
}

static uint64_t do_sys_writefile(uint64_t path_u, uint64_t buf_u, uint64_t len) {
    char path[256];
    static char tmp[131072];
    if (!path_u) return (uint64_t)-EINVAL;
    if (len > sizeof(tmp)) return (uint64_t)-EINVAL;
    if (copy_user_cstr(path, sizeof(path), path_u) < 0) return (uint64_t)-EFAULT;
    if (len && (!buf_u || copy_from_user(tmp, buf_u, len) < 0)) return (uint64_t)-EFAULT;
    return vfs_write_file(path, tmp, (uint32_t)len) < 0 ? (uint64_t)-EINVAL : 0;
}

static uint64_t do_sys_readfile(uint64_t path_u, uint64_t buf_u, uint64_t max_len) {
    char path[256];
    static char tmp[131072];
    int n;
    if (!path_u || !buf_u) return (uint64_t)-EINVAL;
    if (max_len == 0) return 0;
    if (max_len > sizeof(tmp)) max_len = sizeof(tmp);
    if (copy_user_cstr(path, sizeof(path), path_u) < 0) return (uint64_t)-EFAULT;
    n = vfs_read_file(path, tmp, (uint32_t)max_len);
    if (n < 0) return (uint64_t)-EINVAL;
    if (copy_to_user(buf_u, tmp, (uint64_t)n) < 0) return (uint64_t)-EFAULT;
    return (uint64_t)n;
}

static uint64_t do_sys_readlinkat(uint64_t dirfd_u, uint64_t path_u, uint64_t buf_u, uint64_t bufsz_u) {
    int dirfd = (int)dirfd_u;
    uint64_t bufsz = bufsz_u;
    char path_in[256];
    char path[256];
    char target[256];
    int n = 0;

    if (!path_u || !buf_u) return (uint64_t)-EINVAL;
    if (bufsz == 0) return (uint64_t)-EINVAL;
    if (copy_user_cstr(path_in, sizeof(path_in), path_u) < 0) return (uint64_t)-EFAULT;
    if (build_at_path(dirfd, path_in, path, (int)sizeof(path)) < 0) return (uint64_t)-EINVAL;

    if (strncmp(path, "/proc/self/fd/", 14) == 0) {
        const char *pnum = path + 14;
        int fd = 0;
        edge_fd_proc_t *fp = fd_proc_with_stdio();
        edge_fd_t *e;
        if (!*pnum) return (uint64_t)-ENOENT;
        for (const char *q = pnum; *q; ++q) {
            if (*q < '0' || *q > '9') return (uint64_t)-ENOENT;
            fd = fd * 10 + (*q - '0');
            if (fd > 1000000) return (uint64_t)-ENOENT;
        }
        e = fd_get(fp, fd);
        if (!e) return (uint64_t)-ENOENT;

        target[0] = 0;
        if (e->kind == FD_PTY_SLAVE) {
            int pty_id = e->pipe_id;
            int ti = 0;
            if (pty_id < 0 || pty_id >= EDGE_MAX_PTYS || !g_ptys[pty_id].used) return (uint64_t)-ENOENT;
            strcpy(target, "/dev/pts/");
            ti = (int)strlen(target);
            if (pty_id == 0) {
                target[ti++] = '0';
            } else {
                char rev[16];
                int rn = 0;
                int x = pty_id;
                while (x > 0 && rn < (int)sizeof(rev)) {
                    rev[rn++] = (char)('0' + (x % 10));
                    x /= 10;
                }
                while (rn > 0 && ti < (int)sizeof(target) - 1) target[ti++] = rev[--rn];
            }
            target[ti] = 0;
        } else if (e->kind == FD_PTY_MASTER) {
            strncpy(target, "/dev/ptmx", sizeof(target) - 1);
            target[sizeof(target) - 1] = 0;
        } else if (e->kind == FD_CONSOLE) {
            strncpy(target, "/dev/console", sizeof(target) - 1);
            target[sizeof(target) - 1] = 0;
        } else if (e->kind == FD_VFS) {
            if (!e->path[0]) return (uint64_t)-ENOENT;
            strncpy(target, e->path, sizeof(target) - 1);
            target[sizeof(target) - 1] = 0;
        } else if (e->kind == FD_PIPE_R || e->kind == FD_PIPE_W) {
            strcpy(target, "pipe:[");
            {
                int ti = (int)strlen(target);
                int x = e->pipe_id;
                char rev[16];
                int rn = 0;
                if (x < 0) {
                    if (ti < (int)sizeof(target) - 1) target[ti++] = '-';
                    x = -x;
                }
                if (x == 0) rev[rn++] = '0';
                while (x > 0 && rn < (int)sizeof(rev)) {
                    rev[rn++] = (char)('0' + (x % 10));
                    x /= 10;
                }
                while (rn > 0 && ti < (int)sizeof(target) - 2) target[ti++] = rev[--rn];
                target[ti++] = ']';
                target[ti] = 0;
            }
        } else if (e->kind == FD_SOCKET) {
            strcpy(target, "socket:[");
            {
                int ti = (int)strlen(target);
                int x = e->pipe_id;
                char rev[16];
                int rn = 0;
                if (x < 0) {
                    if (ti < (int)sizeof(target) - 1) target[ti++] = '-';
                    x = -x;
                }
                if (x == 0) rev[rn++] = '0';
                while (x > 0 && rn < (int)sizeof(rev)) {
                    rev[rn++] = (char)('0' + (x % 10));
                    x /= 10;
                }
                while (rn > 0 && ti < (int)sizeof(target) - 2) target[ti++] = rev[--rn];
                target[ti++] = ']';
                target[ti] = 0;
            }
        } else {
            return (uint64_t)-ENOENT;
        }
    } else {
        return (uint64_t)-EINVAL;
    }

    n = (int)strlen(target);
    if (n < 0) return (uint64_t)-EINVAL;
    if ((uint64_t)n > bufsz) n = (int)bufsz;
    if (n > 0 && copy_to_user(buf_u, target, (uint64_t)n) < 0) return (uint64_t)-EFAULT;
    return (uint64_t)n;
}

static uint64_t do_sys_readlink(uint64_t path_u, uint64_t buf_u, uint64_t bufsz_u) {
    return do_sys_readlinkat((uint64_t)LINUX_AT_FDCWD, path_u, buf_u, bufsz_u);
}

static uint64_t do_sys_openat(uint64_t dirfd_u, uint64_t path_u, uint64_t flags_u, uint64_t mode_u) {
    (void)mode_u;
    int dirfd = (int)dirfd_u;
    int flags = (int)flags_u;
    char path_in[256];
    char path[256];
    vfs_inode_t ino;
    vfs_superblock_t *sb = 0;

    edge_fd_proc_t *p = fd_proc_with_stdio();
    if (!p) return (uint64_t)-ENOMEM;

    if (!path_u || copy_user_cstr(path_in, sizeof(path_in), path_u) < 0) return (uint64_t)-EFAULT;
    if (build_at_path(dirfd, path_in, path, (int)sizeof(path)) < 0) return (uint64_t)-EINVAL;

    if (strcmp(path, "/dev/tty") == 0) {
        task_t *cur = process_current_task();
        int fd = fd_alloc(p, 0);
        edge_fd_t *e;
        if (fd < 0) return (uint64_t)-ENOMEM;
        e = &p->fds[fd];
        e->file_ref = file_ref_alloc();
        if (!e->file_ref) {
            memset(e, 0, sizeof(*e));
            return (uint64_t)-ENOMEM;
        }
        e->flags = flags;
        e->fd_flags = (flags & LINUX_O_CLOEXEC) ? LINUX_FD_CLOEXEC : 0;
        e->pos = 0;
        e->pipe_id = -1;
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = 0;

        if (!cur || cur->ctty_kind == PROCESS_CTTY_NONE) {
            memset(e, 0, sizeof(*e));
            return (uint64_t)-ENXIO;
        }
        if (cur->ctty_kind == PROCESS_CTTY_CONSOLE) {
            e->kind = FD_CONSOLE;
            return (uint64_t)fd;
        }
        if (cur->ctty_kind == PROCESS_CTTY_PTY) {
            int pty_id = cur->ctty_id;
            if (pty_id < 0 || pty_id >= EDGE_MAX_PTYS || !g_ptys[pty_id].used) {
                memset(e, 0, sizeof(*e));
                return (uint64_t)-ENXIO;
            }
            pty_add_ref(pty_id, 0);
            e->kind = FD_PTY_SLAVE;
            e->pipe_id = pty_id;
            return (uint64_t)fd;
        }
        memset(e, 0, sizeof(*e));
        return (uint64_t)-ENXIO;
    }

    if (strcmp(path, "/dev/ptmx") == 0) {
        int fd = fd_alloc(p, 0);
        int pty_id;
        edge_fd_t *e;
        if (fd < 0) return (uint64_t)-ENOMEM;
        pty_id = pty_alloc();
        if (pty_id < 0) {
            memset(&p->fds[fd], 0, sizeof(p->fds[fd]));
            return (uint64_t)-ENOMEM;
        }
        e = &p->fds[fd];
        e->file_ref = file_ref_alloc();
        if (!e->file_ref) {
            pty_drop_ref(pty_id, 1);
            memset(e, 0, sizeof(*e));
            return (uint64_t)-ENOMEM;
        }
        e->kind = FD_PTY_MASTER;
        e->flags = flags;
        e->fd_flags = (flags & LINUX_O_CLOEXEC) ? LINUX_FD_CLOEXEC : 0;
        e->pipe_id = pty_id;
        e->pos = 0;
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = 0;
        return (uint64_t)fd;
    }
    if (strncmp(path, "/dev/pts/", 9) == 0) {
        int pty_id = 0;
        const char *n = path + 9;
        int fd;
        edge_fd_t *e;
        if (!n[0]) return (uint64_t)-ENOENT;
        for (const char *q = n; *q; ++q) {
            if (*q < '0' || *q > '9') return (uint64_t)-ENOENT;
            pty_id = pty_id * 10 + (*q - '0');
            if (pty_id >= EDGE_MAX_PTYS) return (uint64_t)-ENOENT;
        }
        if (pty_id < 0 || pty_id >= EDGE_MAX_PTYS || !g_ptys[pty_id].used || !g_ptys[pty_id].unlocked) return (uint64_t)-ENOENT;
        fd = fd_alloc(p, 0);
        if (fd < 0) return (uint64_t)-ENOMEM;
        e = &p->fds[fd];
        e->file_ref = file_ref_alloc();
        if (!e->file_ref) {
            memset(e, 0, sizeof(*e));
            return (uint64_t)-ENOMEM;
        }
        pty_add_ref(pty_id, 0);
        if (g_ptys[pty_id].fg_pgid <= 0) {
            int pg = process_getpgid(0);
            if (pg > 0) g_ptys[pty_id].fg_pgid = pg;
        }
        e->kind = FD_PTY_SLAVE;
        e->flags = flags;
        e->fd_flags = (flags & LINUX_O_CLOEXEC) ? LINUX_FD_CLOEXEC : 0;
        e->pipe_id = pty_id;
        e->pos = 0;
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = 0;
        return (uint64_t)fd;
    }

    if (vfs_resolve(path, &ino, &sb, 0, 0) < 0) {
        if (strcmp(path, "/proc") == 0 || strncmp(path, "/proc/", 6) == 0) {
            (void)vfs_mkdir("/proc");
            (void)vfs_mount("proc", "/proc", "proc");
        }
    }
    if (vfs_resolve(path, &ino, &sb, 0, 0) < 0) {
        if ((flags & LINUX_O_CREAT) == 0) return (uint64_t)-ENOENT;
        if (vfs_write_file(path, "", 0) < 0) return (uint64_t)-ENOENT;
        if (vfs_resolve(path, &ino, &sb, 0, 0) < 0) return (uint64_t)-ENOENT;
    }
    {
        int req = 0;
        int acc = flags & LINUX_O_ACCMODE;
        if (acc == LINUX_O_WRONLY) req = 2;
        else if (acc == LINUX_O_RDWR) req = 6;
        else req = 4;
        if (vfs_permission_check(&ino, req, process_current_task()) < 0) return (uint64_t)-EACCES;
    }

    int fd = fd_alloc(p, 0);
    if (fd < 0) return (uint64_t)-ENOMEM;

    edge_fd_t *e = &p->fds[fd];
    e->file_ref = file_ref_alloc();
    if (!e->file_ref) {
        memset(e, 0, sizeof(*e));
        return (uint64_t)-ENOMEM;
    }
    e->kind = FD_VFS;
    e->flags = flags;
    e->fd_flags = (flags & LINUX_O_CLOEXEC) ? LINUX_FD_CLOEXEC : 0;
    e->pos = 0;
    e->inode = ino;
    e->sb = sb;
    e->pipe_id = -1;
    strncpy(e->path, path, sizeof(e->path) - 1);
    e->path[sizeof(e->path) - 1] = 0;

    if ((flags & LINUX_O_TRUNC) != 0 && (flags & (LINUX_O_WRONLY | LINUX_O_RDWR)) != 0) {
        (void)vfs_write_file(path, "", 0);
        e->inode.size = 0;
    }

    if ((flags & LINUX_O_APPEND) != 0) e->pos = e->inode.size;
    if (strcmp(path, "/dev/fb0") == 0) {
        if (g_fb_console_hold_count++ == 0) fb_console_set_present_enabled(0);
    }
    return (uint64_t)fd;
}

static uint64_t do_sys_linkat(uint64_t olddirfd_u, uint64_t oldpath_u, uint64_t newdirfd_u, uint64_t newpath_u, uint64_t flags_u) {
    int olddirfd = (int)olddirfd_u;
    int newdirfd = (int)newdirfd_u;
    char old_in[256];
    char new_in[256];
    char old_path[256];
    char new_path[256];
    vfs_inode_t old_ino;
    static char copy_buf[65536];
    int n;
    (void)flags_u;

    if (!oldpath_u || !newpath_u) return (uint64_t)-EINVAL;
    if (copy_user_cstr(old_in, sizeof(old_in), oldpath_u) < 0) return (uint64_t)-EFAULT;
    if (copy_user_cstr(new_in, sizeof(new_in), newpath_u) < 0) return (uint64_t)-EFAULT;
    if (build_at_path(olddirfd, old_in, old_path, (int)sizeof(old_path)) < 0) return (uint64_t)-EINVAL;
    if (build_at_path(newdirfd, new_in, new_path, (int)sizeof(new_path)) < 0) return (uint64_t)-EINVAL;
    if (vfs_resolve(old_path, &old_ino, 0, 0, 0) < 0) return (uint64_t)-ENOENT;
    if ((old_ino.mode & 0xF000u) == VFS_INODE_DIR) return (uint64_t)-EPERM;
    if (vfs_resolve(new_path, 0, 0, 0, 0) == 0) return (uint64_t)-EEXIST;

    /* Minimal compatibility: emulate hardlink by creating a copied backup file. */
    n = vfs_read_file(old_path, copy_buf, sizeof(copy_buf));
    if (n < 0) return (uint64_t)-EIO;
    if (n >= (int)sizeof(copy_buf)) return (uint64_t)-EIO;
    if (vfs_write_file(new_path, copy_buf, (uint32_t)n) < 0) return (uint64_t)-EIO;
    return 0;
}

static uint64_t do_sys_link(uint64_t oldpath_u, uint64_t newpath_u) {
    return do_sys_linkat((uint64_t)LINUX_AT_FDCWD, oldpath_u, (uint64_t)LINUX_AT_FDCWD, newpath_u, 0);
}

static uint64_t do_sys_close(uint64_t fd_u) {
    int fd = (int)fd_u;
    edge_fd_proc_t *p = fd_proc_for_pid(process_getpid(), 0);
    if (!p) return (uint64_t)-EBADF;
    edge_fd_t *e = fd_get(p, fd);
    if (!e) return (uint64_t)-EBADF;
    fd_log_lifecycle("close", process_getpid(), fd, e, 0);
    if (e->file_ref > 0) (void)file_ref_put(e->file_ref);

    if (e->kind == FD_PIPE_R) pipe_drop_reader(e->pipe_id);
    if (e->kind == FD_PIPE_W) pipe_drop_writer(e->pipe_id);
    if (e->kind == FD_SOCKET) socket_drop_ref(e->pipe_id);
    if (e->kind == FD_PTY_MASTER) pty_drop_ref(e->pipe_id, 1);
    if (e->kind == FD_PTY_SLAVE) pty_drop_ref(e->pipe_id, 0);
    if (e->kind == FD_EVENTFD) eventfd_drop_ref(e->pipe_id);
    if (e->kind == FD_TIMERFD) timerfd_drop_ref(e->pipe_id);
    if (e->kind == FD_SIGNALFD) signalfd_drop_ref(e->pipe_id);
    if (e->kind == FD_EPOLL) epoll_drop_ref(e->pipe_id);
    if (e->kind == FD_VFS && strcmp(e->path, "/dev/fb0") == 0) {
        if (g_fb_console_hold_count > 0) {
            g_fb_console_hold_count--;
            if (g_fb_console_hold_count == 0) fb_console_set_present_enabled(1);
        }
    }

    memset(e, 0, sizeof(*e));
    return 0;
}

static uint64_t do_sys_fsync(uint64_t fd_u) {
    int fd = (int)fd_u;
    edge_fd_proc_t *p = fd_proc_with_stdio();
    edge_fd_t *e = fd_get(p, fd);
    if (!e) return (uint64_t)-EBADF;
    return 0;
}

static uint64_t do_sys_fd_read(uint64_t fd_u, uint64_t buf_u, uint64_t len_u) {
    int fd = (int)fd_u;
    uint64_t len = len_u;
    char chunk[4096];
    task_t *cur = process_current_task();

    if (!buf_u) return (uint64_t)-EINVAL;
    if (len == 0) return 0;
    edge_fd_proc_t *p = fd_proc_with_stdio();
    if (!p) return (uint64_t)-EBADF;
    edge_fd_t *e = fd_get(p, fd);
    if (!e) return (uint64_t)-EBADF;

    if (e->kind == FD_CONSOLE) return do_sys_read(fd_u, buf_u, len_u);
    if (e->kind == FD_PTY_MASTER || e->kind == FD_PTY_SLAVE) {
        edge_pty_t *pty;
        uint8_t *src_buf;
        uint32_t *src_rpos;
        uint32_t *src_count;
        int peer_refs;
        uint64_t n = 0;
        if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_PTYS) return (uint64_t)-EBADF;
        pty = &g_ptys[e->pipe_id];
        if (!pty->used) return (uint64_t)-EBADF;
        if (e->kind == FD_PTY_MASTER) {
            src_buf = pty->s2m_buf;
            src_rpos = &pty->s2m_rpos;
            src_count = &pty->s2m_count;
            peer_refs = pty->refs_slave;
        } else {
            src_buf = pty->m2s_buf;
            src_rpos = &pty->m2s_rpos;
            src_count = &pty->m2s_count;
            peer_refs = pty->refs_master;
        }
        while (*src_count == 0 ||
               (e->kind == FD_PTY_SLAVE &&
                (pty->termios.c_lflag & LINUX_ICANON) != 0 &&
                !pty_slave_input_have_canonical_line(pty))) {
            if (peer_refs <= 0) return 0;
            if ((e->flags & LINUX_O_NONBLOCK) != 0) return (uint64_t)-EAGAIN;
            if (signal_pending_interrupt()) return tty_interrupt_current_ret();
            wait_blocking_step();
            peer_refs = (e->kind == FD_PTY_MASTER) ? pty->refs_slave : pty->refs_master;
        }
        if (e->kind == FD_PTY_SLAVE) n = pty_slave_read_limit(pty, len, *src_count);
        else {
            n = len;
            if (n > *src_count) n = *src_count;
        }
        for (uint64_t i = 0; i < n; ++i) {
            chunk[i % sizeof(chunk)] = (char)src_buf[*src_rpos];
            *src_rpos = (*src_rpos + 1) % EDGE_PTY_BUF_SIZE;
            (*src_count)--;
            if ((i % sizeof(chunk)) == sizeof(chunk) - 1 || i + 1 == n) {
                uint64_t start = i - (i % sizeof(chunk));
                uint64_t sz = (i % sizeof(chunk)) + 1;
                if (copy_to_user(buf_u + start, chunk, sz) < 0) return (uint64_t)-EFAULT;
            }
        }
        return n;
    }
    if (e->kind == FD_SOCKET) {
        uint64_t r = do_sys_recvfrom(fd_u, buf_u, len_u, 0, 0, 0);
        if (ssh_trace_task(cur)) {
            printf("[sshdbg] read pid=%d cmd=%s fd=%d kind=socket len=%u ret=%d fl=0x%x sid=%d\n",
                   cur->pid, cur->name, fd, (unsigned)len, (int)(int64_t)r, (unsigned)e->flags, e->pipe_id);
        }
        return r;
    }
    if (e->kind == FD_EVENTFD) {
        edge_eventfd_t *ev;
        uint64_t v;
        if (len < 8) return (uint64_t)-EINVAL;
        if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_EVENTFDS) return (uint64_t)-EBADF;
        ev = &g_eventfds[e->pipe_id];
        if (!ev->used) return (uint64_t)-EBADF;
        while (ev->counter == 0) {
            if ((e->flags & LINUX_O_NONBLOCK) != 0) return (uint64_t)-EAGAIN;
            if (signal_pending_interrupt()) return tty_interrupt_current_ret();
            wait_blocking_step();
        }
        if (ev->semaphore) {
            v = 1;
            ev->counter--;
        } else {
            v = ev->counter;
            ev->counter = 0;
        }
        if (copy_to_user(buf_u, &v, 8) < 0) return (uint64_t)-EFAULT;
        return 8;
    }
    if (e->kind == FD_TIMERFD) {
        edge_timerfd_t *tf;
        uint64_t expirations;
        if (len < 8) return (uint64_t)-EINVAL;
        if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_TIMERFDS) return (uint64_t)-EBADF;
        tf = &g_timerfds[e->pipe_id];
        if (!tf->used) return (uint64_t)-EBADF;
        while ((expirations = timerfd_expire_count(tf, 1)) == 0) {
            if ((e->flags & LINUX_O_NONBLOCK) != 0) return (uint64_t)-EAGAIN;
            if (signal_pending_interrupt()) return tty_interrupt_current_ret();
            wait_blocking_step();
        }
        if (copy_to_user(buf_u, &expirations, 8) < 0) return (uint64_t)-EFAULT;
        return 8;
    }
    if (e->kind == FD_SIGNALFD) {
        edge_signalfd_t *sf;
        struct edge_linux_signalfd_siginfo si;
        if (len < sizeof(si)) return (uint64_t)-EINVAL;
        if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_SIGNALFDS) return (uint64_t)-EBADF;
        sf = &g_signalfds[e->pipe_id];
        if (!sf->used) return (uint64_t)-EBADF;
        while (!signalfd_peek_current(sf, 1, &si)) {
            if ((e->flags & LINUX_O_NONBLOCK) != 0) return (uint64_t)-EAGAIN;
            wait_blocking_step();
        }
        if (copy_to_user(buf_u, &si, sizeof(si)) < 0) return (uint64_t)-EFAULT;
        return sizeof(si);
    }
    if (e->kind == FD_PIDFD) return (uint64_t)-EINVAL;
    if (e->kind == FD_EPOLL) return (uint64_t)-EINVAL;

    if (e->kind == FD_PIPE_R) {
        if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_PIPES) return (uint64_t)-EBADF;
        edge_pipe_t *pp = &g_pipes[e->pipe_id];
        if (!pp->used) return (uint64_t)-EBADF;
        while (pp->count == 0) {
            if (pp->writers == 0) return 0;
            if ((e->flags & LINUX_O_NONBLOCK) != 0) return (uint64_t)-EAGAIN;
            if (signal_pending_interrupt()) return tty_interrupt_current_ret();
            wait_blocking_step();
        }
        uint64_t n = len;
        if (n > pp->count) n = pp->count;
        for (uint64_t i = 0; i < n; ++i) {
            chunk[i % sizeof(chunk)] = (char)pp->buf[pp->rpos];
            pp->rpos = (pp->rpos + 1) % EDGE_PIPE_SIZE;
            pp->count--;
            if ((i % sizeof(chunk)) == sizeof(chunk) - 1 || i + 1 == n) {
                uint64_t start = i - (i % sizeof(chunk));
                uint64_t sz = (i % sizeof(chunk)) + 1;
                if (copy_to_user(buf_u + start, chunk, sz) < 0) return (uint64_t)-EFAULT;
            }
        }
        if (ssh_trace_task(cur)) {
            printf("[sshdbg] read pid=%d cmd=%s fd=%d kind=pipe len=%u ret=%u fl=0x%x pidx=%d\n",
                   cur->pid, cur->name, fd, (unsigned)len, (unsigned)n, (unsigned)e->flags, e->pipe_id);
        }
        return n;
    }

    if (e->kind == FD_VFS && path_is_console_tty(e->path)) return do_sys_read(fd_u, buf_u, len_u);
    if (e->kind == FD_VFS && path_is_mouse_input(e->path)) {
        uint64_t done = 0;
        char mchunk[256];
        while (done < len) {
            uint64_t n = len - done;
            int r;
            if (n > sizeof(mchunk)) n = sizeof(mchunk);
            r = keyboard_mouse_read(mchunk, (uint32_t)n, 0);
            if (r > 0) {
                if (copy_to_user(buf_u + done, mchunk, (uint64_t)r) < 0) return (uint64_t)-EFAULT;
                done += (uint64_t)r;
                break;
            }
            if ((e->flags & LINUX_O_NONBLOCK) != 0) return done ? done : (uint64_t)-EAGAIN;
            if (signal_pending_interrupt()) return tty_interrupt_current_ret();
            wait_blocking_step();
        }
        return done;
    }

    if (e->kind != FD_VFS) return (uint64_t)-EBADF;
    if ((e->inode.mode & 0xF000) == VFS_INODE_DIR) return (uint64_t)-EISDIR;

    uint64_t done = 0;
    while (done < len) {
        uint64_t n = len - done;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        int r = -1;

        if ((e->inode.mode & 0xF000) == VFS_INODE_CHR || (e->inode.mode & 0xF000) == VFS_INODE_BLK) {
            r = vfs_read_file(e->path, chunk, (uint32_t)n);
        } else if (e->sb && e->sb->ops && e->sb->ops->read) {
            r = e->sb->ops->read(e->sb, &e->inode, (uint32_t)e->pos, chunk, (uint32_t)n);
            if (r > 0) e->pos += (uint64_t)r;
        }

        if (r < 0) return (uint64_t)-EINVAL;
        if (r == 0) break;
        if (copy_to_user(buf_u + done, chunk, (uint64_t)r) < 0) return (uint64_t)-EFAULT;
        done += (uint64_t)r;
        if ((uint64_t)r < n) break;
    }
    if (ssh_trace_task(cur)) {
        printf("[sshdbg] read pid=%d cmd=%s fd=%d kind=vfs len=%u ret=%u fl=0x%x\n",
               cur->pid, cur->name, fd, (unsigned)len, (unsigned)done, (unsigned)e->flags);
    }
    return done;
}

static uint64_t do_sys_fd_write(uint64_t fd_u, uint64_t buf_u, uint64_t len_u) {
    int fd = (int)fd_u;
    uint64_t len = len_u;
    char chunk[4096];
    task_t *cur = process_current_task();

    if (!buf_u) return (uint64_t)-EINVAL;
    if (len == 0) return 0;
    edge_fd_proc_t *p = fd_proc_with_stdio();
    if (!p) return (uint64_t)-EBADF;
    edge_fd_t *e = fd_get(p, fd);
    if (!e) return (uint64_t)-EBADF;

#if EDGE_BB_FD_TRACE
    if (fd == 1 && g_bb_fd_trace_budget > 0) {
        task_t *t = process_current_task();
        if (t && strcmp(t->name, "busybox") == 0) {
            printf("[bbfd] pid=%d fd1 kind=%d flags=0x%x fdflags=0x%x pipe=%d len=%u\n",
                   t->pid, (int)e->kind, (unsigned)e->flags, (unsigned)e->fd_flags, e->pipe_id, (unsigned)len);
            g_bb_fd_trace_budget--;
        }
    }
#endif

    if (e->kind == FD_CONSOLE) return do_sys_write(fd_u, buf_u, len_u);
    if (e->kind == FD_PTY_MASTER || e->kind == FD_PTY_SLAVE) {
        edge_pty_t *pty;
        uint8_t *dst_buf;
        uint32_t *dst_wpos;
        uint32_t *dst_count;
        int peer_refs;
        uint64_t n = 0;
        if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_PTYS) return (uint64_t)-EBADF;
        pty = &g_ptys[e->pipe_id];
        if (!pty->used) return (uint64_t)-EBADF;
        if (e->kind == FD_PTY_MASTER) {
            dst_buf = pty->m2s_buf;
            dst_wpos = &pty->m2s_wpos;
            dst_count = &pty->m2s_count;
            peer_refs = pty->refs_slave;
        } else {
            dst_buf = pty->s2m_buf;
            dst_wpos = &pty->s2m_wpos;
            dst_count = &pty->s2m_count;
            peer_refs = pty->refs_master;
        }
        while (*dst_count >= EDGE_PTY_BUF_SIZE) {
            if (peer_refs <= 0) return (uint64_t)-EPIPE;
            if ((e->flags & LINUX_O_NONBLOCK) != 0) return n ? n : (uint64_t)-EAGAIN;
            if (signal_pending_interrupt()) return tty_interrupt_current_ret();
            wait_blocking_step();
            peer_refs = (e->kind == FD_PTY_MASTER) ? pty->refs_slave : pty->refs_master;
        }
        n = len;
        if (n > (uint64_t)(EDGE_PTY_BUF_SIZE - *dst_count)) n = (uint64_t)(EDGE_PTY_BUF_SIZE - *dst_count);
        for (uint64_t i = 0; i < n; ++i) {
            char c;
            uint32_t need_slots = 1;
            if (copy_from_user(&c, buf_u + i, 1) < 0) return (uint64_t)-EFAULT;
            if (e->kind == FD_PTY_SLAVE &&
                c == '\n' &&
                (pty->termios.c_oflag & LINUX_OPOST) != 0 &&
                (pty->termios.c_oflag & LINUX_ONLCR) != 0) {
                need_slots = 2;
            }
            if ((uint32_t)(EDGE_PTY_BUF_SIZE - *dst_count) < need_slots) {
                n = i;
                break;
            }
            if (e->kind == FD_PTY_SLAVE &&
                c == '\n' &&
                (pty->termios.c_oflag & LINUX_OPOST) != 0 &&
                (pty->termios.c_oflag & LINUX_ONLCR) != 0) {
                dst_buf[*dst_wpos] = (uint8_t)'\r';
                *dst_wpos = (*dst_wpos + 1) % EDGE_PTY_BUF_SIZE;
                (*dst_count)++;
            }
            if (e->kind == FD_PTY_MASTER &&
                (pty->termios.c_lflag & LINUX_ISIG) &&
                (unsigned char)c == 3) {
                int fg = pty->fg_pgid;
                if (fg <= 0) fg = process_getpgid(0);
                if (fg > 0) (void)do_sys_kill((uint64_t)(int64_t)(-fg), LINUX_SIGINT);
                if ((pty->termios.c_lflag & LINUX_ECHO) != 0) {
                    const char echo_seq[3] = {'^', 'C', '\n'};
                    for (int ei = 0; ei < 3; ++ei) {
                        if (pty->s2m_count >= EDGE_PTY_BUF_SIZE) break;
                        pty->s2m_buf[pty->s2m_wpos] = (uint8_t)echo_seq[ei];
                        pty->s2m_wpos = (pty->s2m_wpos + 1) % EDGE_PTY_BUF_SIZE;
                        pty->s2m_count++;
                    }
                }
                continue;
            }
            if (e->kind == FD_PTY_MASTER &&
                (pty->termios.c_lflag & LINUX_ICANON) &&
                (unsigned char)c == pty->termios.c_cc[LINUX_VERASE]) {
                if (pty->m2s_count > 0) {
                    uint32_t prev = (pty->m2s_wpos + EDGE_PTY_BUF_SIZE - 1) % EDGE_PTY_BUF_SIZE;
                    if (pty->m2s_buf[prev] != '\n') {
                        pty->m2s_wpos = prev;
                        pty->m2s_count--;
                        if ((pty->termios.c_lflag & LINUX_ECHO) != 0) {
                            pty_echo_seq_to_master(pty, "\b \b", 3);
                        }
                    }
                }
                continue;
            }
            if (e->kind == FD_PTY_MASTER && (pty->termios.c_lflag & LINUX_ECHO) != 0) {
                if (c == '\r' || c == '\n') {
                    if ((pty->termios.c_oflag & LINUX_OPOST) && (pty->termios.c_oflag & LINUX_ONLCR)) {
                        pty_echo_seq_to_master(pty, "\r\n", 2);
                    } else {
                        pty_echo_to_master(pty, (uint8_t)c);
                    }
                } else {
                    pty_echo_to_master(pty, (uint8_t)c);
                }
            }
            dst_buf[*dst_wpos] = (uint8_t)c;
            *dst_wpos = (*dst_wpos + 1) % EDGE_PTY_BUF_SIZE;
            (*dst_count)++;
        }
        return n;
    }
    if (e->kind == FD_SOCKET) {
        uint64_t w = do_sys_sendto(fd_u, buf_u, len_u, 0, 0, 0);
        if (ssh_trace_task(cur)) {
            printf("[sshdbg] write pid=%d cmd=%s fd=%d kind=socket len=%u ret=%d fl=0x%x sid=%d\n",
                   cur->pid, cur->name, fd, (unsigned)len, (int)(int64_t)w, (unsigned)e->flags, e->pipe_id);
        }
        return w;
    }
    if (e->kind == FD_EVENTFD) {
        edge_eventfd_t *ev;
        uint64_t v;
        if (len < 8) return (uint64_t)-EINVAL;
        if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_EVENTFDS) return (uint64_t)-EBADF;
        ev = &g_eventfds[e->pipe_id];
        if (!ev->used) return (uint64_t)-EBADF;
        if (copy_from_user(&v, buf_u, 8) < 0) return (uint64_t)-EFAULT;
        if (v == UINT64_MAX) return (uint64_t)-EINVAL;
        while (UINT64_MAX - 1 - ev->counter < v) {
            if ((e->flags & LINUX_O_NONBLOCK) != 0) return (uint64_t)-EAGAIN;
            if (signal_pending_interrupt()) return tty_interrupt_current_ret();
            wait_blocking_step();
        }
        ev->counter += v;
        return 8;
    }
    if (e->kind == FD_TIMERFD || e->kind == FD_SIGNALFD || e->kind == FD_EPOLL || e->kind == FD_PIDFD) return (uint64_t)-EINVAL;

    if (e->kind == FD_PIPE_W) {
        if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_PIPES) return (uint64_t)-EBADF;
        edge_pipe_t *pp = &g_pipes[e->pipe_id];
        if (!pp->used || pp->readers == 0) return (uint64_t)-EPIPE;
        while (pp->count >= EDGE_PIPE_SIZE) {
            if (pp->readers == 0) return (uint64_t)-EPIPE;
            if ((e->flags & LINUX_O_NONBLOCK) != 0) return (uint64_t)-EAGAIN;
            if (signal_pending_interrupt()) return tty_interrupt_current_ret();
            wait_blocking_step();
        }
        uint64_t n = len;
        if (n > (uint64_t)(EDGE_PIPE_SIZE - pp->count)) n = (uint64_t)(EDGE_PIPE_SIZE - pp->count);
        for (uint64_t i = 0; i < n; ++i) {
            char c;
            if (copy_from_user(&c, buf_u + i, 1) < 0) return (uint64_t)-EFAULT;
            pp->buf[pp->wpos] = (uint8_t)c;
            pp->wpos = (pp->wpos + 1) % EDGE_PIPE_SIZE;
            pp->count++;
        }
        if (ssh_trace_task(cur)) {
            printf("[sshdbg] write pid=%d cmd=%s fd=%d kind=pipe len=%u ret=%u fl=0x%x pidx=%d\n",
                   cur->pid, cur->name, fd, (unsigned)len, (unsigned)n, (unsigned)e->flags, e->pipe_id);
        }
        return n;
    }

    if (e->kind == FD_VFS && path_is_console_tty(e->path)) return do_sys_write(fd_u, buf_u, len_u);

    if (e->kind != FD_VFS) return (uint64_t)-EBADF;
    if ((e->inode.mode & 0xF000) == VFS_INODE_DIR) return (uint64_t)-EISDIR;

    uint64_t done = 0;

    if ((e->inode.mode & 0xF000) != VFS_INODE_CHR &&
        (e->inode.mode & 0xF000) != VFS_INODE_BLK &&
        e->sb && e->sb->ops && e->sb->ops->write) {
        while (done < len) {
            uint64_t n = len - done;
            int w;
            if (n > (1u << 20)) n = (1u << 20);
            if (n > 0xFFFFFFFFull) n = 0xFFFFFFFFull;
            if (!user_range_ok(buf_u + done, n)) return done ? done : (uint64_t)-EFAULT;
            if ((e->flags & LINUX_O_APPEND) != 0) e->pos = e->inode.size;
            w = e->sb->ops->write(e->sb, &e->inode, (uint32_t)e->pos,
                                  (const void *)(uintptr_t)(buf_u + done), (uint32_t)n);
            if (w < 0) return done ? done : (uint64_t)-EINVAL;
            if (w == 0) break;
            e->pos += (uint64_t)w;
            done += (uint64_t)w;
            if ((uint64_t)w < n) break;
        }
        if (ssh_trace_task(cur)) {
            printf("[sshdbg] write pid=%d cmd=%s fd=%d kind=vfs len=%u ret=%u fl=0x%x\n",
                   cur->pid, cur->name, fd, (unsigned)len, (unsigned)done, (unsigned)e->flags);
        }
        return done;
    }

    while (done < len) {
        uint64_t n = len - done;
        if (n > sizeof(chunk)) n = sizeof(chunk);
        if (copy_from_user(chunk, buf_u + done, n) < 0) return (uint64_t)-EFAULT;

        int w = -1;
        if ((e->inode.mode & 0xF000) == VFS_INODE_CHR) {
            w = vfs_dev_pwrite(e->path, chunk, (uint32_t)n, e->pos);
            if (w > 0) e->pos += (uint64_t)w;
        } else if ((e->inode.mode & 0xF000) == VFS_INODE_BLK) {
            w = vfs_write_file(e->path, chunk, (uint32_t)n);
        } else if (e->sb && e->sb->ops && e->sb->ops->write) {
            if ((e->flags & LINUX_O_APPEND) != 0) e->pos = e->inode.size;
            w = e->sb->ops->write(e->sb, &e->inode, (uint32_t)e->pos, chunk, (uint32_t)n);
            if (w > 0) e->pos += (uint64_t)w;
        }

        if (w < 0) return (uint64_t)-EINVAL;
        done += (uint64_t)w;
        if ((uint64_t)w < n) break;
    }
    if (ssh_trace_task(cur)) {
        printf("[sshdbg] write pid=%d cmd=%s fd=%d kind=vfs len=%u ret=%u fl=0x%x\n",
               cur->pid, cur->name, fd, (unsigned)len, (unsigned)done, (unsigned)e->flags);
    }
    return done;
}

static uint64_t do_sys_readv(uint64_t fd_u, uint64_t iov_u, uint64_t iovcnt_u) {
    int iovcnt = (int)iovcnt_u;
    uint64_t total = 0;
    struct edge_iovec iov[64];

    if (!iov_u || iovcnt <= 0 || iovcnt > (int)(sizeof(iov) / sizeof(iov[0]))) return (uint64_t)-EINVAL;
    if (copy_from_user(iov, iov_u, (uint64_t)iovcnt * sizeof(iov[0])) < 0) return (uint64_t)-EFAULT;

    for (int i = 0; i < iovcnt; ++i) {
        uint64_t len = iov[i].iov_len;
        if (len == 0) continue;
        uint64_t r = do_sys_fd_read(fd_u, iov[i].iov_base, len);
        if ((int64_t)r < 0) return total ? total : r;
        total += r;
        if (r < len) break;
    }
    return total;
}

static uint64_t do_sys_writev(uint64_t fd_u, uint64_t iov_u, uint64_t iovcnt_u) {
    int iovcnt = (int)iovcnt_u;
    uint64_t total = 0;
    struct edge_iovec iov[64];

    if (!iov_u || iovcnt <= 0 || iovcnt > (int)(sizeof(iov) / sizeof(iov[0]))) return (uint64_t)-EINVAL;
    if (copy_from_user(iov, iov_u, (uint64_t)iovcnt * sizeof(iov[0])) < 0) return (uint64_t)-EFAULT;

    for (int i = 0; i < iovcnt; ++i) {
        uint64_t len = iov[i].iov_len;
        if (len == 0) continue;
        uint64_t w = do_sys_fd_write(fd_u, iov[i].iov_base, len);
        if ((int64_t)w < 0) return total ? total : w;
        total += w;
        if (w < len) break;
    }
    return total;
}

static uint64_t do_sys_lseek(uint64_t fd_u, uint64_t off_u, uint64_t whence_u) {
    int fd = (int)fd_u;
    int64_t off = (int64_t)off_u;
    int whence = (int)whence_u;

    edge_fd_proc_t *p = fd_proc_with_stdio();
    edge_fd_t *e = fd_get(p, fd);
    if (!e) return (uint64_t)-EBADF;

    int64_t base = 0;
    if (whence == LINUX_SEEK_SET) base = 0;
    else if (whence == LINUX_SEEK_CUR) base = (int64_t)e->pos;
    else if (whence == LINUX_SEEK_END) base = (int64_t)e->inode.size;
    else return (uint64_t)-EINVAL;

    int64_t np = base + off;
    if (np < 0) return (uint64_t)-EINVAL;
    e->pos = (uint64_t)np;
    return (uint64_t)np;
}

static uint64_t do_sys_fstat(uint64_t fd_u, uint64_t st_u) {
    int fd = (int)fd_u;
    struct edge_linux_stat st;

    if (!st_u) return (uint64_t)-EINVAL;
    edge_fd_proc_t *p = fd_proc_with_stdio();
    edge_fd_t *e = fd_get(p, fd);
    if (!e) return (uint64_t)-EBADF;

    if (e->kind == FD_PIPE_R || e->kind == FD_PIPE_W) {
        fill_kstat_mode_size((uint16_t)(LINUX_S_IFIFO | 0600), 0, &st);
    } else if (e->kind == FD_CONSOLE) {
        fill_kstat_mode_size((uint16_t)(LINUX_S_IFCHR | 0666), 0, &st);
    } else if (e->kind == FD_PTY_MASTER) {
        fill_kstat_mode_size((uint16_t)(LINUX_S_IFCHR | 0666), 0, &st);
        st.st_dev = 1;
        st.st_ino = 0xD0000000u + 12u; /* /dev/ptmx devnode index */
    } else if (e->kind == FD_PTY_SLAVE) {
        fill_kstat_mode_size((uint16_t)(LINUX_S_IFCHR | 0620), 0, &st);
        st.st_dev = 1;
        st.st_ino = 0xD0FFF100u + (uint64_t)(uint32_t)e->pipe_id;
    } else if (e->kind == FD_EVENTFD || e->kind == FD_TIMERFD || e->kind == FD_SIGNALFD || e->kind == FD_EPOLL || e->kind == FD_PIDFD) {
        fill_kstat_mode_size((uint16_t)(LINUX_S_IFCHR | 0600), 0, &st);
        st.st_ino = 0xE0000000u + (uint64_t)(uint32_t)(e->kind << 16) + (uint64_t)(uint32_t)(e->pipe_id & 0xFFFF);
    } else {
        fill_kstat(&e->inode, &st);
    }
    if (copy_to_user(st_u, &st, sizeof(st)) < 0) return (uint64_t)-EFAULT;
    return 0;
}

static uint64_t do_sys_newfstatat(uint64_t dirfd_u, uint64_t path_u, uint64_t st_u, uint64_t flags_u) {
    (void)flags_u;
    int dirfd = (int)dirfd_u;
    char path_in[256];
    char path[256];
    struct edge_linux_stat st;
    vfs_inode_t ino;

    if (!st_u || !path_u) return (uint64_t)-EINVAL;
    if (copy_user_cstr(path_in, sizeof(path_in), path_u) < 0) return (uint64_t)-EFAULT;
    if (build_at_path(dirfd, path_in, path, (int)sizeof(path)) < 0) return (uint64_t)-EINVAL;

    if (vfs_resolve(path, &ino, 0, 0, 0) < 0) return (uint64_t)-ENOENT;
    fill_kstat(&ino, &st);
    if (copy_to_user(st_u, &st, sizeof(st)) < 0) return (uint64_t)-EFAULT;
    return 0;
}

static uint64_t do_sys_getdents64(uint64_t fd_u, uint64_t dirp_u, uint64_t count_u) {
    int fd = (int)fd_u;
    uint64_t count = count_u;
    uint64_t written = 0;
    const uint16_t d64_base = (uint16_t)__builtin_offsetof(struct edge_linux_dirent64, d_name);

    edge_fd_proc_t *p = fd_proc_with_stdio();
    edge_fd_t *e = fd_get(p, fd);
    if (!e) return (uint64_t)-EBADF;
    if ((e->inode.mode & 0xF000) != VFS_INODE_DIR) return (uint64_t)-ENOTDIR;
    if (count < (uint64_t)(d64_base + 2u)) return (uint64_t)-EINVAL;

    if (strcmp(e->path, "/dev/pts") == 0) {
        uint64_t idx = e->pos;
        uint64_t total = 2;
        for (int i = 0; i < EDGE_MAX_PTYS; ++i) {
            if (g_ptys[i].used) total++;
        }
        while (idx < total) {
            const char *name = 0;
            char numbuf[16];
            uint8_t dtype = LINUX_DT_CHR;
            if (idx == 0) {
                name = ".";
                dtype = LINUX_DT_DIR;
            } else if (idx == 1) {
                name = "..";
                dtype = LINUX_DT_DIR;
            } else {
                uint64_t seen = 0;
                int pty_id = -1;
                for (int i = 0; i < EDGE_MAX_PTYS; ++i) {
                    if (!g_ptys[i].used) continue;
                    if (seen == idx - 2) {
                        pty_id = i;
                        break;
                    }
                    seen++;
                }
                if (pty_id < 0) break;
                int n = 0;
                int x = pty_id;
                char tmp[16];
                if (x == 0) {
                    tmp[n++] = '0';
                } else {
                    while (x > 0 && n < (int)sizeof(tmp)) {
                        tmp[n++] = (char)('0' + (x % 10));
                        x /= 10;
                    }
                }
                for (int i = 0; i < n; ++i) numbuf[i] = tmp[n - 1 - i];
                numbuf[n] = 0;
                name = numbuf;
            }

            uint32_t namelen = (uint32_t)strlen(name);
            uint16_t reclen = (uint16_t)((d64_base + namelen + 1u + 7u) & ~7u);
            if (written + reclen > count) break;

            {
                uint8_t rec[320];
                struct edge_linux_dirent64 *d = (struct edge_linux_dirent64 *)rec;
                if (reclen > sizeof(rec)) return (uint64_t)-EINVAL;
                memset(rec, 0, reclen);
                d->d_ino = (idx < 2) ? (uint64_t)2 : (uint64_t)(0xD0FFF100u + (uint64_t)(idx - 2));
                d->d_off = (int64_t)(idx + 1);
                d->d_reclen = reclen;
                d->d_type = dtype;
                memcpy(d->d_name, name, namelen + 1);
                if (copy_to_user(dirp_u + written, rec, reclen) < 0) return (uint64_t)-EFAULT;
                written += reclen;
            }
            idx++;
        }
        e->pos = idx;
        return written;
    }

    {
        int is_dev_dir = 0;
        if (strcmp(e->path, "/dev") == 0) {
            is_dev_dir = 1;
        } else {
            vfs_inode_t dev_ino;
            if (vfs_resolve("/dev", &dev_ino, 0, 0, 0) == 0 &&
                dev_ino.ino == e->inode.ino &&
                (dev_ino.mode & 0xF000) == VFS_INODE_DIR) {
                is_dev_dir = 1;
            }
        }
        if (!is_dev_dir) {
            /* fall through to filesystem-backed readdir */
        } else {
        static const char *k_chr[] = {
            "console", "tty", "tty0", "tty1", "tty2", "tty3", "tty4",
            "null", "zero", "fb0", "random", "urandom", "ptmx", "pts"
        };
        uint64_t idx = e->pos;
        uint64_t total = 2 + (uint64_t)block_count() + (uint64_t)(sizeof(k_chr) / sizeof(k_chr[0]));
        while (idx < total) {
            const char *name;
            uint8_t dtype;
            if (idx == 0) {
                name = ".";
                dtype = LINUX_DT_DIR;
            } else if (idx == 1) {
                name = "..";
                dtype = LINUX_DT_DIR;
            } else if (idx < (uint64_t)(2 + block_count())) {
                int bi = (int)(idx - 2);
                block_device_t *b = block_get(bi);
                if (!b || !b->present) {
                    idx++;
                    continue;
                }
                name = b->name;
                dtype = LINUX_DT_BLK;
            } else {
                uint64_t ci = idx - (uint64_t)(2 + block_count());
                if (ci >= (uint64_t)(sizeof(k_chr) / sizeof(k_chr[0]))) break;
                name = k_chr[ci];
                dtype = LINUX_DT_CHR;
            }

            uint32_t namelen = (uint32_t)strlen(name);
            uint16_t reclen = (uint16_t)((d64_base + namelen + 1u + 7u) & ~7u);
            if (written + reclen > count) break;

            uint8_t rec[320];
            struct edge_linux_dirent64 *d = (struct edge_linux_dirent64 *)rec;
            if (reclen > sizeof(rec)) return (uint64_t)-EINVAL;
            memset(rec, 0, reclen);
            d->d_ino = (idx < 2) ? (uint64_t)2 : (uint64_t)(0xD0000000u + idx);
            d->d_off = (int64_t)(idx + 1);
            d->d_reclen = reclen;
            d->d_type = dtype;
            memcpy(d->d_name, name, namelen + 1);

            if (copy_to_user(dirp_u + written, rec, reclen) < 0) return (uint64_t)-EFAULT;
            written += reclen;
            idx++;
        }
        e->pos = idx;
        return written;
        }
    }

    if (!e->sb || !e->sb->ops || !e->sb->ops->readdir) return (uint64_t)-ENOSYS;

    while (1) {
        char name[VFS_NAME_MAX];
        vfs_inode_t ino;
        uint32_t idx = (uint32_t)e->pos;
        memset(name, 0, sizeof(name));
        if (e->sb->ops->readdir(e->sb, &e->inode, idx, name, &ino) < 0) break;

        uint32_t namelen = (uint32_t)strlen(name);
        uint16_t reclen = (uint16_t)((d64_base + namelen + 1u + 7u) & ~7u);
        if (written + reclen > count) break;

        uint8_t rec[320];
        struct edge_linux_dirent64 *d = (struct edge_linux_dirent64 *)rec;
        if (reclen > sizeof(rec)) return (uint64_t)-EINVAL;
        memset(rec, 0, reclen);
        d->d_ino = ino.ino;
        d->d_off = (int64_t)(idx + 1);
        d->d_reclen = reclen;
        d->d_type = (uint8_t)mode_to_dtype(ino.mode);
        memcpy(d->d_name, name, namelen + 1);

        if (copy_to_user(dirp_u + written, rec, reclen) < 0) return (uint64_t)-EFAULT;
        written += reclen;
        e->pos++;
    }

    return written;
}

static edge_socket_t *socket_from_fd(int fd) {
    edge_fd_proc_t *p = fd_proc_with_stdio();
    edge_fd_t *e = fd_get(p, fd);
    if (!e) return 0;
    if (e->kind != FD_SOCKET) return 0;
    if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_SOCKETS) return 0;
    if (!g_sockets[e->pipe_id].used) return 0;
    return &g_sockets[e->pipe_id];
}

static int ifreq_get_name(char *name_out, uint64_t arg_u, struct edge_linux_ifreq *ifr) {
    if (!arg_u || !name_out || !ifr) return -1;
    if (copy_from_user(ifr, arg_u, sizeof(*ifr)) < 0) return -1;
    memcpy(name_out, ifr->ifr_name, 16);
    name_out[15] = 0;
    return 0;
}

static uint64_t net_ioctl_socket(uint32_t cmd, uint64_t arg_u) {
    struct edge_linux_ifreq ifr;
    char ifname[16];
    edge_netif_t *nif = 0;

    if (cmd == LINUX_SIOCGIFCONF) {
        struct edge_linux_ifconf ifc;
        struct edge_linux_ifreq out[2];
        uint32_t want = 2u * (uint32_t)sizeof(struct edge_linux_ifreq);
        if (!arg_u) return (uint64_t)-EINVAL;
        if (copy_from_user(&ifc, arg_u, sizeof(ifc)) < 0) return (uint64_t)-EFAULT;
        memset(out, 0, sizeof(out));
        memcpy(out[0].ifr_name, g_if_lo.name, strlen(g_if_lo.name));
        out[0].ifr_ifru.ifru_addr.sa_family = LINUX_AF_INET;
        memcpy(out[1].ifr_name, g_if_eth0.name, strlen(g_if_eth0.name));
        out[1].ifr_ifru.ifru_addr.sa_family = LINUX_AF_INET;
        if (ifc.ifc_buf) {
            uint32_t n = (ifc.ifc_len < (int32_t)want) ? (uint32_t)ifc.ifc_len : want;
            if (n > 0 && copy_to_user(ifc.ifc_buf, out, n) < 0) return (uint64_t)-EFAULT;
        }
        ifc.ifc_len = (int32_t)want;
        if (copy_to_user(arg_u, &ifc, sizeof(ifc)) < 0) return (uint64_t)-EFAULT;
        return 0;
    }

    if (ifreq_get_name(ifname, arg_u, &ifr) < 0) return (uint64_t)-EFAULT;
    nif = netif_by_name(ifname);
    if (!nif) return (uint64_t)-EADDRNOTAVAIL;

    switch (cmd) {
        case LINUX_SIOCGIFINDEX:
            ifr.ifr_ifru.ifru_ifindex = nif->ifindex;
            break;
        case LINUX_SIOCGIFFLAGS:
            ifr.ifr_ifru.ifru_flags = (int32_t)nif->flags;
            break;
        case LINUX_SIOCSIFFLAGS:
            nif->flags = (uint32_t)ifr.ifr_ifru.ifru_flags;
            nif->up = (nif->flags & LINUX_IFF_UP) != 0;
            break;
        case LINUX_SIOCGIFMTU:
            ifr.ifr_ifru.ifru_mtu = (int32_t)nif->mtu;
            break;
        case LINUX_SIOCSIFMTU:
            if (ifr.ifr_ifru.ifru_mtu > 0) nif->mtu = (uint32_t)ifr.ifr_ifru.ifru_mtu;
            break;
        case LINUX_SIOCGIFMETRIC:
            ifr.ifr_ifru.ifru_ivalue = 0;
            break;
        case LINUX_SIOCGIFTXQLEN:
            ifr.ifr_ifru.ifru_qlen = 1000;
            break;
        case LINUX_SIOCSIFTXQLEN:
            break;
        case LINUX_SIOCGIFHWADDR:
            memset(&ifr.ifr_ifru.ifru_hwaddr, 0, sizeof(ifr.ifr_ifru.ifru_hwaddr));
            ifr.ifr_ifru.ifru_hwaddr.sa_family = (strcmp(nif->name, "lo") == 0)
                ? LINUX_ARPHRD_LOOPBACK : LINUX_ARPHRD_ETHER;
            memcpy(ifr.ifr_ifru.ifru_hwaddr.sa_data, nif->mac, 6);
            break;
        case LINUX_SIOCGIFADDR:
            memset(&ifr.ifr_ifru.ifru_addr, 0, sizeof(ifr.ifr_ifru.ifru_addr));
            ifr.ifr_ifru.ifru_addr.sa_family = LINUX_AF_INET;
            memcpy(&ifr.ifr_ifru.ifru_addr.sa_data[2], &nif->ipv4_addr_be, sizeof(uint32_t));
            break;
        case LINUX_SIOCSIFADDR:
            memcpy(&nif->ipv4_addr_be, &ifr.ifr_ifru.ifru_addr.sa_data[2], sizeof(uint32_t));
            break;
        case LINUX_SIOCGIFNETMASK:
            memset(&ifr.ifr_ifru.ifru_netmask, 0, sizeof(ifr.ifr_ifru.ifru_netmask));
            ifr.ifr_ifru.ifru_netmask.sa_family = LINUX_AF_INET;
            memcpy(&ifr.ifr_ifru.ifru_netmask.sa_data[2], &nif->ipv4_netmask_be, sizeof(uint32_t));
            break;
        case LINUX_SIOCSIFNETMASK:
            memcpy(&nif->ipv4_netmask_be, &ifr.ifr_ifru.ifru_netmask.sa_data[2], sizeof(uint32_t));
            break;
        case LINUX_SIOCGIFBRDADDR:
            memset(&ifr.ifr_ifru.ifru_broadaddr, 0, sizeof(ifr.ifr_ifru.ifru_broadaddr));
            ifr.ifr_ifru.ifru_broadaddr.sa_family = LINUX_AF_INET;
            memcpy(&ifr.ifr_ifru.ifru_broadaddr.sa_data[2], &nif->ipv4_bcast_be, sizeof(uint32_t));
            break;
        case LINUX_SIOCSIFBRDADDR:
            memcpy(&nif->ipv4_bcast_be, &ifr.ifr_ifru.ifru_broadaddr.sa_data[2], sizeof(uint32_t));
            break;
        case LINUX_SIOCGIFDSTADDR:
            memset(&ifr.ifr_ifru.ifru_dstaddr, 0, sizeof(ifr.ifr_ifru.ifru_dstaddr));
            ifr.ifr_ifru.ifru_dstaddr.sa_family = LINUX_AF_INET;
            memcpy(&ifr.ifr_ifru.ifru_dstaddr.sa_data[2], &nif->ipv4_dst_be, sizeof(uint32_t));
            break;
        case LINUX_SIOCSIFDSTADDR:
            memcpy(&nif->ipv4_dst_be, &ifr.ifr_ifru.ifru_dstaddr.sa_data[2], sizeof(uint32_t));
            break;
        default:
            return (uint64_t)-ENOTTY;
    }

    if (copy_to_user(arg_u, &ifr, sizeof(ifr)) < 0) return (uint64_t)-EFAULT;
    return 0;
}

static uint64_t do_sys_socket(uint64_t domain_u, uint64_t type_u, uint64_t protocol_u) {
    int domain = (int)domain_u;
    int type = (int)type_u;
    int protocol = (int)protocol_u;
    int cloexec = type & LINUX_SOCK_CLOEXEC;
    int nonblock = type & LINUX_SOCK_NONBLOCK;
    type &= 0xFF;

    if (!(domain == LINUX_AF_UNIX || domain == LINUX_AF_INET || domain == LINUX_AF_INET6))
        return (uint64_t)-EAFNOSUPPORT;
    if (!(type == LINUX_SOCK_STREAM || type == LINUX_SOCK_DGRAM || type == LINUX_SOCK_RAW)) return (uint64_t)-EPROTONOSUPPORT;
    if (domain == LINUX_AF_INET && type == LINUX_SOCK_RAW &&
        !(protocol == 0 || protocol == LINUX_IPPROTO_ICMP)) return (uint64_t)-EPROTONOSUPPORT;
    if (domain == LINUX_AF_INET && type == LINUX_SOCK_DGRAM &&
        !(protocol == 0 || protocol == LINUX_IPPROTO_UDP || protocol == LINUX_IPPROTO_ICMP)) return (uint64_t)-EPROTONOSUPPORT;
    if (domain == LINUX_AF_INET && type == LINUX_SOCK_STREAM &&
        !(protocol == 0 || protocol == LINUX_IPPROTO_TCP)) return (uint64_t)-EPROTONOSUPPORT;
    if (domain == LINUX_AF_INET6 && type == LINUX_SOCK_RAW &&
        !(protocol == 0 || protocol == LINUX_IPPROTO_ICMPV6)) return (uint64_t)-EPROTONOSUPPORT;
    if (domain == LINUX_AF_INET6 && type == LINUX_SOCK_DGRAM &&
        !(protocol == 0 || protocol == LINUX_IPPROTO_UDP || protocol == LINUX_IPPROTO_ICMPV6)) return (uint64_t)-EPROTONOSUPPORT;
    if (domain == LINUX_AF_INET6 && type == LINUX_SOCK_STREAM &&
        !(protocol == 0 || protocol == LINUX_IPPROTO_TCP)) return (uint64_t)-EPROTONOSUPPORT;

    edge_fd_proc_t *p = fd_proc_with_stdio();
    if (!p) return (uint64_t)-ENOMEM;
    int fd = fd_alloc(p, 0);
    if (fd < 0) return (uint64_t)-ENOMEM;
    int sid = socket_alloc();
    if (sid < 0) {
        memset(&p->fds[fd], 0, sizeof(p->fds[fd]));
        return (uint64_t)-ENOMEM;
    }

    edge_socket_t *s = &g_sockets[sid];
    if ((domain == LINUX_AF_INET || domain == LINUX_AF_INET6) &&
        type == LINUX_SOCK_DGRAM &&
        (protocol == 0 || protocol == LINUX_IPPROTO_UDP)) {
        struct udp_pcb *up = udp_new_ip_type(domain == LINUX_AF_INET6 ? IPADDR_TYPE_V6 : IPADDR_TYPE_V4);
        if (!up) {
            memset(&p->fds[fd], 0, sizeof(p->fds[fd]));
            memset(s, 0, sizeof(*s));
            return (uint64_t)((domain == LINUX_AF_INET6) ? -EAFNOSUPPORT : -ENOMEM);
        }
        udp_recv(up, edge_udp_recv_cb, s);
        s->lwip_pcb = up;
    } else if ((domain == LINUX_AF_INET || domain == LINUX_AF_INET6) &&
               type == LINUX_SOCK_STREAM &&
               (protocol == 0 || protocol == LINUX_IPPROTO_TCP)) {
        struct tcp_pcb *tp = tcp_new_ip_type(domain == LINUX_AF_INET6 ? IPADDR_TYPE_V6 : IPADDR_TYPE_V4);
        if (!tp) {
            memset(&p->fds[fd], 0, sizeof(p->fds[fd]));
            memset(s, 0, sizeof(*s));
            return (uint64_t)((domain == LINUX_AF_INET6) ? -EAFNOSUPPORT : -ENOMEM);
        }
        tcp_arg(tp, s);
        tcp_recv(tp, edge_tcp_recv_cb);
        tcp_err(tp, edge_tcp_err_cb);
        s->lwip_pcb = tp;
    }
    s->domain = domain;
    s->type = type;
    s->protocol = protocol;
    s->nonblock = nonblock ? 1 : 0;
    s->ip_ttl = 64;
    s->unix_peer_id = -1;
    p->fds[fd].kind = FD_SOCKET;
    p->fds[fd].file_ref = file_ref_alloc();
    if (!p->fds[fd].file_ref) {
        socket_drop_ref(sid);
        memset(&p->fds[fd], 0, sizeof(p->fds[fd]));
        return (uint64_t)-ENOMEM;
    }
    p->fds[fd].pipe_id = sid;
    p->fds[fd].flags = LINUX_O_RDWR | (nonblock ? LINUX_O_NONBLOCK : 0);
    p->fds[fd].fd_flags = cloexec ? LINUX_FD_CLOEXEC : 0;
    return (uint64_t)fd;
}

static uint64_t do_sys_socketpair(uint64_t domain_u, uint64_t type_u, uint64_t protocol_u, uint64_t sv_u) {
    int domain = (int)domain_u;
    int type = (int)type_u;
    int protocol = (int)protocol_u;
    int cloexec = type & LINUX_SOCK_CLOEXEC;
    int nonblock = type & LINUX_SOCK_NONBLOCK;
    int sv[2];
    edge_fd_proc_t *p;
    int fd0, fd1, sid0, sid1;
    edge_socket_t *s0, *s1;

    type &= 0xFF;
    if (!sv_u) return (uint64_t)-EINVAL;
    if (domain != LINUX_AF_UNIX) return (uint64_t)-EAFNOSUPPORT;
    if (type != LINUX_SOCK_STREAM) return (uint64_t)-EOPNOTSUPP;
    if (protocol != 0) return (uint64_t)-EPROTONOSUPPORT;

    p = fd_proc_with_stdio();
    if (!p) return (uint64_t)-ENOMEM;
    fd0 = fd_alloc(p, 0);
    if (fd0 < 0) return (uint64_t)-ENOMEM;
    fd1 = fd_alloc(p, 0);
    if (fd1 < 0) {
        memset(&p->fds[fd0], 0, sizeof(p->fds[fd0]));
        return (uint64_t)-ENOMEM;
    }
    sid0 = socket_alloc();
    if (sid0 < 0) {
        memset(&p->fds[fd0], 0, sizeof(p->fds[fd0]));
        memset(&p->fds[fd1], 0, sizeof(p->fds[fd1]));
        return (uint64_t)-ENOMEM;
    }
    sid1 = socket_alloc();
    if (sid1 < 0) {
        memset(&g_sockets[sid0], 0, sizeof(g_sockets[sid0]));
        memset(&p->fds[fd0], 0, sizeof(p->fds[fd0]));
        memset(&p->fds[fd1], 0, sizeof(p->fds[fd1]));
        return (uint64_t)-ENOMEM;
    }

    s0 = &g_sockets[sid0];
    s1 = &g_sockets[sid1];
    s0->domain = LINUX_AF_UNIX;
    s0->type = LINUX_SOCK_STREAM;
    s0->protocol = 0;
    s0->nonblock = nonblock ? 1 : 0;
    s0->connected = 1;
    s0->ip_ttl = 64;
    s0->unix_peer_id = sid1;
    s1->domain = LINUX_AF_UNIX;
    s1->type = LINUX_SOCK_STREAM;
    s1->protocol = 0;
    s1->nonblock = nonblock ? 1 : 0;
    s1->connected = 1;
    s1->ip_ttl = 64;
    s1->unix_peer_id = sid0;

    p->fds[fd0].kind = FD_SOCKET;
    p->fds[fd0].file_ref = file_ref_alloc();
    if (!p->fds[fd0].file_ref) {
        socket_drop_ref(sid0);
        socket_drop_ref(sid1);
        memset(&p->fds[fd0], 0, sizeof(p->fds[fd0]));
        memset(&p->fds[fd1], 0, sizeof(p->fds[fd1]));
        return (uint64_t)-ENOMEM;
    }
    p->fds[fd0].pipe_id = sid0;
    p->fds[fd0].flags = LINUX_O_RDWR | (nonblock ? LINUX_O_NONBLOCK : 0);
    p->fds[fd0].fd_flags = cloexec ? LINUX_FD_CLOEXEC : 0;
    p->fds[fd1].kind = FD_SOCKET;
    p->fds[fd1].file_ref = file_ref_alloc();
    if (!p->fds[fd1].file_ref) {
        (void)file_ref_put(p->fds[fd0].file_ref);
        socket_drop_ref(sid0);
        socket_drop_ref(sid1);
        memset(&p->fds[fd0], 0, sizeof(p->fds[fd0]));
        memset(&p->fds[fd1], 0, sizeof(p->fds[fd1]));
        return (uint64_t)-ENOMEM;
    }
    p->fds[fd1].pipe_id = sid1;
    p->fds[fd1].flags = LINUX_O_RDWR | (nonblock ? LINUX_O_NONBLOCK : 0);
    p->fds[fd1].fd_flags = cloexec ? LINUX_FD_CLOEXEC : 0;

    sv[0] = fd0;
    sv[1] = fd1;
    if (copy_to_user(sv_u, sv, sizeof(sv)) < 0) {
        (void)file_ref_put(p->fds[fd0].file_ref);
        (void)file_ref_put(p->fds[fd1].file_ref);
        socket_drop_ref(sid0);
        socket_drop_ref(sid1);
        memset(&p->fds[fd0], 0, sizeof(p->fds[fd0]));
        memset(&p->fds[fd1], 0, sizeof(p->fds[fd1]));
        return (uint64_t)-EFAULT;
    }
    return 0;
}

static uint64_t do_sys_bind(uint64_t fd_u, uint64_t addr_u, uint64_t len_u) {
    int fd = (int)fd_u;
    edge_socket_t *s = socket_from_fd(fd);
    uint32_t len = (uint32_t)len_u;
    struct edge_sockaddr_in sin;
    struct edge_sockaddr_in6 sin6;
    if (!s) return (uint64_t)-ENOTSOCK;
    if (!addr_u || len == 0 || len > sizeof(s->bind_addr)) return (uint64_t)-EINVAL;
    if (s->domain == LINUX_AF_INET && len >= sizeof(sin)) {
        if (copy_from_user(&sin, addr_u, sizeof(sin)) < 0) return (uint64_t)-EFAULT;
        if (sin.sin_family != LINUX_AF_INET) return (uint64_t)-EAFNOSUPPORT;
        if (sin.sin_port == 0) sin.sin_port = socket_alloc_ephemeral_port_be();
        if (s->type == LINUX_SOCK_DGRAM && s->lwip_pcb) {
            struct udp_pcb *up = (struct udp_pcb *)s->lwip_pcb;
            ip_addr_t ip;
            ip_addr_set_zero_ip4(&ip);
            ip_2_ip4(&ip)->addr = sin.sin_addr;
            if (udp_bind(up, &ip, edge_bswap16(sin.sin_port)) != ERR_OK) return (uint64_t)-EADDRNOTAVAIL;
            s->local_port_be = sin.sin_port;
        } else if (s->type == LINUX_SOCK_STREAM && s->lwip_pcb) {
            struct tcp_pcb *tp = (struct tcp_pcb *)s->lwip_pcb;
            ip_addr_t ip;
            ip_addr_set_zero_ip4(&ip);
            ip_2_ip4(&ip)->addr = sin.sin_addr;
            if (tcp_bind(tp, &ip, edge_bswap16(sin.sin_port)) != ERR_OK) return (uint64_t)-EADDRNOTAVAIL;
            s->local_port_be = sin.sin_port;
        }
        socket_set_bind_inet(s, sin.sin_addr, sin.sin_port);
    } else if (s->domain == LINUX_AF_INET6 && len >= sizeof(sin6)) {
        if (copy_from_user(&sin6, addr_u, sizeof(sin6)) < 0) return (uint64_t)-EFAULT;
        if (sin6.sin6_family != LINUX_AF_INET6) return (uint64_t)-EAFNOSUPPORT;
        if (sin6.sin6_port == 0) sin6.sin6_port = socket_alloc_ephemeral_port_be();
        if (s->type == LINUX_SOCK_DGRAM && s->lwip_pcb) {
            struct udp_pcb *up = (struct udp_pcb *)s->lwip_pcb;
            ip_addr_t ip6;
            ip_addr_set_zero_ip6(&ip6);
            memcpy(&ip_2_ip6(&ip6)->addr[0], sin6.sin6_addr, 16);
            if (udp_bind(up, &ip6, edge_bswap16(sin6.sin6_port)) != ERR_OK) return (uint64_t)-EADDRNOTAVAIL;
            s->local_port_be = sin6.sin6_port;
        } else if (s->type == LINUX_SOCK_STREAM && s->lwip_pcb) {
            struct tcp_pcb *tp = (struct tcp_pcb *)s->lwip_pcb;
            ip_addr_t ip6;
            ip_addr_set_zero_ip6(&ip6);
            memcpy(&ip_2_ip6(&ip6)->addr[0], sin6.sin6_addr, 16);
            if (tcp_bind(tp, &ip6, edge_bswap16(sin6.sin6_port)) != ERR_OK) return (uint64_t)-EADDRNOTAVAIL;
            s->local_port_be = sin6.sin6_port;
        }
        socket_set_bind_inet6(s, sin6.sin6_addr, sin6.sin6_port, sin6.sin6_scope_id);
    } else {
        if (copy_from_user(s->bind_addr, addr_u, len) < 0) return (uint64_t)-EFAULT;
        s->bind_len = len;
    }
    return 0;
}

static uint64_t do_sys_connect(uint64_t fd_u, uint64_t addr_u, uint64_t len_u) {
    int fd = (int)fd_u;
    edge_fd_proc_t *p = fd_proc_with_stdio();
    edge_fd_t *fde = fd_get(p, fd);
    edge_socket_t *s = socket_from_fd(fd);
    uint32_t len = (uint32_t)len_u;
    uint16_t fam = 0;
    if (!s) return (uint64_t)-ENOTSOCK;
    if (!addr_u || len == 0 || len > sizeof(s->peer_addr)) return (uint64_t)-EINVAL;
    if (copy_from_user(&fam, addr_u, sizeof(fam)) < 0) return (uint64_t)-EFAULT;

    if (s->domain == LINUX_AF_UNIX) {
        if (s->type != LINUX_SOCK_STREAM) return (uint64_t)-EPROTONOSUPPORT;
        /* Only socketpair-backed AF_UNIX streams are supported right now. */
        if (s->unix_peer_id >= 0 && s->unix_peer_id < EDGE_MAX_SOCKETS) {
            if (!g_sockets[s->unix_peer_id].used) return (uint64_t)-ECONNREFUSED;
            s->connected = 1;
            return 0;
        }
        if (fam != (uint16_t)LINUX_AF_UNIX) return (uint64_t)-EAFNOSUPPORT;
        return (uint64_t)-ENOENT;
    }

    if (s->domain == LINUX_AF_INET) socket_autobind_inet(s);
    if (s->domain == LINUX_AF_INET6) socket_autobind_inet6(s);
    if (copy_from_user(s->peer_addr, addr_u, len) < 0) return (uint64_t)-EFAULT;
    if (s->domain == LINUX_AF_INET && len >= sizeof(struct edge_sockaddr_in)) {
        struct edge_sockaddr_in sin;
        if (sockaddr_in_from_buf(s->peer_addr, len, &sin) < 0) return (uint64_t)-EAFNOSUPPORT;
        if (s->type == LINUX_SOCK_DGRAM &&
            (s->protocol == 0 || s->protocol == LINUX_IPPROTO_UDP) &&
            s->lwip_pcb) {
            struct udp_pcb *up = (struct udp_pcb *)s->lwip_pcb;
            ip_addr_t dst;
            ip_addr_set_zero_ip4(&dst);
            ip_2_ip4(&dst)->addr = sin.sin_addr;
            if (udp_connect(up, &dst, edge_bswap16(sin.sin_port)) != ERR_OK) return (uint64_t)-EHOSTUNREACH;
        } else if (s->type == LINUX_SOCK_STREAM &&
                   (s->protocol == 0 || s->protocol == LINUX_IPPROTO_TCP) &&
                   s->lwip_pcb) {
            struct tcp_pcb *tp = (struct tcp_pcb *)s->lwip_pcb;
            ip_addr_t dst;
            uint64_t start_us = boottime_monotonic_us();
            ip_addr_set_zero_ip4(&dst);
            ip_2_ip4(&dst)->addr = sin.sin_addr;
            s->connect_in_progress = 1;
            s->connect_error = 0;
            if (tcp_connect(tp, &dst, edge_bswap16(sin.sin_port), edge_tcp_connected_cb) != ERR_OK) {
                s->connect_in_progress = 0;
                return (uint64_t)-ECONNREFUSED;
            }
            while (s->connect_in_progress) {
                if (signal_pending_interrupt()) return tty_interrupt_current_ret();
                if ((fde && (fde->flags & LINUX_O_NONBLOCK)) || s->nonblock) return (uint64_t)-EINPROGRESS;
                if (s->recv_timeout_us > 0 && boottime_monotonic_us() - start_us >= s->recv_timeout_us) return (uint64_t)-EAGAIN;
                lwip_stack_poll();
                wait_blocking_step();
            }
            if (s->connect_error != 0 || !s->connected) return (uint64_t)-ECONNREFUSED;
            if (s->bind_len < sizeof(struct edge_sockaddr_in)) {
                struct edge_sockaddr_in lsin;
                memset(&lsin, 0, sizeof(lsin));
                lsin.sin_family = LINUX_AF_INET;
                lsin.sin_addr = g_if_eth0.ipv4_addr_be;
                lsin.sin_port = edge_bswap16(tp->local_port);
                socket_set_bind_inet(s, lsin.sin_addr, lsin.sin_port);
                s->local_port_be = lsin.sin_port;
            }
        }
    } else if (s->domain == LINUX_AF_INET6 && len >= sizeof(struct edge_sockaddr_in6)) {
        struct edge_sockaddr_in6 sin6;
        if (sockaddr_in6_from_buf(s->peer_addr, len, &sin6) < 0) return (uint64_t)-EAFNOSUPPORT;
        /* EdgeOS currently exposes IPv6 APIs but does not have reliable routed
         * IPv6 connectivity in this configuration (typically only link-local).
         * Fail fast for non-local IPv6 destinations so libc/apps fall back to
         * IPv4 instead of hanging until timeout and surfacing EINTR. */
        {
            const uint8_t *a = sin6.sin6_addr;
            int is_loopback =
                a[0] == 0 && a[1] == 0 && a[2] == 0 && a[3] == 0 &&
                a[4] == 0 && a[5] == 0 && a[6] == 0 && a[7] == 0 &&
                a[8] == 0 && a[9] == 0 && a[10] == 0 && a[11] == 0 &&
                a[12] == 0 && a[13] == 0 && a[14] == 0 && a[15] == 1;
            int is_link_local = (a[0] == 0xfe) && ((a[1] & 0xc0) == 0x80);
            if (!is_loopback && !is_link_local) return (uint64_t)-ENETUNREACH;
        }
        if (s->type == LINUX_SOCK_DGRAM &&
            (s->protocol == 0 || s->protocol == LINUX_IPPROTO_UDP) &&
            s->lwip_pcb) {
            struct udp_pcb *up = (struct udp_pcb *)s->lwip_pcb;
            ip_addr_t dst6;
            ip_addr_set_zero_ip6(&dst6);
            memcpy(&ip_2_ip6(&dst6)->addr[0], sin6.sin6_addr, 16);
            if (udp_connect(up, &dst6, edge_bswap16(sin6.sin6_port)) != ERR_OK) return (uint64_t)-EHOSTUNREACH;
        } else if (s->type == LINUX_SOCK_STREAM &&
                   (s->protocol == 0 || s->protocol == LINUX_IPPROTO_TCP) &&
                   s->lwip_pcb) {
            struct tcp_pcb *tp = (struct tcp_pcb *)s->lwip_pcb;
            ip_addr_t dst6;
            uint64_t start_us = boottime_monotonic_us();
            ip_addr_set_zero_ip6(&dst6);
            memcpy(&ip_2_ip6(&dst6)->addr[0], sin6.sin6_addr, 16);
            s->connect_in_progress = 1;
            s->connect_error = 0;
            if (tcp_connect(tp, &dst6, edge_bswap16(sin6.sin6_port), edge_tcp_connected_cb) != ERR_OK) {
                s->connect_in_progress = 0;
                return (uint64_t)-ECONNREFUSED;
            }
            while (s->connect_in_progress) {
                if (signal_pending_interrupt()) return tty_interrupt_current_ret();
                if ((fde && (fde->flags & LINUX_O_NONBLOCK)) || s->nonblock) return (uint64_t)-EINPROGRESS;
                if (s->recv_timeout_us > 0 && boottime_monotonic_us() - start_us >= s->recv_timeout_us) return (uint64_t)-EAGAIN;
                lwip_stack_poll();
                wait_blocking_step();
            }
            if (s->connect_error != 0 || !s->connected) return (uint64_t)-ECONNREFUSED;
            if (s->bind_len < sizeof(struct edge_sockaddr_in6)) {
                struct edge_sockaddr_in6 lsin6;
                memset(&lsin6, 0, sizeof(lsin6));
                lsin6.sin6_family = LINUX_AF_INET6;
                edge_ip6_to_bytes(ip_2_ip6(&tp->local_ip), lsin6.sin6_addr);
                lsin6.sin6_port = edge_bswap16(tp->local_port);
                socket_set_bind_inet6(s, lsin6.sin6_addr, lsin6.sin6_port, 0);
                s->local_port_be = lsin6.sin6_port;
            }
        }
    }
    s->peer_len = len;
    s->connected = 1;
    return 0;
}

static uint64_t do_sys_listen(uint64_t fd_u, uint64_t backlog_u) {
    int fd = (int)fd_u;
    int backlog = (int)backlog_u;
    edge_socket_t *s = socket_from_fd(fd);
    struct tcp_pcb *tp;
    struct tcp_pcb *lp;
    err_t lerr = ERR_OK;

    if (!s) return (uint64_t)-ENOTSOCK;
    if (!(s->domain == LINUX_AF_INET || s->domain == LINUX_AF_INET6)) return (uint64_t)-EAFNOSUPPORT;
    if (s->type != LINUX_SOCK_STREAM) return (uint64_t)-EOPNOTSUPP;
    if (!s->lwip_pcb) return (uint64_t)-EINVAL;
    if (s->listening) return 0;

    if (backlog < 1) backlog = 1;
    if (backlog > EDGE_SOCK_ACCEPTQ) backlog = EDGE_SOCK_ACCEPTQ;

    tp = (struct tcp_pcb *)s->lwip_pcb;
    lp = tcp_listen_with_backlog_and_err(tp, (u8_t)backlog, &lerr);
    if (!lp || lerr != ERR_OK) return (uint64_t)-EADDRINUSE;

    s->lwip_pcb = lp;
    s->listening = 1;
    s->backlog = backlog;
    s->pending_head = 0;
    s->pending_tail = 0;
    s->pending_count = 0;
    tcp_arg(lp, s);
    tcp_accept(lp, edge_tcp_accept_cb);
    return 0;
}

static uint64_t do_sys_accept(uint64_t fd_u, uint64_t addr_u, uint64_t len_u) {
    int fd = (int)fd_u;
    edge_socket_t *listener = socket_from_fd(fd);
    edge_fd_proc_t *p;
    edge_fd_t *e;
    int sid;
    edge_socket_t *child;
    int nfd;
    uint32_t outlen;

    if (!listener) return (uint64_t)-ENOTSOCK;
    if (!listener->listening) return (uint64_t)-EINVAL;

    p = fd_proc_with_stdio();
    e = fd_get(p, fd);
    if (!p || !e) return (uint64_t)-EBADF;

    while (listener->pending_count == 0) {
        if ((e->flags & LINUX_O_NONBLOCK) || listener->nonblock) return (uint64_t)-EAGAIN;
        if (signal_pending_interrupt()) return tty_interrupt_current_ret();
        lwip_stack_poll();
        wait_blocking_step();
    }

    sid = socket_pending_dequeue(listener);
    if (sid < 0 || sid >= EDGE_MAX_SOCKETS || !g_sockets[sid].used) return (uint64_t)-EAGAIN;
    child = &g_sockets[sid];

    nfd = fd_alloc(p, 0);
    if (nfd < 0) {
        socket_drop_ref(sid);
        return (uint64_t)-EMFILE;
    }
    p->fds[nfd].kind = FD_SOCKET;
    p->fds[nfd].file_ref = file_ref_alloc();
    if (!p->fds[nfd].file_ref) {
        memset(&p->fds[nfd], 0, sizeof(p->fds[nfd]));
        socket_drop_ref(sid);
        return (uint64_t)-ENOMEM;
    }
    p->fds[nfd].pipe_id = sid;
    p->fds[nfd].flags = LINUX_O_RDWR;
    p->fds[nfd].fd_flags = 0;

    if (listener->lwip_pcb) {
        tcp_backlog_accepted((struct tcp_pcb *)listener->lwip_pcb);
    }

    if (addr_u && len_u) {
        outlen = 0;
        if (copy_from_user(&outlen, len_u, sizeof(outlen)) < 0) {
            (void)do_sys_close((uint64_t)nfd);
            return (uint64_t)-EFAULT;
        }
        if (outlen > child->rx_peer_len) outlen = child->rx_peer_len;
        if (outlen > 0 && copy_to_user(addr_u, child->rx_peer, outlen) < 0) {
            (void)do_sys_close((uint64_t)nfd);
            return (uint64_t)-EFAULT;
        }
        if (copy_to_user(len_u, &outlen, sizeof(outlen)) < 0) {
            (void)do_sys_close((uint64_t)nfd);
            return (uint64_t)-EFAULT;
        }
    }

    return (uint64_t)nfd;
}

static uint64_t do_sys_accept4(uint64_t fd_u, uint64_t addr_u, uint64_t len_u, uint64_t flags_u) {
    uint64_t ret;
    int nfd;
    int flags = (int)flags_u;
    edge_fd_proc_t *p;
    if ((flags & ~(LINUX_SOCK_NONBLOCK | LINUX_SOCK_CLOEXEC)) != 0) return (uint64_t)-EINVAL;
    ret = do_sys_accept(fd_u, addr_u, len_u);
    if ((int64_t)ret < 0) return ret;
    nfd = (int)ret;
    p = fd_proc_with_stdio();
    if (!p || nfd < 0 || nfd >= EDGE_MAX_FD || !p->fds[nfd].used) return (uint64_t)-EBADF;
    if ((flags & LINUX_SOCK_NONBLOCK) != 0 && p->fds[nfd].kind == FD_SOCKET) {
        int sid = p->fds[nfd].pipe_id;
        if (sid >= 0 && sid < EDGE_MAX_SOCKETS && g_sockets[sid].used) {
            p->fds[nfd].flags |= LINUX_O_NONBLOCK;
            g_sockets[sid].nonblock = 1;
        }
    }
    if ((flags & LINUX_SOCK_CLOEXEC) != 0) p->fds[nfd].fd_flags |= LINUX_FD_CLOEXEC;
    return ret;
}

static uint64_t do_sys_getrandom(uint64_t buf_u, uint64_t len_u, uint64_t flags_u) {
    uint64_t len = len_u;
    uint8_t tmp[256];
    uint64_t done = 0;
    (void)flags_u;
    if (!buf_u) return (uint64_t)-EINVAL;
    if (len == 0) return 0;
    while (done < len) {
        uint64_t n = len - done;
        if (n > sizeof(tmp)) n = sizeof(tmp);
        for (uint64_t i = 0; i < n; ++i) tmp[i] = kernel_random_byte();
        if (copy_to_user(buf_u + done, tmp, n) < 0) return (uint64_t)-EFAULT;
        done += n;
    }
    return done;
}

static uint64_t do_sys_setsockopt(uint64_t fd_u, uint64_t level_u, uint64_t name_u, uint64_t val_u, uint64_t len_u) {
    edge_socket_t *s = socket_from_fd((int)fd_u);
    if (!s) return (uint64_t)-ENOTSOCK;
    if ((int)level_u == LINUX_SOL_IP &&
        ((int)name_u == LINUX_IP_TTL || (int)name_u == LINUX_IP_MULTICAST_TTL)) {
        int v = 0;
        if (!val_u || len_u < sizeof(int)) return (uint64_t)-EINVAL;
        if (copy_from_user(&v, val_u, sizeof(v)) < 0) return (uint64_t)-EFAULT;
        if (v < 1 || v > 255) return (uint64_t)-EINVAL;
        s->ip_ttl = (uint8_t)v;
        return 0;
    }
    if ((int)level_u == LINUX_SOL_IPV6 && (int)name_u == LINUX_IPV6_HOPLIMIT) {
        int v = 0;
        if (!val_u || len_u < sizeof(int)) return (uint64_t)-EINVAL;
        if (copy_from_user(&v, val_u, sizeof(v)) < 0) return (uint64_t)-EFAULT;
        if (v < 1 || v > 255) return (uint64_t)-EINVAL;
        s->ip_ttl = (uint8_t)v;
        return 0;
    }
    if ((int)level_u == LINUX_SOL_IPV6 && (int)name_u == LINUX_IPV6_CHECKSUM) {
        return 0;
    }
    if ((int)level_u == LINUX_SOL_SOCKET && (int)name_u == LINUX_SO_RCVTIMEO) {
        struct edge_timeval tv;
        uint64_t sec_us;
        uint64_t usec;
        if (!val_u || len_u < sizeof(tv)) return (uint64_t)-EINVAL;
        if (copy_from_user(&tv, val_u, sizeof(tv)) < 0) return (uint64_t)-EFAULT;
        if (tv.tv_sec < 0 || tv.tv_usec < 0) return (uint64_t)-EINVAL;
        sec_us = (uint64_t)tv.tv_sec * 1000000ull;
        usec = (uint64_t)tv.tv_usec;
        if (usec >= 1000000ull) usec = usec % 1000000ull;
        s->recv_timeout_us = sec_us + usec;
        return 0;
    }
    return 0;
}

static uint64_t do_sys_getsockopt(uint64_t fd_u, uint64_t level_u, uint64_t name_u, uint64_t val_u, uint64_t len_u) {
    edge_socket_t *s = socket_from_fd((int)fd_u);
    uint32_t olen = 0;
    if (!s) return (uint64_t)-ENOTSOCK;
    if (!val_u || !len_u) return (uint64_t)-EINVAL;
    if (copy_from_user(&olen, len_u, sizeof(olen)) < 0) return (uint64_t)-EFAULT;
    if ((int)level_u == LINUX_SOL_SOCKET && (int)name_u == LINUX_SO_RCVTIMEO) {
        struct edge_timeval tv;
        memset(&tv, 0, sizeof(tv));
        tv.tv_sec = (int64_t)(s->recv_timeout_us / 1000000ull);
        tv.tv_usec = (int64_t)(s->recv_timeout_us % 1000000ull);
        if (olen < sizeof(tv)) return (uint64_t)-EINVAL;
        if (copy_to_user(val_u, &tv, sizeof(tv)) < 0) return (uint64_t)-EFAULT;
        olen = sizeof(tv);
        if (copy_to_user(len_u, &olen, sizeof(olen)) < 0) return (uint64_t)-EFAULT;
        return 0;
    }
    if ((int)level_u == LINUX_SOL_IPV6 && (int)name_u == LINUX_IPV6_HOPLIMIT) {
        if (olen < sizeof(int)) return (uint64_t)-EINVAL;
        {
            int v = (int)s->ip_ttl;
            if (copy_to_user(val_u, &v, sizeof(v)) < 0) return (uint64_t)-EFAULT;
            olen = sizeof(v);
        }
        if (copy_to_user(len_u, &olen, sizeof(olen)) < 0) return (uint64_t)-EFAULT;
        return 0;
    }
    if (olen >= sizeof(int)) {
        int v = 0;
        if ((int)level_u == LINUX_SOL_SOCKET && (int)name_u == LINUX_SO_ERROR) v = 0;
        if (copy_to_user(val_u, &v, sizeof(v)) < 0) return (uint64_t)-EFAULT;
        olen = sizeof(v);
    } else {
        olen = 0;
    }
    if (copy_to_user(len_u, &olen, sizeof(olen)) < 0) return (uint64_t)-EFAULT;
    return 0;
}

static uint64_t do_sys_sendto(uint64_t fd_u, uint64_t buf_u, uint64_t len_u, uint64_t flags_u, uint64_t addr_u, uint64_t addrlen_u) {
    (void)flags_u;
    int fd = (int)fd_u;
    edge_fd_proc_t *p = fd_proc_with_stdio();
    edge_fd_t *fde = fd_get(p, fd);
    edge_socket_t *s = socket_from_fd(fd);
    uint32_t len = (uint32_t)len_u;
    uint8_t tx[2048];
    uint8_t tcp_chunk[1024];
    uint8_t peer[28];
    uint32_t peer_len = 0;
    if (!s) return (uint64_t)-ENOTSOCK;
    if (!buf_u || len == 0) return (uint64_t)-EINVAL;

    if (s->domain == LINUX_AF_UNIX && s->type == LINUX_SOCK_STREAM) {
        uint32_t off = 0;
        while (off < len) {
            edge_socket_t *peer;
            uint32_t room;
            uint32_t n;
            if (s->unix_peer_id < 0 || s->unix_peer_id >= EDGE_MAX_SOCKETS) return (uint64_t)-EPIPE;
            peer = &g_sockets[s->unix_peer_id];
            if (!peer->used) return (uint64_t)-EPIPE;
            room = (uint32_t)(sizeof(peer->rx_buf) - peer->rx_len);
            if (room == 0) {
                if ((fde && (fde->flags & LINUX_O_NONBLOCK)) || s->nonblock) {
                    return off ? off : (uint64_t)-EAGAIN;
                }
                if (signal_pending_interrupt()) return tty_interrupt_current_ret();
                lwip_stack_poll();
                wait_blocking_step();
                continue;
            }
            n = len - off;
            if (n > room) n = room;
            {
                uint32_t done = 0;
                while (done < n) {
                    uint32_t chunk = n - done;
                    if (chunk > sizeof(tcp_chunk)) chunk = sizeof(tcp_chunk);
                    if (copy_from_user(tcp_chunk, buf_u + off + done, chunk) < 0) return (uint64_t)-EFAULT;
                    memcpy(peer->rx_buf + peer->rx_len + done, tcp_chunk, chunk);
                    done += chunk;
                }
            }
            peer->rx_len += n;
            off += n;
        }
        return off;
    }

    if (addr_u && addrlen_u > 0) {
        peer_len = (uint32_t)addrlen_u;
        if (peer_len > sizeof(peer)) return (uint64_t)-EINVAL;
        if (copy_from_user(peer, addr_u, peer_len) < 0) return (uint64_t)-EFAULT;
    } else if (s->connected) {
        peer_len = s->peer_len;
        memcpy(peer, s->peer_addr, peer_len);
    } else {
        return (uint64_t)-EADDRNOTAVAIL;
    }
    if (s->domain == LINUX_AF_INET) socket_autobind_inet(s);

    if ((s->domain == LINUX_AF_INET || s->domain == LINUX_AF_INET6) &&
        s->type == LINUX_SOCK_DGRAM &&
        (s->protocol == 0 || s->protocol == LINUX_IPPROTO_UDP) &&
        s->lwip_pcb) {
        struct edge_sockaddr_in sin;
        struct edge_sockaddr_in6 sin6;
        struct udp_pcb *up = (struct udp_pcb *)s->lwip_pcb;
        struct pbuf *p;
        ip_addr_t dst;
        if (s->domain == LINUX_AF_INET) {
            if (sockaddr_in_from_buf(peer, peer_len, &sin) < 0) return (uint64_t)-EAFNOSUPPORT;
            ip_addr_set_zero_ip4(&dst);
            ip_2_ip4(&dst)->addr = sin.sin_addr;
        } else {
            if (sockaddr_in6_from_buf(peer, peer_len, &sin6) < 0) return (uint64_t)-EAFNOSUPPORT;
            ip_addr_set_zero_ip6(&dst);
            memcpy(&ip_2_ip6(&dst)->addr[0], sin6.sin6_addr, 16);
        }
        if (len > sizeof(tx)) return (uint64_t)-EINVAL;
        if (copy_from_user(tx, buf_u, len) < 0) return (uint64_t)-EFAULT;
        p = pbuf_alloc(PBUF_TRANSPORT, (u16_t)len, PBUF_RAM);
        if (!p) return (uint64_t)-ENOMEM;
        if (pbuf_take(p, tx, (u16_t)len) != ERR_OK) {
            pbuf_free(p);
            return (uint64_t)-ENOMEM;
        }
        up->ttl = s->ip_ttl ? s->ip_ttl : 64;
        if (udp_sendto(up, p, &dst, edge_bswap16(s->domain == LINUX_AF_INET ? sin.sin_port : sin6.sin6_port)) != ERR_OK) {
            pbuf_free(p);
            return (uint64_t)-ENETUNREACH;
        }
        pbuf_free(p);
        return len;
    }

    if ((s->domain == LINUX_AF_INET || s->domain == LINUX_AF_INET6) &&
        s->type == LINUX_SOCK_STREAM &&
        (s->protocol == 0 || s->protocol == LINUX_IPPROTO_TCP) &&
        s->lwip_pcb) {
        struct tcp_pcb *tp = (struct tcp_pcb *)s->lwip_pcb;
        uint32_t off = 0;
        if (!s->connected) return (uint64_t)-ENOTCONN;
        while (off < len) {
            uint32_t copied = len - off;
            uint32_t in_off = 0;
            if (copied > sizeof(tcp_chunk)) copied = sizeof(tcp_chunk);
            if (copy_from_user(tcp_chunk, buf_u + off, copied) < 0) return (uint64_t)-EFAULT;
            while (in_off < copied) {
                uint16_t chunk = (uint16_t)((copied - in_off) > 1024 ? 1024 : (copied - in_off));
                err_t er = tcp_write(tp, tcp_chunk + in_off, chunk, TCP_WRITE_FLAG_COPY);
                if (er == ERR_MEM) {
                    if ((fde && (fde->flags & LINUX_O_NONBLOCK)) || s->nonblock) {
                        if (off == 0) return (uint64_t)-EAGAIN;
                        return off;
                    }
                    if (signal_pending_interrupt()) return tty_interrupt_current_ret();
                    lwip_stack_poll();
                    wait_blocking_step();
                    continue;
                }
                if (er != ERR_OK) return off ? off : (uint64_t)-EIO;
                in_off += chunk;
            }
            off += copied;
            (void)tcp_output(tp);
        }
        return off;
    }

    if (len > sizeof(tx)) return (uint64_t)-EINVAL;
    if (copy_from_user(tx, buf_u, len) < 0) return (uint64_t)-EFAULT;

    if (s->domain == LINUX_AF_INET &&
        (s->type == LINUX_SOCK_RAW || s->type == LINUX_SOCK_DGRAM) &&
        (s->protocol == 0 || s->protocol == LINUX_IPPROTO_ICMP)) {
        struct edge_sockaddr_in sin;
        uint32_t dst_ip_be;
        if (len < 8) return len;
        if (len > sizeof(s->ping_req)) len = sizeof(s->ping_req);
        memcpy(s->ping_req, tx, len);
        s->ping_req_len = len;
        memcpy(s->ping_peer, peer, peer_len);
        s->ping_peer_len = peer_len;
        memcpy(&s->ping_next_seq_be, &tx[6], sizeof(uint16_t));
        memcpy(&s->ping_id_be, &tx[4], sizeof(uint16_t));
        s->ping_hw = 0;
        if (peer_len >= sizeof(sin)) {
            memcpy(&sin, peer, sizeof(sin));
            if (sin.sin_family == LINUX_AF_INET) {
                dst_ip_be = sin.sin_addr;
                if (lwip_stack_is_ready() &&
                    lwip_stack_send_icmp_echo(dst_ip_be, tx, (uint16_t)len, s->ip_ttl) == 0) {
                    s->ping_hw = 1;
                    return len;
                }
            }
        }
        return (uint64_t)-ENETUNREACH;
    }

    if (s->domain == LINUX_AF_INET6 &&
        (s->type == LINUX_SOCK_RAW || s->type == LINUX_SOCK_DGRAM) &&
        (s->protocol == 0 || s->protocol == LINUX_IPPROTO_ICMPV6)) {
        struct edge_sockaddr_in6 sin6;
        if (len < 8) return len;
        if (len > sizeof(s->ping_req)) len = sizeof(s->ping_req);
        memcpy(s->ping_req, tx, len);
        s->ping_req_len = len;
        memcpy(s->ping_peer, peer, peer_len);
        s->ping_peer_len = peer_len;
        memcpy(&s->ping_next_seq_be, &tx[6], sizeof(uint16_t));
        memcpy(&s->ping_id_be, &tx[4], sizeof(uint16_t));
        s->ping_hw = 0;
        if (peer_len >= sizeof(sin6)) {
            memcpy(&sin6, peer, sizeof(sin6));
            if (sin6.sin6_family == LINUX_AF_INET6) {
                if (lwip_stack_is_ready() &&
                    lwip_stack_send_icmpv6_echo(sin6.sin6_addr, tx, (uint16_t)len, s->ip_ttl) == 0) {
                    s->ping_hw = 1;
                    return len;
                }
            }
        }
        return (uint64_t)-ENETUNREACH;
    }

    return len;
}

static uint64_t do_sys_recvfrom(uint64_t fd_u, uint64_t buf_u, uint64_t len_u, uint64_t flags_u, uint64_t addr_u, uint64_t addrlen_u) {
    (void)flags_u;
    int fd = (int)fd_u;
    edge_fd_proc_t *p = fd_proc_with_stdio();
    edge_fd_t *e = fd_get(p, fd);
    edge_socket_t *s = socket_from_fd(fd);
    uint32_t len = (uint32_t)len_u;
    if (!s) return (uint64_t)-ENOTSOCK;
    if (!buf_u) return (uint64_t)-EINVAL;
    uint64_t start_us = boottime_monotonic_us();

    while (s->rx_len == 0) {
        if (s->type == LINUX_SOCK_STREAM && s->closed) return 0;
        (void)socket_try_fill_ping_hw_reply(s);
        if (s->rx_len > 0) break;
        if ((e && (e->flags & LINUX_O_NONBLOCK)) || s->nonblock) return (uint64_t)-EAGAIN;
        if (s->recv_timeout_us > 0) {
            uint64_t now_us = boottime_monotonic_us();
            if (now_us - start_us >= s->recv_timeout_us) return (uint64_t)-EAGAIN;
        }
        if (signal_pending_interrupt()) return tty_interrupt_current_ret();
        lwip_stack_poll();
        wait_blocking_step();
    }

    uint32_t n = s->rx_len;
    if (n > len) n = len;
    if (copy_to_user(buf_u, s->rx_buf, n) < 0) return (uint64_t)-EFAULT;
    if (addr_u && addrlen_u) {
        uint32_t alen = 0;
        if (copy_from_user(&alen, addrlen_u, sizeof(alen)) < 0) return (uint64_t)-EFAULT;
        if (alen > s->rx_peer_len) alen = s->rx_peer_len;
        if (alen && copy_to_user(addr_u, s->rx_peer, alen) < 0) return (uint64_t)-EFAULT;
        if (copy_to_user(addrlen_u, &alen, sizeof(alen)) < 0) return (uint64_t)-EFAULT;
    }
    if (s->type == LINUX_SOCK_STREAM && n < s->rx_len) {
        memmove(s->rx_buf, s->rx_buf + n, s->rx_len - n);
        s->rx_len -= n;
    } else {
        s->rx_len = 0;
    }
    return n;
}

static uint64_t do_sys_recvmsg(uint64_t fd_u, uint64_t msg_u, uint64_t flags_u) {
    (void)flags_u;
    struct edge_linux_msghdr msg;
    struct edge_linux_iovec iov;
    edge_socket_t *s;
    uint64_t got;
    if (!msg_u) return (uint64_t)-EINVAL;
    if (copy_from_user(&msg, msg_u, sizeof(msg)) < 0) return (uint64_t)-EFAULT;
    if (!msg.msg_iov || msg.msg_iovlen < 1) return (uint64_t)-EINVAL;
    if (copy_from_user(&iov, msg.msg_iov, sizeof(iov)) < 0) return (uint64_t)-EFAULT;
    got = do_sys_recvfrom(fd_u, iov.iov_base, iov.iov_len, 0, 0, 0);
    if ((int64_t)got < 0) return got;
    s = socket_from_fd((int)fd_u);
    if (s && msg.msg_name && msg.msg_namelen > 0 && s->rx_peer_len > 0) {
        uint32_t nlen = msg.msg_namelen;
        if (nlen > s->rx_peer_len) nlen = s->rx_peer_len;
        if (copy_to_user(msg.msg_name, s->rx_peer, nlen) < 0) return (uint64_t)-EFAULT;
        msg.msg_namelen = nlen;
    }
    msg.msg_flags = 0;
    if (copy_to_user(msg_u, &msg, sizeof(msg)) < 0) return (uint64_t)-EFAULT;
    return got;
}

static uint64_t do_sys_sendmsg(uint64_t fd_u, uint64_t msg_u, uint64_t flags_u) {
    (void)flags_u;
    struct edge_linux_msghdr msg;
    struct edge_linux_iovec iov;
    if (!msg_u) return (uint64_t)-EINVAL;
    if (copy_from_user(&msg, msg_u, sizeof(msg)) < 0) return (uint64_t)-EFAULT;
    if (!msg.msg_iov || msg.msg_iovlen < 1) return (uint64_t)-EINVAL;
    if (copy_from_user(&iov, msg.msg_iov, sizeof(iov)) < 0) return (uint64_t)-EFAULT;
    return do_sys_sendto(fd_u, iov.iov_base, iov.iov_len, 0, msg.msg_name, msg.msg_namelen);
}

static uint64_t do_sys_getsockname(uint64_t fd_u, uint64_t addr_u, uint64_t len_u) {
    edge_socket_t *s = socket_from_fd((int)fd_u);
    uint32_t outlen = 0;
    struct edge_sockaddr_in sin;
    struct edge_sockaddr sa;
    if (!s || !addr_u || !len_u) return (uint64_t)-EINVAL;
    if (copy_from_user(&outlen, len_u, sizeof(outlen)) < 0) return (uint64_t)-EFAULT;
    if (s->domain == LINUX_AF_UNIX) {
        memset(&sa, 0, sizeof(sa));
        sa.sa_family = LINUX_AF_UNIX;
        if (outlen > sizeof(sa.sa_family)) outlen = sizeof(sa.sa_family);
        if (copy_to_user(addr_u, &sa, outlen) < 0) return (uint64_t)-EFAULT;
        if (copy_to_user(len_u, &outlen, sizeof(outlen)) < 0) return (uint64_t)-EFAULT;
        return 0;
    }
    if (s->domain == LINUX_AF_INET) {
        if (s->bind_len >= sizeof(sin)) {
            memcpy(&sin, s->bind_addr, sizeof(sin));
        } else {
            memset(&sin, 0, sizeof(sin));
            sin.sin_family = LINUX_AF_INET;
        }
        if (outlen > sizeof(sin)) outlen = sizeof(sin);
        if (copy_to_user(addr_u, &sin, outlen) < 0) return (uint64_t)-EFAULT;
    } else {
        if (s->bind_len == 0) return (uint64_t)-EADDRNOTAVAIL;
        if (outlen > s->bind_len) outlen = s->bind_len;
        if (copy_to_user(addr_u, s->bind_addr, outlen) < 0) return (uint64_t)-EFAULT;
    }
    if (copy_to_user(len_u, &outlen, sizeof(outlen)) < 0) return (uint64_t)-EFAULT;
    return 0;
}

static uint64_t do_sys_getpeername(uint64_t fd_u, uint64_t addr_u, uint64_t len_u) {
    edge_socket_t *s = socket_from_fd((int)fd_u);
    uint32_t outlen = 0;
    struct edge_sockaddr sa;
    if (!s || !addr_u || !len_u) return (uint64_t)-EINVAL;
    if (s->domain == LINUX_AF_UNIX) {
        if (!s->connected || s->unix_peer_id < 0) return (uint64_t)-ENOTCONN;
        if (copy_from_user(&outlen, len_u, sizeof(outlen)) < 0) return (uint64_t)-EFAULT;
        memset(&sa, 0, sizeof(sa));
        sa.sa_family = LINUX_AF_UNIX;
        if (outlen > sizeof(sa.sa_family)) outlen = sizeof(sa.sa_family);
        if (copy_to_user(addr_u, &sa, outlen) < 0) return (uint64_t)-EFAULT;
        if (copy_to_user(len_u, &outlen, sizeof(outlen)) < 0) return (uint64_t)-EFAULT;
        return 0;
    }
    if (!s->connected || s->peer_len == 0) return (uint64_t)-EADDRNOTAVAIL;
    if (copy_from_user(&outlen, len_u, sizeof(outlen)) < 0) return (uint64_t)-EFAULT;
    if (outlen > s->peer_len) outlen = s->peer_len;
    if (copy_to_user(addr_u, s->peer_addr, outlen) < 0) return (uint64_t)-EFAULT;
    if (copy_to_user(len_u, &outlen, sizeof(outlen)) < 0) return (uint64_t)-EFAULT;
    return 0;
}

static uint64_t do_sys_ioctl(uint64_t fd_u, uint64_t cmd_u, uint64_t arg_u) {
    int fd = (int)fd_u;
    uint32_t cmd = (uint32_t)cmd_u;
    task_t *cur = process_current_task();
    int trace_tty_cmd = tty_ioctl_cmd_is_traced(cmd);

    edge_fd_proc_t *p = fd_proc_with_stdio();
    edge_fd_t *e = fd_get(p, fd);
#if EDGE_FD_FORK_DEBUG
    if (cmd == LINUX_TIOCSPGRP) {
        int pid = cur ? cur->pid : process_getpid();
        printf("[fd][forkdbg] ioctl pid=%d cmd=TIOCSPGRP fd=%d lookup=%s\n",
               pid, fd, e ? "hit" : "miss");
        if (e) {
            printf("[fd][forkdbg] ioctl-slot pid=%d fd=%d kind=%s ref=%d path=%s\n",
                   pid, fd, fd_kind_name(e->kind), e->file_ref, e->path[0] ? e->path : "-");
        }
    }
#endif
    if (!e) {
        if (trace_tty_cmd) tty_log_ioctl_once(cur, fd, cmd, NULL, "EBADF(fd_lookup)");
        return (uint64_t)-EBADF;
    }
    if (tty_ioctl_cmd_requires_tty(cmd) && !fd_is_tty(e)) {
        tty_log_ioctl_once(cur, fd, cmd, e, "ENOTTY(non-tty-fd)");
        return (uint64_t)-ENOTTY;
    }

    if (e->kind == FD_SOCKET) {
        if (cmd == LINUX_FIONBIO) {
            int on = 0;
            if (!arg_u) return (uint64_t)-EINVAL;
            if (copy_from_user(&on, arg_u, sizeof(on)) < 0) return (uint64_t)-EFAULT;
            e->flags = (e->flags & ~LINUX_O_NONBLOCK) | (on ? LINUX_O_NONBLOCK : 0);
            if (e->pipe_id >= 0 && e->pipe_id < EDGE_MAX_SOCKETS && g_sockets[e->pipe_id].used) {
                g_sockets[e->pipe_id].nonblock = on ? 1 : 0;
            }
            return 0;
        }
        if (cmd == LINUX_FIONREAD) {
            int avail = 0;
            if (!arg_u) return (uint64_t)-EINVAL;
            if (e->pipe_id >= 0 && e->pipe_id < EDGE_MAX_SOCKETS && g_sockets[e->pipe_id].used) {
                avail = (int)g_sockets[e->pipe_id].rx_len;
            }
            if (copy_to_user(arg_u, &avail, sizeof(avail)) < 0) return (uint64_t)-EFAULT;
            return 0;
        }
        uint64_t nrc = net_ioctl_socket(cmd, arg_u);
        if ((int64_t)nrc != -ENOTTY) return nrc;
    }
    if (e->kind == FD_VFS && path_is_mouse_input(e->path)) {
        if (cmd == LINUX_FIONREAD) {
            int avail;
            if (!arg_u) return (uint64_t)-EINVAL;
            avail = keyboard_mouse_pending();
            if (copy_to_user(arg_u, &avail, sizeof(avail)) < 0) return (uint64_t)-EFAULT;
            return 0;
        }
    }

    if (cmd == LINUX_TIOCGPTN) {
        int n;
        if (e->kind != FD_PTY_MASTER) return (uint64_t)-ENOTTY;
        if (!arg_u) return (uint64_t)-EINVAL;
        n = e->pipe_id;
        if (copy_to_user(arg_u, &n, sizeof(n)) < 0) return (uint64_t)-EFAULT;
        return 0;
    }
    if (cmd == LINUX_TIOCSPTLCK) {
        int lockv;
        edge_pty_t *pty;
        if (e->kind != FD_PTY_MASTER) return (uint64_t)-ENOTTY;
        if (!arg_u) return (uint64_t)-EINVAL;
        if (copy_from_user(&lockv, arg_u, sizeof(lockv)) < 0) return (uint64_t)-EFAULT;
        if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_PTYS) return (uint64_t)-EINVAL;
        pty = &g_ptys[e->pipe_id];
        if (!pty->used) return (uint64_t)-EINVAL;
        pty->unlocked = (lockv == 0) ? 1 : 0;
        return 0;
    }
    if (cmd == LINUX_TIOCSCTTY) {
        task_t *tcur = process_current_task();
        if (e->kind == FD_PTY_MASTER || e->kind == FD_PTY_SLAVE) {
            if (e->pipe_id >= 0 && e->pipe_id < EDGE_MAX_PTYS && g_ptys[e->pipe_id].used) {
                int pg = process_getpgid(0);
                if (pg > 0) g_ptys[e->pipe_id].fg_pgid = pg;
                if (tcur) {
                    tcur->ctty_kind = PROCESS_CTTY_PTY;
                    tcur->ctty_id = e->pipe_id;
                }
            }
        } else {
            int pg = process_getpgid(0);
            if (pg > 0) g_tty_foreground_pgid = pg;
            if (tcur) {
                tcur->ctty_kind = PROCESS_CTTY_CONSOLE;
                tcur->ctty_id = -1;
            }
        }
        return 0;
    }
    if (cmd == LINUX_TIOCNOTTY) {
        task_t *tcur = process_current_task();
        if (tcur) {
            tcur->ctty_kind = PROCESS_CTTY_NONE;
            tcur->ctty_id = -1;
        }
        return 0;
    }

    if (cmd == LINUX_TIOCGWINSZ) {
        struct edge_winsize ws;
        ws.ws_row = 25;
        ws.ws_col = 80;
        ws.ws_xpixel = 0;
        ws.ws_ypixel = 0;
        if ((e->kind == FD_PTY_MASTER || e->kind == FD_PTY_SLAVE) &&
            e->pipe_id >= 0 && e->pipe_id < EDGE_MAX_PTYS && g_ptys[e->pipe_id].used) {
            ws = g_ptys[e->pipe_id].winsz;
        }
        if (arg_u && copy_to_user(arg_u, &ws, sizeof(ws)) < 0) return (uint64_t)-EFAULT;
        return 0;
    }
    if (cmd == LINUX_TIOCSWINSZ) {
        struct edge_winsize ws;
        if (!arg_u) return (uint64_t)-EINVAL;
        if (copy_from_user(&ws, arg_u, sizeof(ws)) < 0) return (uint64_t)-EFAULT;
        if ((e->kind == FD_PTY_MASTER || e->kind == FD_PTY_SLAVE) &&
            e->pipe_id >= 0 && e->pipe_id < EDGE_MAX_PTYS && g_ptys[e->pipe_id].used) {
            g_ptys[e->pipe_id].winsz = ws;
        }
        return 0;
    }
    if (cmd == LINUX_TCGETS) {
        if (e->kind == FD_PTY_MASTER || e->kind == FD_PTY_SLAVE) {
            if (!arg_u) return (uint64_t)-EINVAL;
            if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_PTYS || !g_ptys[e->pipe_id].used) return (uint64_t)-EINVAL;
            if (copy_to_user(arg_u, &g_ptys[e->pipe_id].termios, sizeof(g_ptys[e->pipe_id].termios)) < 0) return (uint64_t)-EFAULT;
            tty_log_ioctl_once(cur, fd, cmd, e, "ok");
            return 0;
        }
        if (!arg_u) return (uint64_t)-EINVAL;
        if (copy_to_user(arg_u, &g_tty_termios, sizeof(g_tty_termios)) < 0) return (uint64_t)-EFAULT;
        tty_log_ioctl_once(cur, fd, cmd, e, "ok");
        return 0;
    }
    if (cmd == LINUX_TCSETS || cmd == LINUX_TCSETSW || cmd == LINUX_TCSETSF) {
        struct edge_termios t;
        if (!arg_u) return (uint64_t)-EINVAL;
        if (copy_from_user(&t, arg_u, sizeof(t)) < 0) return (uint64_t)-EFAULT;
        if (e->kind == FD_PTY_MASTER || e->kind == FD_PTY_SLAVE) {
            if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_PTYS || !g_ptys[e->pipe_id].used) return (uint64_t)-EINVAL;
            g_ptys[e->pipe_id].termios = t;
            tty_log_ioctl_once(cur, fd, cmd, e, "ok");
            return 0;
        }
        g_tty_termios = t;
        if (cmd == LINUX_TCSETSF) {
            g_tty_line_len = 0;
            g_tty_line_pos = 0;
        }
        tty_log_ioctl_once(cur, fd, cmd, e, "ok");
        return 0;
    }
    if (cmd == LINUX_TIOCGPGRP) {
        int cur_pgid = cur ? cur->pgid : process_getpgid(0);
        int out_pgid = g_tty_foreground_pgid;
        if (e->kind == FD_PTY_MASTER || e->kind == FD_PTY_SLAVE) {
            edge_pty_t *pty;
            if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_PTYS) return (uint64_t)-EINVAL;
            pty = &g_ptys[e->pipe_id];
            if (!pty->used) return (uint64_t)-EINVAL;
            if (pty->fg_pgid <= 0 || !tty_pgrp_alive(pty->fg_pgid)) {
                if (cur_pgid > 0) pty->fg_pgid = cur_pgid;
            }
            out_pgid = pty->fg_pgid;
        } else {
            if (g_tty_foreground_pgid <= 0 || !tty_pgrp_alive(g_tty_foreground_pgid)) {
                if (cur_pgid > 0) g_tty_foreground_pgid = cur_pgid;
            }
    #if EDGE_TTY_JOBCONTROL_COMPAT
            if (cur_pgid > 0 && g_tty_foreground_pgid != cur_pgid) {
                int old_fg = g_tty_foreground_pgid;
                g_tty_foreground_pgid = cur_pgid;
                tty_log_fg_fix_once(cur, old_fg, g_tty_foreground_pgid, "tcgetpgrp-compat");
            }
    #endif
            out_pgid = g_tty_foreground_pgid;
        }
        if (!arg_u) return (uint64_t)-EINVAL;
        if (copy_to_user(arg_u, &out_pgid, sizeof(int)) < 0) return (uint64_t)-EFAULT;
        tty_log_ioctl_once(cur, fd, cmd, e, "ok");
        return 0;
    }
    if (cmd == LINUX_TIOCSPGRP) {
        int pg = 0;
        int found = 0;
        if (!arg_u) return (uint64_t)-EINVAL;
        if (copy_from_user(&pg, arg_u, sizeof(int)) < 0) return (uint64_t)-EFAULT;
        if (pg <= 0) return (uint64_t)-EINVAL;
        for (int i = 0; i < PROC_MAX_TASKS; ++i) {
            const task_t *t = process_task_by_index(i);
            if (!t || t->state == TASK_UNUSED) continue;
            if (t->pgid == pg) {
                found = 1;
                break;
            }
        }
        if (!found) return (uint64_t)-ESRCH;
        if (e->kind == FD_PTY_MASTER || e->kind == FD_PTY_SLAVE) {
            if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_PTYS) return (uint64_t)-EINVAL;
            if (!g_ptys[e->pipe_id].used) return (uint64_t)-EINVAL;
            g_ptys[e->pipe_id].fg_pgid = pg;
        } else {
            g_tty_foreground_pgid = pg;
        }
        tty_log_ioctl_once(cur, fd, cmd, e, "ok");
        return 0;
    }

    if (e->kind == FD_VFS) {
        int rc = vfs_dev_ioctl(e->path, cmd, (void *)(uintptr_t)arg_u);
        if (rc == 0) return 0;
        if (rc == -ENOSYS) return (uint64_t)-ENOTTY;
    }

    return (uint64_t)-ENOTTY;
}

static uint64_t do_sys_fcntl(uint64_t fd_u, uint64_t cmd_u, uint64_t arg_u) {
    int fd = (int)fd_u;
    int cmd = (int)cmd_u;
    edge_fd_proc_t *p = fd_proc_with_stdio();
    edge_fd_t *e = fd_get(p, fd);
    if (!e) return (uint64_t)-EBADF;

    switch (cmd) {
        case LINUX_F_DUPFD:
        case LINUX_F_DUPFD_CLOEXEC: {
            int oldfd = fd;
            int nfd = fd_alloc(p, (int)arg_u);
            if (nfd < 0) return (uint64_t)-ENOMEM;
            p->fds[nfd] = *e;
            if (p->fds[nfd].file_ref <= 0 || file_ref_get(p->fds[nfd].file_ref) < 0) {
                memset(&p->fds[nfd], 0, sizeof(p->fds[nfd]));
                return (uint64_t)-ENOMEM;
            }
            if (cmd == LINUX_F_DUPFD_CLOEXEC) p->fds[nfd].fd_flags |= LINUX_FD_CLOEXEC;
            else p->fds[nfd].fd_flags &= ~LINUX_FD_CLOEXEC;
            if (p->fds[nfd].kind == FD_PIPE_R && p->fds[nfd].pipe_id >= 0 && p->fds[nfd].pipe_id < EDGE_MAX_PIPES) g_pipes[p->fds[nfd].pipe_id].readers++;
            if (p->fds[nfd].kind == FD_PIPE_W && p->fds[nfd].pipe_id >= 0 && p->fds[nfd].pipe_id < EDGE_MAX_PIPES) g_pipes[p->fds[nfd].pipe_id].writers++;
            if (p->fds[nfd].kind == FD_SOCKET && p->fds[nfd].pipe_id >= 0 && p->fds[nfd].pipe_id < EDGE_MAX_SOCKETS) socket_add_ref(p->fds[nfd].pipe_id);
            if (p->fds[nfd].kind == FD_PTY_MASTER && p->fds[nfd].pipe_id >= 0 && p->fds[nfd].pipe_id < EDGE_MAX_PTYS) pty_add_ref(p->fds[nfd].pipe_id, 1);
            if (p->fds[nfd].kind == FD_PTY_SLAVE && p->fds[nfd].pipe_id >= 0 && p->fds[nfd].pipe_id < EDGE_MAX_PTYS) pty_add_ref(p->fds[nfd].pipe_id, 0);
            if (p->fds[nfd].kind == FD_EVENTFD) eventfd_add_ref(p->fds[nfd].pipe_id);
            if (p->fds[nfd].kind == FD_TIMERFD) timerfd_add_ref(p->fds[nfd].pipe_id);
            if (p->fds[nfd].kind == FD_SIGNALFD) signalfd_add_ref(p->fds[nfd].pipe_id);
            if (p->fds[nfd].kind == FD_EPOLL) epoll_add_ref(p->fds[nfd].pipe_id);
            fd_log_lifecycle("dup", process_getpid(), oldfd, e, nfd);
            fd_log_lifecycle("dup-new", process_getpid(), nfd, &p->fds[nfd], oldfd);
            return (uint64_t)nfd;
        }
        case LINUX_F_GETFD:
            return (uint64_t)(e->fd_flags & LINUX_FD_CLOEXEC);
        case LINUX_F_SETFD:
            e->fd_flags = (int)(arg_u & LINUX_FD_CLOEXEC);
            return 0;
        case LINUX_F_GETFL:
            return (uint64_t)e->flags;
        case LINUX_F_SETFL:
            e->flags = (e->flags & LINUX_O_ACCMODE) | ((int)arg_u & ~LINUX_O_ACCMODE);
            if (e->kind == FD_SOCKET && e->pipe_id >= 0 && e->pipe_id < EDGE_MAX_SOCKETS) {
                if (g_sockets[e->pipe_id].used) {
                    g_sockets[e->pipe_id].nonblock = ((e->flags & LINUX_O_NONBLOCK) != 0);
                }
            }
            return 0;
        case LINUX_F_SETLK:
        case LINUX_F_SETLKW:
            /* Advisory record locks are process-local no-op for single-node edgeOS. */
            return 0;
        case LINUX_F_GETLK: {
            struct edge_linux_flock fl;
            if (!arg_u) return (uint64_t)-EINVAL;
            if (copy_from_user(&fl, arg_u, sizeof(fl)) < 0) return (uint64_t)-EFAULT;
            fl.l_type = LINUX_F_UNLCK;
            fl.l_pid = 0;
            if (copy_to_user(arg_u, &fl, sizeof(fl)) < 0) return (uint64_t)-EFAULT;
            return 0;
        }
        default:
            return (uint64_t)-ENOSYS;
    }
}

static uint64_t do_sys_dup(uint64_t fd_u) {
    return do_sys_fcntl(fd_u, LINUX_F_DUPFD, 0);
}

static uint64_t do_sys_dup2(uint64_t oldfd_u, uint64_t newfd_u) {
    int oldfd = (int)oldfd_u;
    int newfd = (int)newfd_u;
    edge_fd_proc_t *p = fd_proc_with_stdio();
    edge_fd_t *oldf = fd_get(p, oldfd);
    if (!oldf || !p || newfd < 0 || newfd >= EDGE_MAX_FD) return (uint64_t)-EBADF;
    if (oldfd == newfd) return (uint64_t)newfd;
    if (p->fds[newfd].used) (void)do_sys_close((uint64_t)newfd);
    p->fds[newfd] = *oldf;
    if (p->fds[newfd].file_ref <= 0 || file_ref_get(p->fds[newfd].file_ref) < 0) {
        memset(&p->fds[newfd], 0, sizeof(p->fds[newfd]));
        return (uint64_t)-ENOMEM;
    }
    p->fds[newfd].used = 1;
    p->fds[newfd].fd_flags &= ~LINUX_FD_CLOEXEC;
    if (p->fds[newfd].kind == FD_PIPE_R && p->fds[newfd].pipe_id >= 0 && p->fds[newfd].pipe_id < EDGE_MAX_PIPES) g_pipes[p->fds[newfd].pipe_id].readers++;
    if (p->fds[newfd].kind == FD_PIPE_W && p->fds[newfd].pipe_id >= 0 && p->fds[newfd].pipe_id < EDGE_MAX_PIPES) g_pipes[p->fds[newfd].pipe_id].writers++;
    if (p->fds[newfd].kind == FD_SOCKET && p->fds[newfd].pipe_id >= 0 && p->fds[newfd].pipe_id < EDGE_MAX_SOCKETS) socket_add_ref(p->fds[newfd].pipe_id);
    if (p->fds[newfd].kind == FD_PTY_MASTER && p->fds[newfd].pipe_id >= 0 && p->fds[newfd].pipe_id < EDGE_MAX_PTYS) pty_add_ref(p->fds[newfd].pipe_id, 1);
    if (p->fds[newfd].kind == FD_PTY_SLAVE && p->fds[newfd].pipe_id >= 0 && p->fds[newfd].pipe_id < EDGE_MAX_PTYS) pty_add_ref(p->fds[newfd].pipe_id, 0);
    if (p->fds[newfd].kind == FD_EVENTFD) eventfd_add_ref(p->fds[newfd].pipe_id);
    if (p->fds[newfd].kind == FD_TIMERFD) timerfd_add_ref(p->fds[newfd].pipe_id);
    if (p->fds[newfd].kind == FD_SIGNALFD) signalfd_add_ref(p->fds[newfd].pipe_id);
    if (p->fds[newfd].kind == FD_EPOLL) epoll_add_ref(p->fds[newfd].pipe_id);
    if (ssh_trace_task(process_current_task())) {
        printf("[sshdbg] dup2 pid=%d cmd=%s old=%d new=%d kind=%d fl=0x%x sid=%d\n",
               process_getpid(), process_current_task()->name, oldfd, newfd,
               (int)p->fds[newfd].kind, (unsigned)p->fds[newfd].flags, p->fds[newfd].pipe_id);
    }
    fd_log_lifecycle("dup2", process_getpid(), oldfd, oldf, newfd);
    fd_log_lifecycle("dup2-new", process_getpid(), newfd, &p->fds[newfd], oldfd);
    return (uint64_t)newfd;
}

static uint64_t do_sys_dup3(uint64_t oldfd_u, uint64_t newfd_u, uint64_t flags_u) {
    int oldfd = (int)oldfd_u;
    int newfd = (int)newfd_u;
    int flags = (int)flags_u;
    if (flags & ~LINUX_O_CLOEXEC) return (uint64_t)-EINVAL;
    if (oldfd == newfd) return (uint64_t)-EINVAL;
    uint64_t r = do_sys_dup2(oldfd_u, newfd_u);
    if ((int64_t)r < 0) return r;
    if (flags & LINUX_O_CLOEXEC) {
        edge_fd_proc_t *p = fd_proc_with_stdio();
        edge_fd_t *e = fd_get(p, (int)r);
        if (e) e->fd_flags |= LINUX_FD_CLOEXEC;
    }
    return r;
}

static uint64_t do_sys_pipe(uint64_t pipefd_u, uint64_t flags_u) {
    int flags = (int)flags_u;
    if (flags & ~(LINUX_O_CLOEXEC | LINUX_O_NONBLOCK)) return (uint64_t)-EINVAL;
    edge_fd_proc_t *p = fd_proc_with_stdio();
    if (!p) return (uint64_t)-ENOMEM;

    int pid = pipe_alloc();
    if (pid < 0) return (uint64_t)-ENOMEM;

    int rfd = fd_alloc(p, 0);
    if (rfd < 0) {
        memset(&g_pipes[pid], 0, sizeof(g_pipes[pid]));
        return (uint64_t)-ENOMEM;
    }
    int wfd = fd_alloc(p, 0);
    if (wfd < 0) {
        memset(&p->fds[rfd], 0, sizeof(p->fds[rfd]));
        memset(&g_pipes[pid], 0, sizeof(g_pipes[pid]));
        return (uint64_t)-ENOMEM;
    }

    p->fds[rfd].kind = FD_PIPE_R;
    p->fds[rfd].file_ref = file_ref_alloc();
    if (!p->fds[rfd].file_ref) {
        memset(&p->fds[rfd], 0, sizeof(p->fds[rfd]));
        memset(&p->fds[wfd], 0, sizeof(p->fds[wfd]));
        memset(&g_pipes[pid], 0, sizeof(g_pipes[pid]));
        return (uint64_t)-ENOMEM;
    }
    p->fds[rfd].pipe_id = pid;
    p->fds[rfd].flags = (flags & LINUX_O_NONBLOCK) ? LINUX_O_NONBLOCK : 0;
    p->fds[rfd].fd_flags = (flags & LINUX_O_CLOEXEC) ? LINUX_FD_CLOEXEC : 0;
    p->fds[wfd].kind = FD_PIPE_W;
    p->fds[wfd].file_ref = file_ref_alloc();
    if (!p->fds[wfd].file_ref) {
        (void)file_ref_put(p->fds[rfd].file_ref);
        memset(&p->fds[rfd], 0, sizeof(p->fds[rfd]));
        memset(&p->fds[wfd], 0, sizeof(p->fds[wfd]));
        memset(&g_pipes[pid], 0, sizeof(g_pipes[pid]));
        return (uint64_t)-ENOMEM;
    }
    p->fds[wfd].pipe_id = pid;
    p->fds[wfd].flags = LINUX_O_WRONLY | ((flags & LINUX_O_NONBLOCK) ? LINUX_O_NONBLOCK : 0);
    p->fds[wfd].fd_flags = (flags & LINUX_O_CLOEXEC) ? LINUX_FD_CLOEXEC : 0;

    g_pipes[pid].readers++;
    g_pipes[pid].writers++;

    int32_t pair[2];
    pair[0] = rfd;
    pair[1] = wfd;
    if (copy_to_user(pipefd_u, pair, sizeof(pair)) < 0) {
        (void)do_sys_close((uint64_t)rfd);
        (void)do_sys_close((uint64_t)wfd);
        return (uint64_t)-EFAULT;
    }
    return 0;
}

static uint64_t do_sys_fork_compat(REGISTERS *regs) {
    int parent_pid = process_getpid();
    int child_pid = process_fork((const edge_trap_frame_t *)regs);
    if (child_pid < 0) return (uint64_t)-EAGAIN;
    if (child_pid > 0) {
        fd_clone_after_fork(parent_pid, child_pid);
        (void)process_set_state(child_pid, TASK_RUNNABLE);
    }
    return (uint64_t)child_pid;
}

static uint64_t do_sys_vfork_compat(REGISTERS *regs);

static uint64_t do_sys_clone_compat(uint64_t flags_u, uint64_t child_stack_u, uint64_t parent_tid_u,
                                    uint64_t child_tid_u, uint64_t tls_u, REGISTERS *regs) {
    uint64_t sig = flags_u & 0xffu;
    uint64_t unsupported;
    uint64_t ret;
    (void)parent_tid_u;
    (void)child_tid_u;
    (void)tls_u;
    if (!regs) return (uint64_t)-EINVAL;
    unsupported = flags_u & ~(uint64_t)(0xffu | LINUX_CLONE_VFORK);
    /* Return ENOSYS (not EINVAL) for non-fork-like clone usage so musl and
     * applications can fall back to fork/vfork code paths. */
    if (unsupported) return (uint64_t)-ENOSYS;
    if (flags_u & (LINUX_CLONE_VM | LINUX_CLONE_THREAD | LINUX_CLONE_SIGHAND | LINUX_CLONE_FILES | LINUX_CLONE_FS)) {
        return (uint64_t)-ENOSYS;
    }
    if (sig != 0 && sig != LINUX_SIGCHLD) return (uint64_t)-ENOSYS;
    if (child_stack_u || parent_tid_u || child_tid_u || tls_u) return (uint64_t)-ENOSYS;

    if (flags_u & LINUX_CLONE_VFORK) {
        ret = do_sys_vfork_compat(regs);
    } else {
        ret = do_sys_fork_compat(regs);
    }
    if ((int64_t)ret <= 0) return ret;

    return ret;
}

static uint64_t do_sys_vfork_compat(REGISTERS *regs) {
    uint64_t ret = do_sys_fork_compat(regs);
    if ((int64_t)ret > 0) {
        /* Best-effort vfork compatibility: schedule the child immediately so it
         * can exec/exit before the parent continues shell logic. */
        scheduler_yield();
    }
    return ret;
}

static uint64_t do_sys_clone3(uint64_t args_u, uint64_t size_u, REGISTERS *regs) {
    struct edge_linux_clone_args ca;
    uint64_t ret;
    if (!args_u || !regs) return (uint64_t)-EINVAL;
    if (size_u < 64 || size_u > sizeof(ca)) return (uint64_t)-EINVAL;
    memset(&ca, 0, sizeof(ca));
    if (copy_from_user(&ca, args_u, size_u) < 0) return (uint64_t)-EFAULT;

    if ((ca.flags & ~LINUX_CLONE_PIDFD) != 0) return (uint64_t)-EINVAL;
    if (ca.stack || ca.stack_size || ca.tls || ca.child_tid || ca.parent_tid || ca.set_tid || ca.set_tid_size || ca.cgroup) return (uint64_t)-EINVAL;
    if (ca.exit_signal != 0 && ca.exit_signal != LINUX_SIGCHLD) return (uint64_t)-EINVAL;
    if ((ca.flags & LINUX_CLONE_PIDFD) && !ca.pidfd) return (uint64_t)-EINVAL;

    ret = do_sys_fork_compat(regs);
    if ((int64_t)ret < 0) return ret;

    if (ret > 0 && (ca.flags & LINUX_CLONE_PIDFD)) {
        int pfd = alloc_special_fd(FD_PIDFD, (int)ret, LINUX_O_CLOEXEC);
        if (pfd < 0) return (uint64_t)(int64_t)pfd;
        if (copy_to_user(ca.pidfd, &pfd, sizeof(pfd)) < 0) {
            (void)do_sys_close((uint64_t)pfd);
            return (uint64_t)-EFAULT;
        }
    }
    return ret;
}

static uint64_t do_sys_pidfd_open(uint64_t pid_u, uint64_t flags_u) {
    int pid = (int)pid_u;
    if (flags_u != 0) return (uint64_t)-EINVAL;
    if (pid <= 0) return (uint64_t)-EINVAL;
    if (!process_get_task(pid)) return (uint64_t)-ESRCH;
    return (uint64_t)(int64_t)alloc_special_fd(FD_PIDFD, pid, LINUX_O_CLOEXEC);
}

static uint64_t do_sys_pidfd_send_signal(uint64_t pidfd_u, uint64_t sig_u, uint64_t info_u, uint64_t flags_u) {
    edge_fd_proc_t *p = fd_proc_with_stdio();
    edge_fd_t *e = fd_get(p, (int)pidfd_u);
    int sig = (int)sig_u;
    int pid;
    (void)info_u;
    if (flags_u != 0) return (uint64_t)-EINVAL;
    if (!e || e->kind != FD_PIDFD) return (uint64_t)-EBADF;
    pid = e->pipe_id;
    if (sig < 0) return (uint64_t)-EINVAL;
    if (sig == 0) return process_get_task(pid) ? 0 : (uint64_t)-ESRCH;
    return process_send_signal(pid, sig) == 0 ? 0 : (uint64_t)-ESRCH;
}

static uint64_t do_sys_io_uring_setup(uint64_t entries_u, uint64_t params_u) {
    (void)entries_u;
    (void)params_u;
    return (uint64_t)-ENOSYS;
}

static uint64_t do_sys_io_uring_enter(uint64_t fd_u, uint64_t to_submit_u, uint64_t min_complete_u,
                                      uint64_t flags_u, uint64_t sig_u, uint64_t sigsz_u) {
    (void)fd_u;
    (void)to_submit_u;
    (void)min_complete_u;
    (void)flags_u;
    (void)sig_u;
    (void)sigsz_u;
    return (uint64_t)-ENOSYS;
}

static uint64_t do_sys_io_uring_register(uint64_t fd_u, uint64_t opcode_u, uint64_t arg_u, uint64_t nr_args_u) {
    (void)fd_u;
    (void)opcode_u;
    (void)arg_u;
    (void)nr_args_u;
    return (uint64_t)-ENOSYS;
}

static uint64_t do_sys_wait4(uint64_t pid_u, uint64_t status_u, uint64_t options_u, uint64_t rusage_u) {
    int pid = (int)pid_u;
    int options = (int)options_u;
    int wait_opts = 0;
    int status = 0;
    int rc;
    (void)rusage_u;

    /*
     * We currently only implement exited-child waits. Accept common Linux
     * flags used by shells (WUNTRACED/WCONTINUED) and ignore their semantics
     * instead of failing with EINVAL, so waitpid() can still reap children.
     */
    if (options & ~(LINUX_WNOHANG | LINUX_WUNTRACED | LINUX_WCONTINUED)) return (uint64_t)-EINVAL;
    if (options & LINUX_WNOHANG) wait_opts |= LINUX_WNOHANG;
    rc = process_wait_pid(pid, status_u ? &status : 0, wait_opts);
    if (rc < 0) return (uint64_t)-ECHILD;
    if (status_u && rc > 0) {
        if (copy_to_user(status_u, &status, sizeof(status)) < 0) return (uint64_t)-EFAULT;
    }
    return (uint64_t)rc;
}

static uint64_t do_sys_fchmod(uint64_t fd_u, uint64_t mode_u) {
    int fd = (int)fd_u;
    task_t *t = process_current_task();
    edge_fd_proc_t *p = fd_proc_with_stdio();
    edge_fd_t *e = fd_get(p, fd);
    if (!e) return (uint64_t)-EBADF;
    if (e->kind != FD_VFS) return (uint64_t)-EINVAL;
    if (!t) return 0;
    if (t->euid != 0 && t->euid != e->inode.uid) return (uint64_t)-EPERM;
    if (vfs_chmod(e->path, (uint16_t)mode_u) < 0) return (uint64_t)-EINVAL;
    return 0;
}

static uint64_t do_sys_fchown(uint64_t fd_u, uint64_t owner_u, uint64_t group_u) {
    int fd = (int)fd_u;
    task_t *t = process_current_task();
    edge_fd_proc_t *p = fd_proc_with_stdio();
    edge_fd_t *e = fd_get(p, fd);
    if (!e) return (uint64_t)-EBADF;
    if (e->kind != FD_VFS) return (uint64_t)-EINVAL;
    if (t && t->euid != 0) return (uint64_t)-EPERM;
    if (vfs_chown(e->path, (uint16_t)owner_u, (uint16_t)group_u) < 0) return (uint64_t)-EINVAL;
    return 0;
}

static uint64_t do_sys_chmod(uint64_t path_u, uint64_t mode_u) {
    char path[256];
    vfs_inode_t ino;
    task_t *t = process_current_task();
    if (!path_u) return (uint64_t)-EINVAL;
    if (copy_user_cstr(path, sizeof(path), path_u) < 0) return (uint64_t)-EFAULT;
    if (vfs_resolve(path, &ino, 0, 0, 0) < 0) return (uint64_t)-ENOENT;
    if (t && t->euid != 0 && t->euid != ino.uid) return (uint64_t)-EPERM;
    return vfs_chmod(path, (uint16_t)mode_u) == 0 ? 0 : (uint64_t)-EINVAL;
}

static uint64_t do_sys_chown(uint64_t path_u, uint64_t uid_u, uint64_t gid_u) {
    char path[256];
    task_t *t = process_current_task();
    if (!path_u) return (uint64_t)-EINVAL;
    if (copy_user_cstr(path, sizeof(path), path_u) < 0) return (uint64_t)-EFAULT;
    if (t && t->euid != 0) return (uint64_t)-EPERM;
    return vfs_chown(path, (uint16_t)uid_u, (uint16_t)gid_u) == 0 ? 0 : (uint64_t)-EINVAL;
}

static uint64_t do_sys_setuid(uint64_t uid_u) {
    return process_setuid((uint32_t)uid_u) == 0 ? 0 : (uint64_t)-EPERM;
}

static uint64_t do_sys_setgid(uint64_t gid_u) {
    return process_setgid((uint32_t)gid_u) == 0 ? 0 : (uint64_t)-EPERM;
}

static uint64_t do_sys_setreuid(uint64_t ruid_u, uint64_t euid_u) {
    return process_setreuid((uint32_t)ruid_u, (uint32_t)euid_u) == 0 ? 0 : (uint64_t)-EPERM;
}

static uint64_t do_sys_setregid(uint64_t rgid_u, uint64_t egid_u) {
    return process_setregid((uint32_t)rgid_u, (uint32_t)egid_u) == 0 ? 0 : (uint64_t)-EPERM;
}

static uint64_t do_sys_setresuid(uint64_t ruid_u, uint64_t euid_u, uint64_t suid_u) {
    uint32_t ruid = (uint32_t)ruid_u;
    uint32_t euid = (uint32_t)euid_u;
    uint32_t suid = (uint32_t)suid_u;
    task_t *cur = process_current_task();
    if (!cur) return (uint64_t)-EINVAL;
    /* We don't track a separate saved uid yet; enforce privilege checks and apply real/effective ids. */
    if (cur->euid != 0 && suid != (uint32_t)-1 &&
        suid != cur->uid && suid != cur->euid &&
        (ruid == (uint32_t)-1 || suid != ruid) &&
        (euid == (uint32_t)-1 || suid != euid)) {
        return (uint64_t)-EPERM;
    }
    return process_setreuid(ruid, euid) == 0 ? 0 : (uint64_t)-EPERM;
}

static uint64_t do_sys_getresuid(uint64_t ruid_u, uint64_t euid_u, uint64_t suid_u) {
    uint32_t r = process_getuid();
    uint32_t e = process_geteuid();
    uint32_t s = e;
    if (!ruid_u || !euid_u || !suid_u) return (uint64_t)-EINVAL;
    if (copy_to_user(ruid_u, &r, sizeof(r)) < 0) return (uint64_t)-EFAULT;
    if (copy_to_user(euid_u, &e, sizeof(e)) < 0) return (uint64_t)-EFAULT;
    if (copy_to_user(suid_u, &s, sizeof(s)) < 0) return (uint64_t)-EFAULT;
    return 0;
}

static uint64_t do_sys_setresgid(uint64_t rgid_u, uint64_t egid_u, uint64_t sgid_u) {
    uint32_t rgid = (uint32_t)rgid_u;
    uint32_t egid = (uint32_t)egid_u;
    uint32_t sgid = (uint32_t)sgid_u;
    task_t *cur = process_current_task();
    if (!cur) return (uint64_t)-EINVAL;
    if (cur->euid != 0 && sgid != (uint32_t)-1 &&
        sgid != cur->gid && sgid != cur->egid &&
        (rgid == (uint32_t)-1 || sgid != rgid) &&
        (egid == (uint32_t)-1 || sgid != egid)) {
        return (uint64_t)-EPERM;
    }
    return process_setregid(rgid, egid) == 0 ? 0 : (uint64_t)-EPERM;
}

static uint64_t do_sys_getresgid(uint64_t rgid_u, uint64_t egid_u, uint64_t sgid_u) {
    uint32_t r = process_getgid();
    uint32_t e = process_getegid();
    uint32_t s = e;
    if (!rgid_u || !egid_u || !sgid_u) return (uint64_t)-EINVAL;
    if (copy_to_user(rgid_u, &r, sizeof(r)) < 0) return (uint64_t)-EFAULT;
    if (copy_to_user(egid_u, &e, sizeof(e)) < 0) return (uint64_t)-EFAULT;
    if (copy_to_user(sgid_u, &s, sizeof(s)) < 0) return (uint64_t)-EFAULT;
    return 0;
}

static uint64_t do_sys_setfsuid(uint64_t fsuid_u) {
    uint32_t prev = process_geteuid();
    (void)process_setreuid((uint32_t)-1, (uint32_t)fsuid_u);
    return prev;
}

static uint64_t do_sys_setfsgid(uint64_t fsgid_u) {
    uint32_t prev = process_getegid();
    (void)process_setregid((uint32_t)-1, (uint32_t)fsgid_u);
    return prev;
}

static uint64_t do_sys_getgroups(uint64_t size_u, uint64_t list_u) {
    int size = (int)size_u;
    (void)list_u;
    if (size < 0) return (uint64_t)-EINVAL;
    if (size == 0) return 0;
    return 0;
}

static uint64_t do_sys_setgroups(uint64_t size_u, uint64_t list_u) {
    int size = (int)size_u;
    (void)list_u;
    if (size < 0) return (uint64_t)-EINVAL;
    return 0;
}

static uint64_t do_sys_umask(uint64_t mask_u) {
    return (uint64_t)process_set_umask((uint32_t)mask_u);
}

static uint64_t do_sys_setpgid(uint64_t pid_u, uint64_t pgid_u) {
    return process_setpgid((int)pid_u, (int)pgid_u) == 0 ? 0 : (uint64_t)-EPERM;
}

static uint64_t do_sys_getpgrp(void) {
    int pg = process_getpgid(0);
    return pg >= 0 ? (uint64_t)pg : (uint64_t)-ESRCH;
}

static uint64_t do_sys_getpgid(uint64_t pid_u) {
    int pg = process_getpgid((int)pid_u);
    return pg >= 0 ? (uint64_t)pg : (uint64_t)-ESRCH;
}

static uint64_t do_sys_setsid(void) {
    int sid = process_setsid();
    if (sid < 0) return (uint64_t)-EPERM;
    return (uint64_t)sid;
}

static uint64_t do_sys_getsid(uint64_t pid_u) {
    int sid = process_getsid((int)pid_u);
    return sid >= 0 ? (uint64_t)sid : (uint64_t)-ESRCH;
}

static uint64_t do_sys_utimensat(uint64_t dirfd_u, uint64_t path_u, uint64_t times_u, uint64_t flags_u) {
    (void)times_u;
    (void)flags_u;
    int dirfd = (int)dirfd_u;
    if (path_u) {
        char path_in[256];
        char path[256];
        vfs_inode_t ino;
        if (copy_user_cstr(path_in, sizeof(path_in), path_u) < 0) return (uint64_t)-EFAULT;
        if (build_at_path(dirfd, path_in, path, (int)sizeof(path)) < 0) return (uint64_t)-EINVAL;
        if (vfs_resolve(path, &ino, 0, 0, 0) < 0) return (uint64_t)-ENOENT;
        return 0;
    }
    if (dirfd == LINUX_AT_FDCWD) return (uint64_t)-EINVAL;
    edge_fd_proc_t *p = fd_proc_with_stdio();
    return fd_get(p, dirfd) ? 0 : (uint64_t)-EBADF;
}

static uint64_t do_sys_brk(uint64_t addr) {
    task_t *cur = process_current_task();
    if (!cur) return (uint64_t)-EINVAL;
    if (addr == 0) return cur->user_brk;

    uint64_t min = cur->user_heap_base;
    uint64_t max = cur->user_heap_limit;
    uint64_t req = page_align_up(addr);
    if (req < min || req > max) return cur->user_brk;
    if (req > cur->user_brk) {
        for (int i = 0; i < PROCESS_USER_VMA_MAX; ++i) {
            edge_user_vma_t *v = &cur->user_vmas[i];
            if (v->end <= v->start) continue;
            if (v->end <= cur->user_brk || v->start >= req) continue;
            return cur->user_brk;
        }
    }
    if (req > min) {
        uint64_t flags = user_pte_flags(req - 1);
        if (!(flags & PTE_PRESENT) || !(flags & PTE_USER) || !(flags & PTE_WRITE)) return cur->user_brk;
    }
    cur->user_brk = req;
    return cur->user_brk;
}

static int user_vma_find_free_slot(task_t *t) {
    if (!t) return -1;
    for (int i = 0; i < PROCESS_USER_VMA_MAX; ++i) {
        if (t->user_vmas[i].end <= t->user_vmas[i].start) return i;
    }
    return -1;
}

static void user_vma_recount(task_t *t) {
    uint16_t n = 0;
    if (!t) return;
    for (int i = 0; i < PROCESS_USER_VMA_MAX; ++i) {
        if (t->user_vmas[i].end > t->user_vmas[i].start) ++n;
    }
    t->user_vma_count = n;
}

static void user_vma_refresh_mmap_hint(task_t *t) {
    uint64_t hint;
    if (!t) return;
    hint = t->user_heap_limit;
    for (int i = 0; i < PROCESS_USER_VMA_MAX; ++i) {
        edge_user_vma_t *v = &t->user_vmas[i];
        if (v->end <= v->start) continue;
        if (v->start < USER_HEAP_BASE_ADDR || v->start >= t->user_heap_limit) continue;
        if (v->start < hint) hint = v->start;
    }
    t->user_mmap_next = hint;
}

static int user_vma_record(task_t *t, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags) {
    int slot;
    if (!t || end <= start) return -1;
    slot = user_vma_find_free_slot(t);
    if (slot < 0) return -1;
    t->user_vmas[slot].start = start;
    t->user_vmas[slot].end = end;
    t->user_vmas[slot].prot = prot;
    t->user_vmas[slot].flags = flags;
    user_vma_recount(t);
    user_vma_refresh_mmap_hint(t);
    return 0;
}

static uint64_t user_vma_find_topdown_gap(task_t *t, uint64_t floor, uint64_t top, uint64_t need) {
    uint64_t cursor;
    if (!t || need == 0) return 0;
    if (top <= floor || need > (top - floor)) return 0;
    cursor = top;
    for (int iter = 0; iter < (PROCESS_USER_VMA_MAX + 4); ++iter) {
        uint64_t base;
        uint64_t end;
        uint64_t next_cursor = cursor;
        int overlap = 0;
        if (cursor <= floor || need > (cursor - floor)) return 0;
        base = (cursor - need) & ~(PAGE_SIZE - 1ULL);
        if (base < floor) base = floor;
        end = base + need;
        if (end > top || end < base) return 0;
        for (int i = 0; i < PROCESS_USER_VMA_MAX; ++i) {
            edge_user_vma_t *v = &t->user_vmas[i];
            if (v->end <= v->start) continue;
            if (v->end <= floor || v->start >= top) continue;
            if (end <= v->start || base >= v->end) continue;
            overlap = 1;
            if (v->start < next_cursor) next_cursor = v->start;
        }
        if (!overlap) return base;
        if (next_cursor >= cursor) return 0;
        cursor = next_cursor;
    }
    return 0;
}

static int user_vma_remove_range(task_t *t, uint64_t start, uint64_t end) {
    int changed = 0;
    if (!t || end <= start) return 0;
    for (int i = 0; i < PROCESS_USER_VMA_MAX; ++i) {
        edge_user_vma_t *v = &t->user_vmas[i];
        if (v->end <= v->start) continue;
        if (end <= v->start || start >= v->end) continue;
        changed = 1;
        if (start <= v->start && end >= v->end) {
            memset(v, 0, sizeof(*v));
            continue;
        }
        if (start <= v->start) {
            v->start = end;
            continue;
        }
        if (end >= v->end) {
            v->end = start;
            continue;
        }
        /* Split the VMA if possible; otherwise keep the lower half. */
        int slot = user_vma_find_free_slot(t);
        if (slot >= 0) {
            t->user_vmas[slot].start = end;
            t->user_vmas[slot].end = v->end;
            t->user_vmas[slot].prot = v->prot;
            t->user_vmas[slot].flags = v->flags;
        }
        v->end = start;
    }
    if (changed) {
        user_vma_recount(t);
        user_vma_refresh_mmap_hint(t);
    }
    return changed;
}

static uint64_t do_sys_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t off) {
    uint64_t base;
    uint64_t need;
    uint64_t top;
    uint64_t floor;
    static uint8_t mmap_kbuf[4096];
    task_t *cur = process_current_task();
    int trace_py = 0;
    const char *tname = "?";
    if (len == 0) return (uint64_t)-EINVAL;
    if ((int64_t)fd < 0 && (flags & LINUX_MAP_ANON) == 0) return (uint64_t)-EINVAL;
    if (((flags & LINUX_MAP_ANON) == 0) && ((off & (PAGE_SIZE - 1)) != 0)) return (uint64_t)-EINVAL;

    if (!cur) return (uint64_t)-EINVAL;
    tname = cur->name[0] ? cur->name : "?";
    trace_py = 0 && (strcmp(tname, "python3") == 0);
    need = page_align_up(len);
    if (need == 0) return (uint64_t)-EINVAL;

    if ((flags & LINUX_MAP_FIXED) != 0) {
        if ((addr & (PAGE_SIZE - 1)) != 0) return (uint64_t)-EINVAL;
        base = addr;
    } else {
        /* Ignore non-fixed hints: without VMA tracking we cannot safely
         * validate whether a hinted range is already in active use. Use a
         * dedicated top-down mmap arena separate from brk instead. */
        top = cur->user_heap_limit;
        floor = page_align_up(cur->user_brk);
        if (floor < cur->user_heap_base) floor = cur->user_heap_base;
        if (floor < USER_HEAP_BASE_ADDR) floor = USER_HEAP_BASE_ADDR;
        base = user_vma_find_topdown_gap(cur, floor, top, need);
        if (base == 0) return (uint64_t)-ENOMEM;
    }

    if (base < USER_MIN_ADDR || base + need < base || base + need > USER_MAX_ADDR) return (uint64_t)-ENOMEM;
    if (user_vma_remove_range(cur, base, base + need)) {
        /* MAP_FIXED-like replacement for tracked heap-window VMAs. */
    }
    for (uint64_t v = base; v < base + need; v += PAGE_SIZE) {
        uint64_t ptef = user_pte_flags(v);
        if (!(ptef & PTE_PRESENT) || !(ptef & PTE_USER)) return (uint64_t)-ENOMEM;
        if ((prot & LINUX_PROT_WRITE) && !(ptef & PTE_WRITE)) return (uint64_t)-ENOMEM;
    }

    if ((flags & LINUX_MAP_ANON) == 0) {
        edge_fd_proc_t *p = fd_proc_for_pid(process_getpid(), 0);
        edge_fd_t *e = fd_get(p, (int)fd);
        uint64_t done = 0;
        if (!e || e->kind != FD_VFS) return (uint64_t)-EBADF;
        if ((e->inode.mode & 0xF000u) == VFS_INODE_CHR || (e->inode.mode & 0xF000u) == VFS_INODE_BLK) {
            uint64_t maddr = 0;
            uint32_t mlen = 0;
            if (vfs_dev_mmap(e->path, &maddr, &mlen) == 0) {
                if (trace_py) {
                    printf("[mmap] pid=%d %s dev ret=0x%x addr=0x%x len=0x%x prot=0x%x flags=0x%x fd=%d off=0x%x\n",
                           cur->pid, tname, (uint32_t)maddr, (uint32_t)addr, (uint32_t)len, (uint32_t)prot,
                           (uint32_t)flags, (int)fd, (uint32_t)off);
                }
                return maddr;
            }
            return (uint64_t)-ENOSYS;
        }
        memset(mmap_kbuf, 0, sizeof(mmap_kbuf));
        for (uint64_t z = 0; z < need; ) {
            uint64_t zn = need - z;
            if (zn > sizeof(mmap_kbuf)) zn = sizeof(mmap_kbuf);
            if (copy_to_user(base + z, mmap_kbuf, zn) < 0) return (uint64_t)-EFAULT;
            z += zn;
        }
        while (done < len) {
            uint64_t n = len - done;
            int r;
            if (n > 4096) n = 4096;
            if (!e->sb || !e->sb->ops || !e->sb->ops->read) break;
            r = e->sb->ops->read(e->sb, &e->inode, (uint32_t)(off + done), (void *)mmap_kbuf, (uint32_t)n);
            if (r < 0) return (uint64_t)-EIO;
            if (r == 0) break;
            if (copy_to_user(base + done, mmap_kbuf, (uint64_t)r) < 0) return (uint64_t)-EFAULT;
            done += (uint64_t)r;
            if ((uint64_t)r < n) break;
        }
    } else {
        memset(mmap_kbuf, 0, sizeof(mmap_kbuf));
        for (uint64_t z = 0; z < need; ) {
            uint64_t zn = need - z;
            if (zn > sizeof(mmap_kbuf)) zn = sizeof(mmap_kbuf);
            if (copy_to_user(base + z, mmap_kbuf, zn) < 0) return (uint64_t)-EFAULT;
            z += zn;
        }
    }

    if (base >= USER_HEAP_BASE_ADDR && base < cur->user_heap_limit) {
        if (user_vma_record(cur, base, base + need, (uint32_t)prot, (uint32_t)flags) < 0) {
            return (uint64_t)-ENOMEM;
        }
    }
    if (trace_py) {
        printf("[mmap] pid=%d %s ret=0x%x addr=0x%x len=0x%x need=0x%x prot=0x%x flags=0x%x fd=%d off=0x%x next=0x%x\n",
               cur->pid, tname, (uint32_t)base, (uint32_t)addr, (uint32_t)len, (uint32_t)need, (uint32_t)prot,
               (uint32_t)flags, (int)fd, (uint32_t)off, (uint32_t)cur->user_mmap_next);
    }
    return base;
}

static uint64_t do_sys_munmap(uint64_t addr, uint64_t len) {
    task_t *cur = process_current_task();
    uint64_t need;
    int trace_py = 0;
    const char *tname = "?";
    if (!cur) return (uint64_t)-EINVAL;
    tname = cur->name[0] ? cur->name : "?";
    trace_py = 0 && (strcmp(tname, "python3") == 0);
    if (len == 0) return 0;
    if ((addr & (PAGE_SIZE - 1)) != 0) return (uint64_t)-EINVAL;
    need = page_align_up(len);
    if (need == 0) return (uint64_t)-EINVAL;

    if (addr >= USER_HEAP_BASE_ADDR && addr < cur->user_heap_limit) {
        (void)user_vma_remove_range(cur, addr, addr + need);
        if (trace_py) {
            printf("[munmap] pid=%d %s addr=0x%x len=0x%x need=0x%x next=0x%x\n",
                   cur->pid, tname, (uint32_t)addr, (uint32_t)len, (uint32_t)need, (uint32_t)cur->user_mmap_next);
        }
        return 0;
    }
    if (trace_py) {
        printf("[munmap] pid=%d %s addr=0x%x len=0x%x need=0x%x next=0x%x (noop)\n",
               cur->pid, tname, (uint32_t)addr, (uint32_t)len, (uint32_t)need, (uint32_t)cur->user_mmap_next);
    }
    return 0;
}

static uint64_t do_sys_mprotect(uint64_t addr, uint64_t len, uint64_t prot) {
    if ((addr & (PAGE_SIZE - 1)) != 0) return (uint64_t)-EINVAL;
    if (len == 0) return 0;

    uint64_t end = page_align_up(addr + len);
    if (end < addr) return (uint64_t)-EINVAL;
    for (uint64_t v = addr; v < end; v += PAGE_SIZE) {
        uint64_t ptef = user_pte_flags(v);
        if (!(ptef & PTE_PRESENT) || !(ptef & PTE_USER)) return (uint64_t)-ENOMEM;
        if ((prot & LINUX_PROT_WRITE) && !(ptef & PTE_WRITE)) return (uint64_t)-ENOMEM;
    }
    return 0;
}

static uint64_t do_sys_uname(uint64_t buf_u) {
    struct edge_utsname u;
    const char *hn = lwip_stack_get_hostname();
    if (!buf_u) return (uint64_t)-EINVAL;
    memset(&u, 0, sizeof(u));
    strcpy(u.sysname, "EdgeOS");
    strcpy(u.nodename, (hn && hn[0]) ? hn : "edgeos");
    strcpy(u.release, "0.02");
    strcpy(u.version, "edgeos");
    strcpy(u.machine, "x86_64");
    strcpy(u.domainname, "localdomain");
    if (copy_to_user(buf_u, &u, sizeof(u)) < 0) return (uint64_t)-EFAULT;
    return 0;
}

static uint64_t do_sys_sethostname(uint64_t name_u, uint64_t len_u) {
    char host[65];
    char filebuf[72];
    uint64_t n = len_u;
    int i;
    int out = 0;
    task_t *t = process_current_task();

    if (t && t->euid != 0) return (uint64_t)-EPERM;
    if (!name_u) return (uint64_t)-EFAULT;
    if (n > 64) return (uint64_t)-EINVAL;
    if (n == 0) return (uint64_t)-EINVAL;
    if (!user_range_ok(name_u, n)) return (uint64_t)-EFAULT;
    if (copy_from_user(host, name_u, n) < 0) return (uint64_t)-EFAULT;
    host[n] = 0;
    for (i = 0; i < (int)n; ++i) {
        if (host[i] == 0) break;
        if (out >= (int)sizeof(filebuf) - 2) break;
        filebuf[out++] = host[i];
    }
    filebuf[out] = 0;
    if (lwip_stack_set_hostname(filebuf) < 0) return (uint64_t)-EINVAL;
    filebuf[out++] = '\n';
    filebuf[out] = 0;
    (void)vfs_write_file("/etc/hostname", filebuf, (uint32_t)out);
    return 0;
}

static uint64_t do_sys_clock_gettime(uint64_t clk_id, uint64_t ts_u) {
    struct edge_timespec ts;
    uint64_t us;
    if (clk_id == CLOCK_REALTIME) us = boottime_realtime_us();
    else if (clk_id == CLOCK_MONOTONIC) us = boottime_monotonic_us();
    else return (uint64_t)-EINVAL;
    ts.tv_sec = (int64_t)(us / 1000000ull);
    ts.tv_nsec = (int64_t)((us % 1000000ull) * 1000ull);
    if (!ts_u) return (uint64_t)-EINVAL;
    if (copy_to_user(ts_u, &ts, sizeof(ts)) < 0) return (uint64_t)-EFAULT;
    return 0;
}

static uint64_t do_sys_gettimeofday(uint64_t tv_u, uint64_t tz_u) {
    struct edge_timeval tv;
    uint64_t us = boottime_realtime_us();
    (void)tz_u;
    if (!tv_u) return (uint64_t)-EINVAL;
    tv.tv_sec = (int64_t)(us / 1000000ull);
    tv.tv_usec = (int64_t)(us % 1000000ull);
    if (copy_to_user(tv_u, &tv, sizeof(tv)) < 0) return (uint64_t)-EFAULT;
    return 0;
}

static uint64_t do_sys_nanosleep(uint64_t req_u) {
    struct edge_timespec req;
    uint64_t ms;
    if (!req_u) return (uint64_t)-EINVAL;
    if (copy_from_user(&req, req_u, sizeof(req)) < 0) return (uint64_t)-EFAULT;
    if (req.tv_sec < 0 || req.tv_nsec < 0) return (uint64_t)-EINVAL;
    ms = (uint64_t)req.tv_sec * 1000ULL + (uint64_t)(req.tv_nsec / 1000000ULL);
    return do_sys_sleep(ms);
}

static uint64_t do_sys_clock_nanosleep(uint64_t clk_id_u, uint64_t flags_u, uint64_t req_u, uint64_t rem_u) {
    struct edge_timespec req;
    uint64_t start_us;
    uint64_t target_us;
    uint64_t timeout_us;
    uint64_t base_now;
    int ok = 0;

    if (flags_u & (uint64_t)~TIMER_ABSTIME) return (uint64_t)-EINVAL;
    if (clk_id_u != CLOCK_REALTIME && clk_id_u != CLOCK_MONOTONIC) return (uint64_t)-EINVAL;
    if (!req_u) return (uint64_t)-EINVAL;
    if (copy_from_user(&req, req_u, sizeof(req)) < 0) return (uint64_t)-EFAULT;

    timeout_us = timespec_to_us_checked(&req, &ok);
    if (!ok) return (uint64_t)-EINVAL;

    if (flags_u & TIMER_ABSTIME) {
        base_now = (clk_id_u == CLOCK_REALTIME) ? boottime_realtime_us() : boottime_monotonic_us();
        if (timeout_us <= base_now) return 0;
        timeout_us -= base_now;
    }
    start_us = boottime_monotonic_us();
    target_us = start_us + timeout_us;

    for (;;) {
        struct edge_timespec rem = {0, 0};
        uint64_t now_us = boottime_monotonic_us();
        if (now_us >= target_us) return 0;
        if (signal_pending_interrupt()) {
            if (rem_u && ((flags_u & TIMER_ABSTIME) == 0)) {
                uint64_t left_us = target_us - now_us;
                rem.tv_sec = (int64_t)(left_us / 1000000ull);
                rem.tv_nsec = (int64_t)((left_us % 1000000ull) * 1000ull);
                if (copy_to_user(rem_u, &rem, sizeof(rem)) < 0) return (uint64_t)-EFAULT;
            }
            return (uint64_t)-EINTR;
        }
        lwip_stack_poll();
        wait_blocking_step();
    }
}

static uint64_t do_sys_sched_yield(void) {
    scheduler_yield();
    return 0;
}

static uint64_t do_sys_gettid(void) {
    return (uint64_t)process_getpid();
}

static uint64_t do_sys_tgkill(uint64_t tgid_u, uint64_t tid_u, uint64_t sig_u) {
    int tgid = (int)tgid_u;
    int tid = (int)tid_u;
    int sig = (int)sig_u;
    if (tgid <= 0 || tid <= 0 || sig <= 0) return (uint64_t)-EINVAL;
    if (tgid != tid) return (uint64_t)-ESRCH;
    return process_send_signal(tid, sig) == 0 ? 0 : (uint64_t)-ESRCH;
}

static uint64_t do_sys_fchdir(uint64_t fd_u) {
    int fd = (int)fd_u;
    edge_fd_proc_t *p = fd_proc_with_stdio();
    edge_fd_t *e = fd_get(p, fd);
    if (!e) return (uint64_t)-EBADF;
    if (e->kind != FD_VFS) return (uint64_t)-ENOTDIR;
    if ((e->inode.mode & 0xF000u) != VFS_INODE_DIR) return (uint64_t)-ENOTDIR;
    return vfs_chdir(e->path) == 0 ? 0 : (uint64_t)-ENOENT;
}

static uint64_t do_sys_getrlimit(uint64_t resource_u, uint64_t rlim_u) {
    return do_sys_prlimit64(0, resource_u, 0, rlim_u);
}

static uint64_t do_sys_setrlimit(uint64_t resource_u, uint64_t rlim_u) {
    return do_sys_prlimit64(0, resource_u, rlim_u, 0);
}

static uint64_t do_sys_getrusage(uint64_t who_u, uint64_t usage_u) {
    struct edge_linux_rusage ru;
    int who = (int)who_u;
    if (!usage_u) return (uint64_t)-EINVAL;
    if (who != LINUX_RUSAGE_SELF && who != LINUX_RUSAGE_CHILDREN) return (uint64_t)-EINVAL;
    memset(&ru, 0, sizeof(ru));
    if (copy_to_user(usage_u, &ru, sizeof(ru)) < 0) return (uint64_t)-EFAULT;
    return 0;
}

static uint64_t do_sys_sigaltstack(uint64_t new_u, uint64_t old_u) {
    task_t *t = process_current_task();
    struct edge_linux_stack_t st;
    if (!t) return (uint64_t)-EINVAL;

    if (old_u) {
        memset(&st, 0, sizeof(st));
        st.ss_sp = t->sigaltstack_sp;
        st.ss_size = t->sigaltstack_size;
        st.ss_flags = (int32_t)t->sigaltstack_flags;
        if (copy_to_user(old_u, &st, sizeof(st)) < 0) return (uint64_t)-EFAULT;
    }

    if (new_u) {
        if (copy_from_user(&st, new_u, sizeof(st)) < 0) return (uint64_t)-EFAULT;
        if (st.ss_flags & ~(LINUX_SS_ONSTACK | LINUX_SS_DISABLE)) return (uint64_t)-EINVAL;
        if (st.ss_flags & LINUX_SS_ONSTACK) return (uint64_t)-EINVAL;
        if (st.ss_flags & LINUX_SS_DISABLE) {
            t->sigaltstack_sp = 0;
            t->sigaltstack_size = 0;
            t->sigaltstack_flags = LINUX_SS_DISABLE;
        } else {
            if (!st.ss_sp || st.ss_size < LINUX_MINSIGSTKSZ) return (uint64_t)-ENOMEM;
            if (!user_range_ok(st.ss_sp, st.ss_size)) return (uint64_t)-EFAULT;
            t->sigaltstack_sp = st.ss_sp;
            t->sigaltstack_size = st.ss_size;
            t->sigaltstack_flags = 0;
        }
    }
    return 0;
}

static uint64_t do_sys_madvise(uint64_t addr_u, uint64_t len_u, uint64_t advice_u) {
    (void)addr_u;
    (void)len_u;
    (void)advice_u;
    /* Advisory only. Keep permissive for libc allocators. */
    return 0;
}

static uint64_t do_sys_mremap(uint64_t old_addr_u, uint64_t old_size_u, uint64_t new_size_u, uint64_t flags_u, uint64_t new_addr_u) {
    uint64_t old_addr = old_addr_u;
    uint64_t old_size = page_align_up(old_size_u);
    uint64_t new_size = page_align_up(new_size_u);
    (void)new_addr_u;
    task_t *cur = process_current_task();
    const char *tname = (cur && cur->name[0]) ? cur->name : "?";
    int trace_py = 0 && (cur && strcmp(tname, "python3") == 0);
    if ((old_addr & (PAGE_SIZE - 1)) != 0) return (uint64_t)-EINVAL;
    if (old_size == 0 || new_size == 0) return (uint64_t)-EINVAL;
    if ((flags_u & LINUX_MREMAP_FIXED) && ((flags_u & LINUX_MREMAP_MAYMOVE) == 0)) return (uint64_t)-EINVAL;
    if (flags_u & ~(uint64_t)(LINUX_MREMAP_MAYMOVE | LINUX_MREMAP_FIXED)) return (uint64_t)-EINVAL;
    if (!user_range_ok(old_addr, old_size)) return (uint64_t)-EFAULT;
    if (new_size <= old_size) {
        if (trace_py) {
            printf("[mremap] pid=%d %s old=0x%x oldsz=0x%x newsz=0x%x flags=0x%x ret=0x%x\n",
                   cur->pid, tname, (uint32_t)old_addr_u, (uint32_t)old_size_u, (uint32_t)new_size_u,
                   (uint32_t)flags_u, (uint32_t)old_addr);
        }
        return old_addr;
    }
    /*
     * Do not provide partial/non-Linux growth semantics yet.
     * Returning ENOSYS keeps libc realloc fallback paths reliable.
     */
    if (trace_py) {
        printf("[mremap] pid=%d %s old=0x%x oldsz=0x%x newsz=0x%x flags=0x%x ret=-ENOSYS\n",
               cur->pid, tname, (uint32_t)old_addr_u, (uint32_t)old_size_u, (uint32_t)new_size_u, (uint32_t)flags_u);
    }
    return (uint64_t)-ENOSYS;
}

static uint64_t do_sys_futex(uint64_t uaddr_u, uint64_t op_u, uint64_t val_u, uint64_t timeout_u, uint64_t uaddr2_u, uint64_t val3_u) {
    uint32_t op = (uint32_t)op_u;
    uint32_t cmd = op & LINUX_FUTEX_CMD_MASK;
    int32_t curv = 0;
    int32_t expect = (int32_t)val_u;
    (void)uaddr2_u;
    (void)val3_u;
    if ((uaddr_u & 3u) != 0) return (uint64_t)-EINVAL;
    if (!user_range_ok(uaddr_u, sizeof(int32_t))) return (uint64_t)-EFAULT;

    if (cmd == LINUX_FUTEX_WAKE || cmd == LINUX_FUTEX_WAKE_BITSET) {
        return (uint64_t)(int32_t)val_u;
    }
    if (cmd != LINUX_FUTEX_WAIT && cmd != LINUX_FUTEX_WAIT_BITSET) return (uint64_t)-ENOSYS;

    if (copy_from_user(&curv, uaddr_u, sizeof(curv)) < 0) return (uint64_t)-EFAULT;
    if (curv != expect) return (uint64_t)-EAGAIN;

    {
        uint64_t deadline_us = 0;
        if (timeout_u) {
            struct edge_timespec ts;
            int ok = 0;
            uint64_t dur_us;
            if (copy_from_user(&ts, timeout_u, sizeof(ts)) < 0) return (uint64_t)-EFAULT;
            if (cmd == LINUX_FUTEX_WAIT_BITSET && (op & LINUX_FUTEX_CLOCK_REALTIME)) {
                uint64_t abs_us = 0;
                dur_us = timespec_to_us_checked(&ts, &ok);
                if (!ok) return (uint64_t)-EINVAL;
                abs_us = boottime_realtime_us();
                if (dur_us <= abs_us) return (uint64_t)-ETIMEDOUT;
                deadline_us = boottime_monotonic_us() + (dur_us - abs_us);
            } else {
                dur_us = timespec_to_us_checked(&ts, &ok);
                if (!ok) return (uint64_t)-EINVAL;
                deadline_us = boottime_monotonic_us() + dur_us;
            }
        }
        for (;;) {
            uint64_t now_us;
            if (copy_from_user(&curv, uaddr_u, sizeof(curv)) < 0) return (uint64_t)-EFAULT;
            if (curv != expect) return 0;
            if (signal_pending_interrupt()) return (uint64_t)-EINTR;
            if (deadline_us) {
                now_us = boottime_monotonic_us();
                if (now_us >= deadline_us) return (uint64_t)-ETIMEDOUT;
            }
            lwip_stack_poll();
            wait_blocking_step();
        }
    }
}

static uint64_t do_sys_rt_sigaction(uint64_t sig_u, uint64_t act_u, uint64_t old_u, uint64_t sigsetsize_u) {
    task_t *t = process_current_task();
    struct edge_linux_sigaction sa;
    uint64_t *handler_slot = 0;
    if (!t) return (uint64_t)-EINVAL;
    if (sigsetsize_u != 8) return (uint64_t)-EINVAL;
    if (sig_u == LINUX_SIGALRM) handler_slot = &t->sigalrm_handler;
    else if (sig_u == LINUX_SIGINT) handler_slot = &t->sigint_handler;
    else if (sig_u == LINUX_SIGTERM) handler_slot = &t->sigterm_handler;
    else if (sig_u == LINUX_SIGCHLD) handler_slot = &t->sigchld_handler;
    else return 0;
    if (old_u) {
        memset(&sa, 0, sizeof(sa));
        sa.handler = *handler_slot;
        if (copy_to_user(old_u, &sa, sizeof(sa)) < 0) return (uint64_t)-EFAULT;
    }
    if (act_u) {
        if (copy_from_user(&sa, act_u, sizeof(sa)) < 0) return (uint64_t)-EFAULT;
        *handler_slot = sa.handler;
    }
    return 0;
}

static uint64_t do_sys_rt_sigprocmask(uint64_t how_u, uint64_t set_u, uint64_t oldset_u, uint64_t sigsetsize_u) {
    task_t *t = process_current_task();
    if (!t) return (uint64_t)-EINVAL;
    if (sigsetsize_u != 8) return (uint64_t)-EINVAL;
    (void)how_u;
    (void)set_u;
    /* Compatibility mode: report current mask but ignore mutations until a full
     * Linux-compatible user signal frame/rt_sigreturn path is stable. */
    if (oldset_u) {
        if (copy_to_user(oldset_u, &t->sigmask, sizeof(t->sigmask)) < 0) return (uint64_t)-EFAULT;
    }
    return 0;
}

static uint64_t do_sys_setitimer(uint64_t which_u, uint64_t new_u, uint64_t old_u) {
    task_t *t = process_current_task();
    struct edge_itimerval oldv;
    struct edge_itimerval newv;
    uint64_t now;
    uint64_t val_us;
    uint64_t int_us;

    if (!t) return (uint64_t)-EINVAL;
    if (which_u != LINUX_ITIMER_REAL) return (uint64_t)-EINVAL;
    if (!new_u && !old_u) return (uint64_t)-EINVAL;

    now = boottime_monotonic_us();
    memset(&oldv, 0, sizeof(oldv));
    if (t->itimer_real_active && t->itimer_real_next_us > now) {
        us_to_timeval(t->itimer_real_next_us - now, &oldv.it_value);
    }
    us_to_timeval(t->itimer_real_interval_us, &oldv.it_interval);
    if (old_u && copy_to_user(old_u, &oldv, sizeof(oldv)) < 0) return (uint64_t)-EFAULT;

    if (!new_u) return 0;
    if (copy_from_user(&newv, new_u, sizeof(newv)) < 0) return (uint64_t)-EFAULT;
    val_us = timeval_to_us(&newv.it_value);
    int_us = timeval_to_us(&newv.it_interval);
    t->itimer_real_interval_us = int_us;
    t->sigalrm_pending = 0;
    if (val_us == 0) {
        t->itimer_real_active = 0;
        t->itimer_real_next_us = 0;
    } else {
        t->itimer_real_active = 1;
        t->itimer_real_next_us = now + val_us;
    }
    return 0;
}

static uint64_t do_sys_alarm(uint64_t seconds_u) {
    task_t *t = process_current_task();
    uint64_t now;
    uint64_t old_secs = 0;
    uint64_t seconds = seconds_u;
    if (!t) return (uint64_t)-EINVAL;
    now = boottime_monotonic_us();
    if (t->itimer_real_active && t->itimer_real_next_us > now) {
        uint64_t rem_us = t->itimer_real_next_us - now;
        old_secs = (rem_us + 999999ull) / 1000000ull;
    }
    t->itimer_real_interval_us = 0;
    t->sigalrm_pending = 0;
    if (seconds == 0) {
        t->itimer_real_active = 0;
        t->itimer_real_next_us = 0;
    } else {
        t->itimer_real_active = 1;
        t->itimer_real_next_us = now + seconds * 1000000ull;
    }
    return old_secs;
}

static int install_user_sig_stub(task_t *t) {
    static const uint8_t code[2] = {0x58, 0xC3}; /* pop rax; ret */
    if (!t) return -1;
    if (t->sig_stub_installed) return 0;
    if (copy_to_user(EDGE_SIGTRAMP_ADDR, code, sizeof(code)) < 0) return -1;
    t->sig_stub_installed = 1;
    return 0;
}

static void maybe_deliver_signal_on_sysret(REGISTERS *r) {
    task_t *t = process_current_task();
    uint64_t blocked = 0;
    uint64_t handler = LINUX_SIG_DFL;
    uint64_t sig = 0;
    uint8_t *pending = 0;
    uint64_t old_rsp, old_rip, saved_rax, new_rsp;
    uint64_t frame[3];

    if (!t || !r) return;
    blocked = t->sigmask;
    task_timer_poll(t);

    if (t->sigint_pending && ((blocked & (1ull << (LINUX_SIGINT - 1))) == 0)) {
        sig = LINUX_SIGINT; handler = t->sigint_handler; pending = &t->sigint_pending;
    } else if (t->sigterm_pending && ((blocked & (1ull << (LINUX_SIGTERM - 1))) == 0)) {
        sig = LINUX_SIGTERM; handler = t->sigterm_handler; pending = &t->sigterm_pending;
    } else if (t->sigalrm_pending && ((blocked & (1ull << (LINUX_SIGALRM - 1))) == 0)) {
        sig = LINUX_SIGALRM; handler = t->sigalrm_handler; pending = &t->sigalrm_pending;
    } else if (t->sigchld_pending && ((blocked & (1ull << (LINUX_SIGCHLD - 1))) == 0)) {
        sig = LINUX_SIGCHLD; handler = t->sigchld_handler; pending = &t->sigchld_pending;
    } else {
        return;
    }

    if (handler == LINUX_SIG_IGN) {
        if (pending) *pending = 0;
        return;
    }
    if (handler == LINUX_SIG_DFL) {
        if (pending) *pending = 0;
        if (sig == LINUX_SIGINT || sig == LINUX_SIGTERM) {
            fd_proc_release(process_getpid());
            scheduler_kill_current_and_yield(128 + (int)sig);
        }
        return;
    }
    if (!user_range_ok(handler, 1)) return;
    if (install_user_sig_stub(t) < 0) return;

    old_rsp = r->rsp;
    old_rip = r->rip;
    saved_rax = r->rax;
    if (old_rsp < USER_MIN_ADDR + 24 || old_rsp > USER_MAX_ADDR) return;
    new_rsp = old_rsp - 24;
    if (!user_range_ok(new_rsp, 24)) return;

    frame[0] = EDGE_SIGTRAMP_ADDR;
    frame[1] = saved_rax;
    frame[2] = old_rip;
    if (copy_to_user(new_rsp, frame, sizeof(frame)) < 0) return;

    if (pending) *pending = 0;
    r->rsp = new_rsp;
    r->rip = handler;
    r->rdi = sig;
}

static uint64_t do_sys_access(uint64_t path_u, uint64_t mode) {
    char path[256];
    vfs_inode_t ino;
    if (!path_u) return (uint64_t)-EINVAL;
    if (copy_user_cstr(path, sizeof(path), path_u) < 0) return (uint64_t)-EFAULT;
    if (vfs_resolve(path, &ino, 0, 0, 0) != 0) return (uint64_t)-ENOENT;
    if (mode == 0) return 0;
    return vfs_permission_check(&ino, (int)(mode & 7u), process_current_task()) == 0 ? 0 : (uint64_t)-EACCES;
}

static uint64_t do_sys_arch_prctl(uint64_t code, uint64_t addr_u) {
    if (code == ARCH_SET_FS) {
        return process_set_fs_base(addr_u) == 0 ? 0 : (uint64_t)-EINVAL;
    }
    if (code == ARCH_GET_FS) {
        uint64_t fs = process_get_fs_base();
        if (!addr_u) return (uint64_t)-EINVAL;
        if (copy_to_user(addr_u, &fs, sizeof(fs)) < 0) return (uint64_t)-EFAULT;
        return 0;
    }
    return (uint64_t)-EINVAL;
}

static uint64_t do_sys_prlimit64(uint64_t pid, uint64_t resource, uint64_t new_u, uint64_t old_u) {
    static struct edge_linux_rlimit nofile_lim = {EDGE_MAX_FD, EDGE_MAX_FD};
    static struct edge_linux_rlimit default_lim = {LINUX_RLIM_INFINITY, LINUX_RLIM_INFINITY};
    struct edge_linux_rlimit lim;
    if (pid != 0 && pid != (uint64_t)process_getpid()) return (uint64_t)-EPERM;
    lim = (resource == LINUX_RLIMIT_NOFILE) ? nofile_lim : default_lim;
    if (old_u) {
        if (copy_to_user(old_u, &lim, sizeof(lim)) < 0) return (uint64_t)-EFAULT;
    }
    if (new_u) {
        if (copy_from_user(&lim, new_u, sizeof(lim)) < 0) return (uint64_t)-EFAULT;
        if (resource == LINUX_RLIMIT_NOFILE) {
            if (lim.rlim_cur > lim.rlim_max) return (uint64_t)-EINVAL;
            if (lim.rlim_max > EDGE_MAX_FD) lim.rlim_max = EDGE_MAX_FD;
            if (lim.rlim_cur > lim.rlim_max) lim.rlim_cur = lim.rlim_max;
            nofile_lim = lim;
        } else {
            /* Accept and ignore unknown limits for Linux userspace compatibility. */
            default_lim = lim;
        }
    }
    return 0;
}

static void syscall_trace(uint64_t nr, int64_t ret) {
    task_t *t = process_current_task();
    int pid = t ? t->pid : -1;
    const char *name = (t && t->name[0]) ? t->name : "?";
    if (!EDGE_SYSCALL_DEBUG) {
        return;
    }
    if (ret < 0) {
        printf("[syscall] pid=%d cmd=%s nr=%u ret=%d errno=%d\n",
               pid, name, (uint32_t)nr, (int32_t)ret, (int32_t)(-ret));
    } else {
        printf("[syscall] pid=%d cmd=%s nr=%u ret=%d\n",
               pid, name, (uint32_t)nr, (int32_t)ret);
    }
}

static uint64_t do_sys_execveat(uint64_t dirfd_u, uint64_t path_u, uint64_t argv_u, uint64_t envp_u, uint64_t flags_u) {
    int dirfd = (int)dirfd_u;
    int flags = (int)flags_u;
    char path_in[256];
    if ((flags & ~(LINUX_AT_EMPTY_PATH | LINUX_AT_SYMLINK_NOFOLLOW)) != 0) return (uint64_t)-EINVAL;
    if (!path_u) return (uint64_t)-EINVAL;
    if (copy_user_cstr(path_in, sizeof(path_in), path_u) < 0) return (uint64_t)-EFAULT;
    if ((flags & LINUX_AT_EMPTY_PATH) && path_in[0] == 0) {
        edge_fd_proc_t *p = fd_proc_with_stdio();
        edge_fd_t *e = fd_get(p, dirfd);
        if (!e || e->kind != FD_VFS || !e->path[0]) return (uint64_t)-EBADF;
        /* Full dirfd/AT_EMPTY_PATH exec needs a kpath helper; report unsupported for now. */
        return (uint64_t)-ENOSYS;
    }
    if (dirfd != LINUX_AT_FDCWD && path_in[0] != '/') return (uint64_t)-ENOSYS;
    return do_sys_execve(path_u, argv_u, envp_u);
}

static uint64_t do_sys_renameat(uint64_t olddirfd_u, uint64_t old_u, uint64_t newdirfd_u, uint64_t new_u) {
    int olddirfd = (int)olddirfd_u;
    int newdirfd = (int)newdirfd_u;
    char old_in[256], new_in[256], oldp[256], newp[256];
    static char tmp[131072];
    vfs_inode_t oldino, newino;
    int n;
    if (!old_u || !new_u) return (uint64_t)-EINVAL;
    if (copy_user_cstr(old_in, sizeof(old_in), old_u) < 0) return (uint64_t)-EFAULT;
    if (copy_user_cstr(new_in, sizeof(new_in), new_u) < 0) return (uint64_t)-EFAULT;
    if (build_at_path(olddirfd, old_in, oldp, (int)sizeof(oldp)) < 0) return (uint64_t)-EINVAL;
    if (build_at_path(newdirfd, new_in, newp, (int)sizeof(newp)) < 0) return (uint64_t)-EINVAL;
    if (vfs_resolve(oldp, &oldino, 0, 0, 0) < 0) return (uint64_t)-ENOENT;
    if ((oldino.mode & 0xF000u) == VFS_INODE_DIR) return (uint64_t)-EISDIR;
    if (vfs_resolve(newp, &newino, 0, 0, 0) == 0 && (newino.mode & 0xF000u) == VFS_INODE_DIR) return (uint64_t)-EISDIR;
    n = vfs_read_file(oldp, tmp, sizeof(tmp));
    if (n < 0) return (uint64_t)-EIO;
    if (vfs_write_file(newp, tmp, (uint32_t)n) < 0) return (uint64_t)-EXDEV;
    if (vfs_unlink(oldp) < 0) return (uint64_t)-EIO;
    return 0;
}

static uint64_t do_sys_renameat2(uint64_t olddirfd_u, uint64_t old_u, uint64_t newdirfd_u, uint64_t new_u, uint64_t flags_u) {
    if (flags_u != 0) return (uint64_t)-EOPNOTSUPP;
    return do_sys_renameat(olddirfd_u, old_u, newdirfd_u, new_u);
}

static uint64_t do_sys_truncate(uint64_t path_u, uint64_t len_u) {
    char path[256];
    vfs_inode_t ino;
    int rc;
    if (!path_u) return (uint64_t)-EINVAL;
    if ((int64_t)len_u < 0) return (uint64_t)-EINVAL;
    if (copy_user_cstr(path, sizeof(path), path_u) < 0) return (uint64_t)-EFAULT;
    if (vfs_resolve(path, &ino, 0, 0, 0) < 0) return (uint64_t)-ENOENT;
    if ((ino.mode & 0xF000u) == VFS_INODE_DIR) return (uint64_t)-EISDIR;
    rc = truncate_vfs_path_to_len(path, len_u);
    return rc == 0 ? 0 : (uint64_t)(int64_t)rc;
}

static uint64_t do_sys_ftruncate(uint64_t fd_u, uint64_t len_u) {
    edge_fd_proc_t *p = fd_proc_with_stdio();
    edge_fd_t *e = fd_get(p, (int)fd_u);
    int rc;
    if (!e) return (uint64_t)-EBADF;
    if (e->kind != FD_VFS || !e->path[0]) return (uint64_t)-EINVAL;
    if ((int64_t)len_u < 0) return (uint64_t)-EINVAL;
    rc = truncate_vfs_path_to_len(e->path, len_u);
    if (rc == 0 && e->pos > len_u) e->pos = len_u;
    return rc == 0 ? 0 : (uint64_t)(int64_t)rc;
}

static uint64_t do_sys_prctl(uint64_t opt_u, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    task_t *t = process_current_task();
    (void)a3; (void)a4; (void)a5;
    if (!t) return (uint64_t)-EINVAL;
    switch ((int)opt_u) {
        case LINUX_PR_SET_NAME: {
            char name[16];
            if (!a2) return (uint64_t)-EINVAL;
            if (copy_from_user(name, a2, sizeof(name)) < 0) return (uint64_t)-EFAULT;
            name[15] = 0;
            strncpy(t->name, name, TASK_NAME_MAX - 1);
            t->name[TASK_NAME_MAX - 1] = 0;
            return 0;
        }
        case LINUX_PR_GET_NAME: {
            char name[16];
            memset(name, 0, sizeof(name));
            strncpy(name, t->name, sizeof(name) - 1);
            if (!a2) return (uint64_t)-EINVAL;
            if (copy_to_user(a2, name, sizeof(name)) < 0) return (uint64_t)-EFAULT;
            return 0;
        }
        case LINUX_PR_SET_PDEATHSIG:
            return 0;
        case LINUX_PR_GET_PDEATHSIG: {
            int sig = 0;
            if (!a2) return (uint64_t)-EINVAL;
            if (copy_to_user(a2, &sig, sizeof(sig)) < 0) return (uint64_t)-EFAULT;
            return 0;
        }
        case LINUX_PR_GET_DUMPABLE:
            return 1;
        case LINUX_PR_SET_DUMPABLE:
            return 0;
        case LINUX_PR_SET_NO_NEW_PRIVS:
            return a2 == 1 ? 0 : (uint64_t)-EINVAL;
        case LINUX_PR_GET_NO_NEW_PRIVS:
            return 1;
        case LINUX_PR_CAPBSET_READ:
            return 0;
        default:
            return (uint64_t)-EINVAL;
    }
}

static uint64_t do_sys_rt_sigreturn(void) {
    return (uint64_t)-ENOSYS;
}

static uint64_t do_sys_waitid(uint64_t idtype_u, uint64_t id_u, uint64_t info_u, uint64_t options_u, uint64_t ru_u) {
    int idtype = (int)idtype_u;
    int options = (int)options_u;
    int pid = -1;
    int status = 0;
    int rc;
    struct edge_linux_siginfo_min si;
    (void)ru_u;
    if (!(options & LINUX_WEXITED)) return (uint64_t)-EINVAL;
    if (options & LINUX_WNOWAIT) return (uint64_t)-EOPNOTSUPP;
    if (idtype == LINUX_WAITID_P_ALL) pid = -1;
    else if (idtype == LINUX_WAITID_P_PID) pid = (int)id_u;
    else if (idtype == LINUX_WAITID_P_PGID) pid = (id_u == 0) ? 0 : -(int)id_u;
    else return (uint64_t)-EINVAL;
    rc = process_wait_pid(pid, &status, (options & LINUX_WNOHANG) ? LINUX_WNOHANG : 0);
    if (rc < 0) return (uint64_t)-ECHILD;
    if (rc == 0) {
        if (info_u) {
            memset(&si, 0, sizeof(si));
            if (copy_to_user(info_u, &si, sizeof(si)) < 0) return (uint64_t)-EFAULT;
        }
        return 0;
    }
    if (info_u) {
        memset(&si, 0, sizeof(si));
        si.si_signo = LINUX_SIGCHLD;
        si.si_code = LINUX_CLD_EXITED;
        si.si_pid = rc;
        si.si_uid = 0;
        si.si_status = (status >> 8) & 0xFF;
        if (copy_to_user(info_u, &si, sizeof(si)) < 0) return (uint64_t)-EFAULT;
    }
    return 0;
}

static uint64_t do_sys_sched_getaffinity(uint64_t pid_u, uint64_t cpusetsize_u, uint64_t mask_u) {
    task_t *t = task_by_pid_mutable_local((int)pid_u);
    uint64_t m = 0;
    uint64_t n = cpusetsize_u;
    int cpu;
    if (!mask_u || n == 0) return (uint64_t)-EINVAL;
    if (!t) return (uint64_t)-ESRCH;
    cpu = (t->assigned_cpu >= 0) ? t->assigned_cpu : 0;
    if (cpu >= 64) cpu = 0;
    m = 1ull << (unsigned)cpu;
    if (n > sizeof(m)) n = sizeof(m);
    if (copy_to_user(mask_u, &m, n) < 0) return (uint64_t)-EFAULT;
    return n;
}

static uint64_t do_sys_sched_setaffinity(uint64_t pid_u, uint64_t cpusetsize_u, uint64_t mask_u) {
    task_t *t = task_by_pid_mutable_local((int)pid_u);
    uint64_t m = 0;
    uint64_t n = cpusetsize_u;
    int cpu = 0;
    if (!mask_u || n == 0) return (uint64_t)-EINVAL;
    if (!t) return (uint64_t)-ESRCH;
    if (n > sizeof(m)) n = sizeof(m);
    if (copy_from_user(&m, mask_u, n) < 0) return (uint64_t)-EFAULT;
    if (m == 0) return (uint64_t)-EINVAL;
    while (cpu < 64 && ((m >> cpu) & 1ull) == 0) cpu++;
    if (cpu >= SCHED_MAX_CPUS) cpu = 0;
    t->assigned_cpu = cpu;
    return 0;
}

static uint64_t do_sys_sched_getparam(uint64_t pid_u, uint64_t param_u) {
    struct edge_linux_sched_param p;
    if (!task_by_pid_mutable_local((int)pid_u)) return (uint64_t)-ESRCH;
    if (!param_u) return (uint64_t)-EINVAL;
    memset(&p, 0, sizeof(p));
    if (copy_to_user(param_u, &p, sizeof(p)) < 0) return (uint64_t)-EFAULT;
    return 0;
}

static uint64_t do_sys_sched_setscheduler(uint64_t pid_u, uint64_t policy_u, uint64_t param_u) {
    struct edge_linux_sched_param p;
    int policy = (int)policy_u;
    if (!task_by_pid_mutable_local((int)pid_u)) return (uint64_t)-ESRCH;
    if (!(policy == LINUX_SCHED_OTHER || policy == LINUX_SCHED_FIFO || policy == LINUX_SCHED_RR)) return (uint64_t)-EINVAL;
    if (param_u) {
        if (copy_from_user(&p, param_u, sizeof(p)) < 0) return (uint64_t)-EFAULT;
        if (policy == LINUX_SCHED_OTHER && p.sched_priority != 0) return (uint64_t)-EINVAL;
    }
    return 0;
}

static uint64_t do_sys_getcpu(uint64_t cpu_u, uint64_t node_u, uint64_t tcache_u) {
    uint32_t cpu = scheduler_cpu_id();
    uint32_t node = 0;
    (void)tcache_u;
    if (cpu_u && copy_to_user(cpu_u, &cpu, sizeof(cpu)) < 0) return (uint64_t)-EFAULT;
    if (node_u && copy_to_user(node_u, &node, sizeof(node)) < 0) return (uint64_t)-EFAULT;
    return 0;
}

static uint64_t do_sys_chroot(uint64_t path_u) {
    char path[256];
    vfs_inode_t ino;
    int rc;
    task_t *t = process_current_task();
    if (!t || t->euid != 0) return (uint64_t)-EPERM;
    if (!path_u) return (uint64_t)-EINVAL;
    rc = resolve_user_path(path_u, path, sizeof(path), &ino, 0);
    if (rc < 0) return (uint64_t)rc;
    if ((ino.mode & 0xF000u) != VFS_INODE_DIR) return (uint64_t)-ENOTDIR;
    return vfs_chroot(path) == 0 ? 0 : (uint64_t)-EINVAL;
}

static uint64_t do_sys_pivot_root(uint64_t new_root_u, uint64_t put_old_u) {
    (void)new_root_u;
    (void)put_old_u;
    return (uint64_t)-EINVAL;
}

static uint64_t do_sys_setns(uint64_t fd_u, uint64_t nstype_u) {
    (void)fd_u;
    (void)nstype_u;
    return (uint64_t)-EINVAL;
}

static uint64_t do_sys_unshare(uint64_t flags_u) {
    if (flags_u == 0) return 0;
    return (uint64_t)-EINVAL;
}

static uint64_t do_sys_capget(uint64_t hdrp_u, uint64_t datap_u) {
    struct edge_linux_cap_user_header hdr;
    struct edge_linux_cap_user_data data[2];
    int ndata = 0;
    if (!hdrp_u) return (uint64_t)-EFAULT;
    if (copy_from_user(&hdr, hdrp_u, sizeof(hdr)) < 0) return (uint64_t)-EFAULT;
    if (hdr.pid != 0 && hdr.pid != process_getpid()) return (uint64_t)-ESRCH;
    if (hdr.version == LINUX_CAP_VERSION_1) ndata = 1;
    else if (hdr.version == LINUX_CAP_VERSION_2 || hdr.version == LINUX_CAP_VERSION_3) ndata = 2;
    else {
        hdr.version = LINUX_CAP_VERSION_3;
        (void)copy_to_user(hdrp_u, &hdr, sizeof(hdr));
        return (uint64_t)-EINVAL;
    }
    if (datap_u) {
        memset(data, 0, sizeof(data));
        if (copy_to_user(datap_u, data, (uint64_t)ndata * sizeof(data[0])) < 0) return (uint64_t)-EFAULT;
    }
    return 0;
}

static uint64_t do_sys_capset(uint64_t hdrp_u, uint64_t datap_u) {
    struct edge_linux_cap_user_header hdr;
    struct edge_linux_cap_user_data data[2];
    int ndata = 0;
    task_t *t = process_current_task();
    if (!hdrp_u || !datap_u) return (uint64_t)-EFAULT;
    if (copy_from_user(&hdr, hdrp_u, sizeof(hdr)) < 0) return (uint64_t)-EFAULT;
    if (!t || t->euid != 0) return (uint64_t)-EPERM;
    if (hdr.pid != 0 && hdr.pid != process_getpid()) return (uint64_t)-EPERM;
    if (hdr.version == LINUX_CAP_VERSION_1) ndata = 1;
    else if (hdr.version == LINUX_CAP_VERSION_2 || hdr.version == LINUX_CAP_VERSION_3) ndata = 2;
    else return (uint64_t)-EINVAL;
    if (copy_from_user(data, datap_u, (uint64_t)ndata * sizeof(data[0])) < 0) return (uint64_t)-EFAULT;
    /* No capability model yet: accept and ignore for root callers. */
    return 0;
}

static uint64_t do_sys_sync(void) { return 0; }
static uint64_t do_sys_syncfs(uint64_t fd_u) { (void)fd_u; return 0; }

static uint64_t do_sys_getentropy(uint64_t buf_u, uint64_t len_u) {
    if (len_u > 256) return (uint64_t)-EIO;
    return do_sys_getrandom(buf_u, len_u, 0);
}

static uint64_t do_sys_getpriority(uint64_t which_u, uint64_t who_u) {
    (void)who_u;
    if ((int)which_u != LINUX_PRIO_PROCESS) return (uint64_t)-EINVAL;
    return 0;
}

static uint64_t do_sys_setpriority(uint64_t which_u, uint64_t who_u, uint64_t prio_u) {
    (void)who_u;
    (void)prio_u;
    if ((int)which_u != LINUX_PRIO_PROCESS) return (uint64_t)-EINVAL;
    return 0;
}

static uint64_t do_sys_times(uint64_t tms_u) {
    struct edge_linux_tms t;
    uint64_t ticks = boottime_monotonic_us() / 10000ull; /* 100Hz */
    memset(&t, 0, sizeof(t));
    if (tms_u && copy_to_user(tms_u, &t, sizeof(t)) < 0) return (uint64_t)-EFAULT;
    return ticks;
}

static uint64_t do_sys_getitimer(uint64_t which_u, uint64_t old_u) {
    task_t *t = process_current_task();
    struct edge_itimerval oldv;
    uint64_t now;
    if (!t || !old_u) return (uint64_t)-EINVAL;
    if (which_u != 0) return (uint64_t)-EINVAL;
    now = boottime_monotonic_us();
    memset(&oldv, 0, sizeof(oldv));
    if (t->itimer_real_active && t->itimer_real_next_us > now) us_to_timeval(t->itimer_real_next_us - now, &oldv.it_value);
    us_to_timeval(t->itimer_real_interval_us, &oldv.it_interval);
    if (copy_to_user(old_u, &oldv, sizeof(oldv)) < 0) return (uint64_t)-EFAULT;
    return 0;
}

static uint64_t do_sys_faccessat2(uint64_t dirfd_u, uint64_t path_u, uint64_t mode_u, uint64_t flags_u) {
    int dirfd = (int)dirfd_u;
    int flags = (int)flags_u;
    char path_in[256], path[256];
    vfs_inode_t ino;
    if ((flags & ~(LINUX_AT_EACCESS | LINUX_AT_SYMLINK_NOFOLLOW)) != 0) return (uint64_t)-EINVAL;
    if (!path_u) return (uint64_t)-EINVAL;
    if (copy_user_cstr(path_in, sizeof(path_in), path_u) < 0) return (uint64_t)-EFAULT;
    if (build_at_path(dirfd, path_in, path, (int)sizeof(path)) < 0) return (uint64_t)-EINVAL;
    if (vfs_resolve(path, &ino, 0, 0, 0) != 0) return (uint64_t)-ENOENT;
    if (mode_u == 0) return 0;
    return vfs_permission_check(&ino, (int)(mode_u & 7u), process_current_task()) == 0 ? 0 : (uint64_t)-EACCES;
}

static uint64_t do_sys_openat2(uint64_t dirfd_u, uint64_t path_u, uint64_t how_u, uint64_t size_u) {
    struct edge_linux_open_how how;
    if (!how_u) return (uint64_t)-EINVAL;
    memset(&how, 0, sizeof(how));
    if (size_u == 0) return (uint64_t)-EINVAL;
    if (size_u > sizeof(how)) size_u = sizeof(how);
    if (copy_from_user(&how, how_u, size_u) < 0) return (uint64_t)-EFAULT;
    if (how.resolve != 0) return (uint64_t)-EOPNOTSUPP;
    return do_sys_openat(dirfd_u, path_u, how.flags, how.mode);
}

static uint64_t do_sys_statx(uint64_t dirfd_u, uint64_t path_u, uint64_t flags_u, uint64_t mask_u, uint64_t statx_u) {
    int dirfd = (int)dirfd_u;
    char path_in[256], path[256];
    vfs_inode_t ino;
    struct edge_linux_statx stx;
    (void)mask_u;
    if (!path_u || !statx_u) return (uint64_t)-EINVAL;
    if (copy_user_cstr(path_in, sizeof(path_in), path_u) < 0) return (uint64_t)-EFAULT;
    if ((flags_u & ~(LINUX_AT_SYMLINK_NOFOLLOW | LINUX_AT_EMPTY_PATH)) != 0) return (uint64_t)-EINVAL;
    if ((flags_u & LINUX_AT_EMPTY_PATH) && path_in[0] == 0) return (uint64_t)-ENOSYS;
    if (build_at_path(dirfd, path_in, path, (int)sizeof(path)) < 0) return (uint64_t)-EINVAL;
    if (vfs_resolve(path, &ino, 0, 0, 0) < 0) return (uint64_t)-ENOENT;
    memset(&stx, 0, sizeof(stx));
    stx.stx_mask = 0x000017ffu;
    stx.stx_blksize = 4096;
    stx.stx_nlink = 1;
    stx.stx_uid = ino.uid;
    stx.stx_gid = ino.gid;
    stx.stx_mode = ino.mode;
    stx.stx_ino = ino.ino;
    stx.stx_size = ino.size;
    stx.stx_blocks = (ino.size + 511u) / 512u;
    if (copy_to_user(statx_u, &stx, sizeof(stx)) < 0) return (uint64_t)-EFAULT;
    return 0;
}

static uint64_t do_sys_sendfile(uint64_t out_fd_u, uint64_t in_fd_u, uint64_t off_u, uint64_t count_u) {
    uint8_t buf[8192];
    uint64_t done = 0;
    edge_fd_proc_t *p = fd_proc_with_stdio();
    edge_fd_t *in_e = fd_get(p, (int)in_fd_u);
    uint64_t saved_pos = 0;
    int use_off = 0;
    if (count_u == 0) return 0;
    if (!in_e) return (uint64_t)-EBADF;
    if (off_u) {
        if (copy_from_user(&saved_pos, off_u, sizeof(saved_pos)) < 0) return (uint64_t)-EFAULT;
        use_off = 1;
        {
            uint64_t cur = do_sys_lseek(in_fd_u, 0, LINUX_SEEK_CUR);
            if ((int64_t)cur >= 0) {
                uint64_t want = saved_pos;
                if ((int64_t)do_sys_lseek(in_fd_u, want, LINUX_SEEK_SET) < 0) use_off = 0;
                else saved_pos = cur;
            } else {
                use_off = 0;
            }
        }
    }
    while (done < count_u) {
        uint64_t n = count_u - done;
        uint64_t r, w;
        if (n > sizeof(buf)) n = sizeof(buf);
        r = do_sys_fd_read(in_fd_u, (uint64_t)(uintptr_t)buf, n);
        if ((int64_t)r < 0) {
            if (done == 0) done = r;
            break;
        }
        if (r == 0) break;
        w = do_sys_fd_write(out_fd_u, (uint64_t)(uintptr_t)buf, r);
        if ((int64_t)w < 0) {
            if (done == 0) done = w;
            break;
        }
        done += w;
        if (w < r) break;
    }
    if (use_off) {
        uint64_t end = do_sys_lseek(in_fd_u, 0, LINUX_SEEK_CUR);
        uint64_t new_off = ((int64_t)end >= 0) ? end : 0;
        (void)copy_to_user(off_u, &new_off, sizeof(new_off));
        (void)do_sys_lseek(in_fd_u, saved_pos, LINUX_SEEK_SET);
    }
    return done;
}

static uint64_t do_sys_copy_file_range(uint64_t fd_in_u, uint64_t off_in_u, uint64_t fd_out_u, uint64_t off_out_u,
                                       uint64_t len_u, uint64_t flags_u) {
    uint8_t buf[8192];
    uint64_t done = 0;
    uint64_t in_saved = 0, out_saved = 0;
    int restore_in = 0, restore_out = 0;
    if (flags_u != 0) return (uint64_t)-EINVAL;
    if (len_u == 0) return 0;
    if (off_in_u) {
        uint64_t off;
        uint64_t cur = do_sys_lseek(fd_in_u, 0, LINUX_SEEK_CUR);
        if ((int64_t)cur < 0) return (uint64_t)-EINVAL;
        if (copy_from_user(&off, off_in_u, sizeof(off)) < 0) return (uint64_t)-EFAULT;
        if ((int64_t)do_sys_lseek(fd_in_u, off, LINUX_SEEK_SET) < 0) return (uint64_t)-EINVAL;
        in_saved = cur;
        restore_in = 1;
    }
    if (off_out_u) {
        uint64_t off;
        uint64_t cur = do_sys_lseek(fd_out_u, 0, LINUX_SEEK_CUR);
        if ((int64_t)cur < 0) return (uint64_t)-EINVAL;
        if (copy_from_user(&off, off_out_u, sizeof(off)) < 0) return (uint64_t)-EFAULT;
        if ((int64_t)do_sys_lseek(fd_out_u, off, LINUX_SEEK_SET) < 0) return (uint64_t)-EINVAL;
        out_saved = cur;
        restore_out = 1;
    }
    while (done < len_u) {
        uint64_t n = len_u - done;
        uint64_t r, w;
        if (n > sizeof(buf)) n = sizeof(buf);
        r = do_sys_fd_read(fd_in_u, (uint64_t)(uintptr_t)buf, n);
        if ((int64_t)r < 0) {
            if (done == 0) done = r;
            break;
        }
        if (r == 0) break;
        w = do_sys_fd_write(fd_out_u, (uint64_t)(uintptr_t)buf, r);
        if ((int64_t)w < 0) {
            if (done == 0) done = w;
            break;
        }
        done += w;
        if (w < r) break;
    }
    if (off_in_u) {
        uint64_t pos = do_sys_lseek(fd_in_u, 0, LINUX_SEEK_CUR);
        (void)copy_to_user(off_in_u, &pos, sizeof(pos));
    }
    if (off_out_u) {
        uint64_t pos = do_sys_lseek(fd_out_u, 0, LINUX_SEEK_CUR);
        (void)copy_to_user(off_out_u, &pos, sizeof(pos));
    }
    if (restore_in) (void)do_sys_lseek(fd_in_u, in_saved, LINUX_SEEK_SET);
    if (restore_out) (void)do_sys_lseek(fd_out_u, out_saved, LINUX_SEEK_SET);
    return done;
}

static uint64_t do_sys_fallocate(uint64_t fd_u, uint64_t mode_u, uint64_t off_u, uint64_t len_u) {
    edge_fd_proc_t *p = fd_proc_with_stdio();
    edge_fd_t *e = fd_get(p, (int)fd_u);
    uint64_t end;
    (void)mode_u;
    if (!e) return (uint64_t)-EBADF;
    if (e->kind != FD_VFS) return (uint64_t)-EINVAL;
    if ((int64_t)off_u < 0 || (int64_t)len_u < 0) return (uint64_t)-EINVAL;
    end = off_u + len_u;
    if (end < off_u) return (uint64_t)-EOVERFLOW;
    if (end > e->inode.size) return do_sys_ftruncate(fd_u, end);
    return 0;
}

static uint64_t do_sys_splice(uint64_t fd_in_u, uint64_t off_in_u, uint64_t fd_out_u, uint64_t off_out_u,
                              uint64_t len_u, uint64_t flags_u) {
    (void)flags_u;
    /* Minimal compatibility: emulate via copy path for common file/socket/pipe cases. */
    return do_sys_copy_file_range(fd_in_u, off_in_u, fd_out_u, off_out_u, len_u, 0);
}

static uint64_t do_sys_tee(uint64_t fd_in_u, uint64_t fd_out_u, uint64_t len_u, uint64_t flags_u) {
    (void)fd_in_u; (void)fd_out_u; (void)len_u; (void)flags_u;
    return (uint64_t)-EOPNOTSUPP;
}

static uint64_t do_sys_vmsplice(uint64_t fd_u, uint64_t iov_u, uint64_t nr_segs_u, uint64_t flags_u) {
    uint64_t done = 0;
    uint64_t iovcnt = nr_segs_u;
    struct edge_linux_iovec iov[16];
    (void)flags_u;
    if (!iov_u || iovcnt == 0) return (uint64_t)-EINVAL;
    if (iovcnt > 16) iovcnt = 16;
    if (copy_from_user(iov, iov_u, iovcnt * sizeof(iov[0])) < 0) return (uint64_t)-EFAULT;
    for (uint64_t i = 0; i < iovcnt; ++i) {
        uint64_t w = do_sys_fd_write(fd_u, iov[i].iov_base, iov[i].iov_len);
        if ((int64_t)w < 0) return done ? done : w;
        done += w;
        if (w < iov[i].iov_len) break;
    }
    return done;
}

static int alloc_special_fd(edge_fd_kind_t kind, int obj_id, int flags) {
    edge_fd_proc_t *p = fd_proc_with_stdio();
    int fd;
    if (!p) return -ENOMEM;
    fd = fd_alloc(p, 0);
    if (fd < 0) return -ENOMEM;
    p->fds[fd].kind = kind;
    p->fds[fd].file_ref = file_ref_alloc();
    if (!p->fds[fd].file_ref) {
        memset(&p->fds[fd], 0, sizeof(p->fds[fd]));
        return -ENOMEM;
    }
    p->fds[fd].pipe_id = obj_id;
    p->fds[fd].flags = flags & (LINUX_O_NONBLOCK | LINUX_O_RDWR | LINUX_O_WRONLY);
    p->fds[fd].fd_flags = (flags & LINUX_O_CLOEXEC) ? LINUX_FD_CLOEXEC : 0;
    return fd;
}

static uint64_t do_sys_eventfd2(uint64_t initval_u, uint64_t flags_u) {
    int flags = (int)flags_u;
    int id, fd;
    if (flags & ~(LINUX_EFD_SEMAPHORE | LINUX_EFD_NONBLOCK | LINUX_EFD_CLOEXEC)) return (uint64_t)-EINVAL;
    id = eventfd_alloc_obj((uint32_t)initval_u, (flags & LINUX_EFD_SEMAPHORE) != 0);
    if (id < 0) return (uint64_t)-ENOMEM;
    fd = alloc_special_fd(FD_EVENTFD, id, (flags & (LINUX_O_NONBLOCK | LINUX_O_CLOEXEC)));
    if (fd < 0) {
        eventfd_drop_ref(id);
        return (uint64_t)(int64_t)fd;
    }
    return (uint64_t)fd;
}

static uint64_t do_sys_eventfd(uint64_t initval_u) { return do_sys_eventfd2(initval_u, 0); }

static uint64_t do_sys_timerfd_create(uint64_t clockid_u, uint64_t flags_u) {
    int clockid = (int)clockid_u;
    int flags = (int)flags_u;
    int id, fd;
    if (!(clockid == CLOCK_REALTIME || clockid == CLOCK_MONOTONIC)) return (uint64_t)-EINVAL;
    if (flags & ~(LINUX_TFD_NONBLOCK | LINUX_TFD_CLOEXEC)) return (uint64_t)-EINVAL;
    id = timerfd_alloc_obj(clockid);
    if (id < 0) return (uint64_t)-ENOMEM;
    fd = alloc_special_fd(FD_TIMERFD, id, (flags & (LINUX_O_NONBLOCK | LINUX_O_CLOEXEC)));
    if (fd < 0) {
        timerfd_drop_ref(id);
        return (uint64_t)(int64_t)fd;
    }
    return (uint64_t)fd;
}

static uint64_t do_sys_timerfd_settime(uint64_t fd_u, uint64_t flags_u, uint64_t new_u, uint64_t old_u) {
    edge_fd_proc_t *p = fd_proc_with_stdio();
    edge_fd_t *e = fd_get(p, (int)fd_u);
    edge_timerfd_t *tf;
    struct edge_itimerspec newv, oldv;
    uint64_t now;
    int ok;
    uint64_t val_us, int_us;
    if (!e || e->kind != FD_TIMERFD) return (uint64_t)-EBADF;
    if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_TIMERFDS) return (uint64_t)-EBADF;
    tf = &g_timerfds[e->pipe_id];
    if (!tf->used || !new_u) return (uint64_t)-EINVAL;
    memset(&oldv, 0, sizeof(oldv));
    now = timerfd_now_us(tf->clockid);
    if (tf->active && tf->next_us > now) {
        uint64_t rem = tf->next_us - now;
        oldv.it_value.tv_sec = (int64_t)(rem / 1000000ull);
        oldv.it_value.tv_nsec = (int64_t)((rem % 1000000ull) * 1000ull);
    }
    oldv.it_interval.tv_sec = (int64_t)(tf->interval_us / 1000000ull);
    oldv.it_interval.tv_nsec = (int64_t)((tf->interval_us % 1000000ull) * 1000ull);
    if (old_u && copy_to_user(old_u, &oldv, sizeof(oldv)) < 0) return (uint64_t)-EFAULT;
    if (copy_from_user(&newv, new_u, sizeof(newv)) < 0) return (uint64_t)-EFAULT;
    val_us = timespec_to_us_checked(&newv.it_value, &ok);
    if (!ok) return (uint64_t)-EINVAL;
    int_us = timespec_to_us_checked(&newv.it_interval, &ok);
    if (!ok) return (uint64_t)-EINVAL;
    tf->interval_us = int_us;
    if (val_us == 0) {
        tf->active = 0;
        tf->next_us = 0;
    } else {
        tf->active = 1;
        tf->next_us = ((flags_u & LINUX_TFD_TIMER_ABSTIME) ? val_us : (now + val_us));
    }
    return 0;
}

static uint64_t do_sys_timerfd_gettime(uint64_t fd_u, uint64_t cur_u) {
    edge_fd_proc_t *p = fd_proc_with_stdio();
    edge_fd_t *e = fd_get(p, (int)fd_u);
    edge_timerfd_t *tf;
    struct edge_itimerspec curv;
    uint64_t now;
    if (!e || e->kind != FD_TIMERFD || !cur_u) return (uint64_t)-EINVAL;
    if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_TIMERFDS) return (uint64_t)-EBADF;
    tf = &g_timerfds[e->pipe_id];
    if (!tf->used) return (uint64_t)-EBADF;
    memset(&curv, 0, sizeof(curv));
    now = timerfd_now_us(tf->clockid);
    if (tf->active && tf->next_us > now) {
        uint64_t rem = tf->next_us - now;
        curv.it_value.tv_sec = (int64_t)(rem / 1000000ull);
        curv.it_value.tv_nsec = (int64_t)((rem % 1000000ull) * 1000ull);
    } else if (tf->active && tf->next_us <= now) {
        curv.it_value.tv_nsec = 1; /* readable now */
    }
    curv.it_interval.tv_sec = (int64_t)(tf->interval_us / 1000000ull);
    curv.it_interval.tv_nsec = (int64_t)((tf->interval_us % 1000000ull) * 1000ull);
    if (copy_to_user(cur_u, &curv, sizeof(curv)) < 0) return (uint64_t)-EFAULT;
    return 0;
}

static uint64_t do_sys_signalfd4(uint64_t fd_u, uint64_t mask_u, uint64_t siz_u, uint64_t flags_u) {
    edge_fd_proc_t *p = fd_proc_with_stdio();
    uint64_t mask = 0;
    int flags = (int)flags_u;
    if (!mask_u || siz_u < 8) return (uint64_t)-EINVAL;
    if (copy_from_user(&mask, mask_u, 8) < 0) return (uint64_t)-EFAULT;
    if (flags & ~(LINUX_SFD_NONBLOCK | LINUX_SFD_CLOEXEC)) return (uint64_t)-EINVAL;
    if ((int)fd_u == -1) {
        int id = signalfd_alloc_obj(mask);
        int fd;
        if (id < 0) return (uint64_t)-ENOMEM;
        fd = alloc_special_fd(FD_SIGNALFD, id, (flags & (LINUX_O_NONBLOCK | LINUX_O_CLOEXEC)));
        if (fd < 0) {
            signalfd_drop_ref(id);
            return (uint64_t)(int64_t)fd;
        }
        return (uint64_t)fd;
    } else {
        edge_fd_t *e = fd_get(p, (int)fd_u);
        if (!e || e->kind != FD_SIGNALFD) return (uint64_t)-EBADF;
        if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_SIGNALFDS || !g_signalfds[e->pipe_id].used) return (uint64_t)-EBADF;
        g_signalfds[e->pipe_id].mask = mask;
        e->flags = (e->flags & ~LINUX_O_NONBLOCK) | (flags & LINUX_O_NONBLOCK);
        if (flags & LINUX_O_CLOEXEC) e->fd_flags |= LINUX_FD_CLOEXEC;
        return fd_u;
    }
}

static uint64_t do_sys_signalfd(uint64_t fd_u, uint64_t mask_u, uint64_t siz_u) {
    return do_sys_signalfd4(fd_u, mask_u, siz_u, 0);
}

static uint64_t do_sys_epoll_create1(uint64_t flags_u) {
    int flags = (int)flags_u;
    int id, fd;
    if (flags & ~LINUX_EPOLL_CLOEXEC) return (uint64_t)-EINVAL;
    id = epoll_alloc_obj();
    if (id < 0) return (uint64_t)-ENOMEM;
    fd = alloc_special_fd(FD_EPOLL, id, (flags & LINUX_O_CLOEXEC));
    if (fd < 0) {
        epoll_drop_ref(id);
        return (uint64_t)(int64_t)fd;
    }
    return (uint64_t)fd;
}

static uint64_t do_sys_epoll_create(uint64_t size_u) {
    if ((int)size_u <= 0) return (uint64_t)-EINVAL;
    return do_sys_epoll_create1(0);
}

static uint64_t do_sys_epoll_ctl(uint64_t epfd_u, uint64_t op_u, uint64_t fd_u, uint64_t ev_u) {
    edge_fd_proc_t *p = fd_proc_with_stdio();
    edge_fd_t *e = fd_get(p, (int)epfd_u);
    edge_epoll_t *ep;
    int op = (int)op_u;
    int fd = (int)fd_u;
    int idx = -1;
    struct edge_linux_epoll_event ev;
    if (!e || e->kind != FD_EPOLL) return (uint64_t)-EBADF;
    if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_EPOLLS) return (uint64_t)-EBADF;
    ep = &g_epolls[e->pipe_id];
    if (!ep->used) return (uint64_t)-EBADF;
    if (fd < 0 || fd == (int)epfd_u) return (uint64_t)-EINVAL;
    for (int i = 0; i < ep->nwatch; ++i) if (ep->watch[i].fd == fd) { idx = i; break; }
    if (op != LINUX_EPOLL_CTL_DEL) {
        if (!ev_u) return (uint64_t)-EINVAL;
        if (copy_from_user(&ev, ev_u, sizeof(ev)) < 0) return (uint64_t)-EFAULT;
    } else memset(&ev, 0, sizeof(ev));
    if (op == LINUX_EPOLL_CTL_ADD) {
        if (idx >= 0) return (uint64_t)-EEXIST;
        if (ep->nwatch >= EDGE_EPOLL_MAX_WATCH) return (uint64_t)-ENOMEM;
        ep->watch[ep->nwatch].fd = fd;
        ep->watch[ep->nwatch].events = ev.events;
        ep->watch[ep->nwatch].data = ev.data;
        ep->nwatch++;
        return 0;
    }
    if (op == LINUX_EPOLL_CTL_MOD) {
        if (idx < 0) return (uint64_t)-ENOENT;
        ep->watch[idx].events = ev.events;
        ep->watch[idx].data = ev.data;
        return 0;
    }
    if (op == LINUX_EPOLL_CTL_DEL) {
        if (idx < 0) return (uint64_t)-ENOENT;
        ep->watch[idx] = ep->watch[ep->nwatch - 1];
        ep->nwatch--;
        return 0;
    }
    return (uint64_t)-EINVAL;
}

static uint64_t do_sys_epoll_wait(uint64_t epfd_u, uint64_t events_u, uint64_t maxevents_u, uint64_t timeout_u) {
    edge_fd_proc_t *p = fd_proc_with_stdio();
    edge_fd_t *e = fd_get(p, (int)epfd_u);
    edge_epoll_t *ep;
    struct edge_linux_epoll_event out[64];
    int maxevents = (int)maxevents_u;
    int timeout = (int)timeout_u;
    uint64_t start_us;
    if (!e || e->kind != FD_EPOLL) return (uint64_t)-EBADF;
    if (!events_u || maxevents <= 0) return (uint64_t)-EINVAL;
    if (maxevents > 64) maxevents = 64;
    if (e->pipe_id < 0 || e->pipe_id >= EDGE_MAX_EPOLLS) return (uint64_t)-EBADF;
    ep = &g_epolls[e->pipe_id];
    if (!ep->used) return (uint64_t)-EBADF;
    start_us = boottime_monotonic_us();
    for (;;) {
        int nout = 0;
        for (int i = 0; i < ep->nwatch && nout < maxevents; ++i) {
            edge_fd_t *we = fd_get(p, ep->watch[i].fd);
            int16_t req = 0, rev;
            uint32_t eev = 0;
            if (!we) continue;
            if (ep->watch[i].events & LINUX_EPOLLIN) req |= (LINUX_POLLIN | LINUX_POLLPRI);
            if (ep->watch[i].events & LINUX_EPOLLOUT) req |= LINUX_POLLOUT;
            if (req == 0) req = LINUX_POLLIN | LINUX_POLLOUT;
            rev = poll_fd_revents(we, req);
            if (rev & (LINUX_POLLIN | LINUX_POLLPRI)) eev |= LINUX_EPOLLIN;
            if (rev & LINUX_POLLOUT) eev |= LINUX_EPOLLOUT;
            if (rev & LINUX_POLLERR) eev |= LINUX_EPOLLERR;
            if (rev & LINUX_POLLHUP) eev |= LINUX_EPOLLHUP;
            if ((eev & (ep->watch[i].events | LINUX_EPOLLERR | LINUX_EPOLLHUP)) == 0) continue;
            out[nout].events = eev;
            out[nout].data = ep->watch[i].data;
            nout++;
        }
        if (nout > 0) {
            if (copy_to_user(events_u, out, (uint64_t)nout * sizeof(out[0])) < 0) return (uint64_t)-EFAULT;
            return (uint64_t)nout;
        }
        if (timeout == 0) return 0;
        if (timeout > 0) {
            uint64_t now = boottime_monotonic_us();
            if (now - start_us >= (uint64_t)timeout * 1000ull) return 0;
        }
        if (signal_pending_interrupt()) return tty_interrupt_current_ret();
        lwip_stack_poll();
        wait_blocking_step();
    }
}

static void syscall_handler(REGISTERS *r) {
    const uint64_t nr = r->rax;
    /* Some entry paths arrive with IF cleared (e.g. #UD-based syscall emulation).
     * Re-enable IRQs for long syscalls so timer/IO keep progressing. */
    __asm__ __volatile__("sti");

    const uint64_t a1 = r->rdi;
    const uint64_t a2 = r->rsi;
    const uint64_t a3 = r->rdx;
    const uint64_t a4 = r->r10;
    const uint64_t a5 = r->r8;
    const uint64_t a6 = r->r9;
    g_syscall_debug_nr = nr;
    g_syscall_debug_rip = r->rip;
    g_syscall_debug_pid = process_getpid();

#define SCRUB_REGS_AFTER_EXEC() do { \
    r->rbx = 0; r->rcx = 0; r->rdx = 0; r->rsi = 0; r->rdi = 0; r->rbp = 0; \
    r->r8 = 0; r->r9 = 0; r->r10 = 0; r->r11 = 0; r->r12 = 0; r->r13 = 0; r->r14 = 0; r->r15 = 0; \
} while (0)

    switch (nr) {
        case SYS_stat:
            r->rax = do_sys_stat_path(a1, a2);
            break;
        case SYS_fstat:
            r->rax = do_sys_fstat(a1, a2);
            break;
        case SYS_lstat:
            r->rax = do_sys_lstat_path(a1, a2);
            break;
        case EDGE_SYS_write:
            r->rax = do_sys_fd_write(a1, a2, a3);
            break;
        case EDGE_SYS_read:
            r->rax = do_sys_fd_read(a1, a2, a3);
            break;
        case EDGE_SYS_open:
            r->rax = do_sys_openat((uint64_t)LINUX_AT_FDCWD, a1, a2, a3);
            break;
        case EDGE_SYS_close:
            r->rax = do_sys_close(a1);
            break;
        case EDGE_SYS_fork:
            r->rax = do_sys_fork_compat(r);
            break;
        case EDGE_SYS_wait:
            if (user_range_ok(a1, sizeof(int))) {
                r->rax = do_sys_wait4((uint64_t)-1, a1, 0, 0);
            } else {
                r->rax = do_sys_wait4(a1, a2, a3, a4);
            }
            break;
        case EDGE_SYS_exit:
            fd_proc_release(process_getpid());
            scheduler_kill_current_and_yield((int)a1);
            break;
        case EDGE_SYS_execve:
                r->rax = do_sys_execve(a1, a2, a3);
                /* * CRITICAL FIX: Clean registers on successful execve.
                * Musl libc checks RDX at startup; if non-zero, it executes it as a finalizer
                * on exit, causing GP Faults. We must zero GPRs to ensure a clean state.
                */
                if (r->rax == 0) {
                    SCRUB_REGS_AFTER_EXEC();
                }
            break;
        case EDGE_SYS_spawn:
            r->rax = do_sys_spawn(a1, a2, a3);
            break;
        case EDGE_SYS_getpid:
            r->rax = (uint64_t)process_getpid();
            break;
        case EDGE_SYS_brk:
            r->rax = do_sys_brk(a1);
            break;
        case EDGE_SYS_getcwd:
            r->rax = do_sys_getcwd(a1, a2);
            break;
        case EDGE_SYS_chdir:
            r->rax = do_sys_chdir(a1);
            break;
        case EDGE_SYS_ls:
            r->rax = do_sys_ls(a1, a2);
            break;
        case EDGE_SYS_mkdir:
            r->rax = do_sys_mkdir(a1);
            break;
        case EDGE_SYS_touch:
            r->rax = do_sys_touch(a1);
            break;
        case EDGE_SYS_unlink:
            r->rax = do_sys_unlink(a1);
            break;
        case EDGE_SYS_cat:
            r->rax = do_sys_cat(a1);
            break;
        case EDGE_SYS_statfs:
            r->rax = do_sys_statfs(a1, a2, a3);
            break;
        case EDGE_SYS_meminfo:
            r->rax = do_sys_meminfo(a1, a2, a3);
            break;
        case EDGE_SYS_mounts:
            r->rax = do_sys_mounts();
            break;
        case EDGE_SYS_mount:
            r->rax = do_sys_mount(a1, a2, a3);
            break;
        case EDGE_SYS_shutdown:
            r->rax = do_sys_shutdown();
            break;
        case EDGE_SYS_ps:
            r->rax = do_sys_ps();
            break;
        case EDGE_SYS_kill:
            r->rax = do_sys_kill(a1, a2);
            break;
        case EDGE_SYS_sleep:
            r->rax = do_sys_sleep(a1);
            break;
        case EDGE_SYS_dmesg:
            r->rax = do_sys_dmesg();
            break;
        case EDGE_SYS_stat:
            r->rax = do_sys_stat(a1);
            break;
        case EDGE_SYS_mv:
            r->rax = do_sys_mv(a1, a2);
            break;
        case EDGE_SYS_writefile:
            r->rax = do_sys_writefile(a1, a2, a3);
            break;
        case EDGE_SYS_readfile:
            r->rax = do_sys_readfile(a1, a2, a3);
            break;

        case SYS_lseek:
            r->rax = do_sys_lseek(a1, a2, a3);
            break;
        case SYS_poll:
            r->rax = do_sys_poll(a1, a2, a3);
            break;
        case SYS_sched_yield:
            r->rax = do_sys_sched_yield();
            break;
        case SYS_select:
            r->rax = do_sys_select(a1, a2, a3, a4, a5);
            break;
        case SYS_pselect6:
            r->rax = do_sys_pselect6(a1, a2, a3, a4, a5, a6);
            break;
        case SYS_ppoll:
            r->rax = do_sys_ppoll(a1, a2, a3, a4, a5);
            break;
        case SYS_splice:
            r->rax = do_sys_splice(a1, a2, a3, a4, a5, a6);
            break;
        case SYS_tee:
            r->rax = do_sys_tee(a1, a2, a3, a4);
            break;
        case SYS_vmsplice:
            r->rax = do_sys_vmsplice(a1, a2, a3, a4);
            break;
        case SYS_epoll_create:
            r->rax = do_sys_epoll_create(a1);
            break;
        case SYS_epoll_create1:
            r->rax = do_sys_epoll_create1(a1);
            break;
        case SYS_epoll_ctl:
            r->rax = do_sys_epoll_ctl(a1, a2, a3, a4);
            break;
        case SYS_epoll_wait:
            r->rax = do_sys_epoll_wait(a1, a2, a3, a4);
            break;
        case SYS_eventfd:
            r->rax = do_sys_eventfd(a1);
            break;
        case SYS_eventfd2:
            r->rax = do_sys_eventfd2(a1, a2);
            break;
        case SYS_timerfd_create:
            r->rax = do_sys_timerfd_create(a1, a2);
            break;
        case SYS_timerfd_settime:
            r->rax = do_sys_timerfd_settime(a1, a2, a3, a4);
            break;
        case SYS_timerfd_gettime:
            r->rax = do_sys_timerfd_gettime(a1, a2);
            break;
        case SYS_signalfd:
            r->rax = do_sys_signalfd(a1, a2, a3);
            break;
        case SYS_signalfd4:
            r->rax = do_sys_signalfd4(a1, a2, a3, a4);
            break;
        case SYS_mmap:
            r->rax = do_sys_mmap(a1, a2, a3, a4, a5, a6);
            break;
        case SYS_mremap:
            r->rax = do_sys_mremap(a1, a2, a3, a4, a5);
            break;
        case SYS_madvise:
            r->rax = do_sys_madvise(a1, a2, a3);
            break;
        case SYS_mprotect:
            r->rax = do_sys_mprotect(a1, a2, a3);
            break;
        case SYS_munmap:
            r->rax = do_sys_munmap(a1, a2);
            break;
        case SYS_getcwd:
            r->rax = do_sys_getcwd(a1, a2);
            break;
        case SYS_chdir:
            r->rax = do_sys_chdir(a1);
            break;
        case SYS_chroot:
            r->rax = do_sys_chroot(a1);
            break;
        case SYS_fchdir:
            r->rax = do_sys_fchdir(a1);
            break;
        case SYS_rename:
            r->rax = do_sys_rename(a1, a2);
            break;
        case SYS_mkdir:
            r->rax = do_sys_mkdir(a1);
            break;
        case SYS_rmdir:
            r->rax = do_sys_rmdir(a1);
            break;
        case SYS_link:
            r->rax = do_sys_link(a1, a2);
            break;
        case SYS_unlink:
            r->rax = do_sys_unlink(a1);
            break;
        case SYS_gettimeofday:
            r->rax = do_sys_gettimeofday(a1, a2);
            break;
        case SYS_getrlimit:
            r->rax = do_sys_getrlimit(a1, a2);
            break;
        case SYS_getrusage:
            r->rax = do_sys_getrusage(a1, a2);
            break;
        case SYS_sysinfo:
            r->rax = do_sys_sysinfo(a1);
            break;
        case SYS_chmod:
            r->rax = do_sys_chmod(a1, a2);
            break;
        case SYS_readlink:
            r->rax = do_sys_readlink(a1, a2, a3);
            break;
        case SYS_chown:
            r->rax = do_sys_chown(a1, a2, a3);
            break;
        case SYS_fchmod:
            r->rax = do_sys_fchmod(a1, a2);
            break;
        case SYS_fchown:
            r->rax = do_sys_fchown(a1, a2, a3);
            break;
        case SYS_readv:
            r->rax = do_sys_readv(a1, a2, a3);
            break;
        case SYS_writev:
            r->rax = do_sys_writev(a1, a2, a3);
            break;
        case SYS_rt_sigaction:
            r->rax = do_sys_rt_sigaction(a1, a2, a3, a4);
            break;
        case SYS_rt_sigreturn:
            r->rax = do_sys_rt_sigreturn();
            break;
        case SYS_rt_sigprocmask:
            r->rax = do_sys_rt_sigprocmask(a1, a2, a3, a4);
            break;
        case SYS_ioctl:
            r->rax = do_sys_ioctl(a1, a2, a3);
            break;
        case SYS_socket:
            r->rax = do_sys_socket(a1, a2, a3);
            break;
        case SYS_sendfile:
            r->rax = do_sys_sendfile(a1, a2, a3, a4);
            break;
        case SYS_bind:
            r->rax = do_sys_bind(a1, a2, a3);
            break;
        case SYS_connect:
            r->rax = do_sys_connect(a1, a2, a3);
            break;
        case SYS_sendto:
            r->rax = do_sys_sendto(a1, a2, a3, a4, a5, a6);
            break;
        case SYS_recvfrom:
            r->rax = do_sys_recvfrom(a1, a2, a3, a4, a5, a6);
            break;
        case SYS_sendmsg:
            r->rax = do_sys_sendmsg(a1, a2, a3);
            break;
        case SYS_recvmsg:
            r->rax = do_sys_recvmsg(a1, a2, a3);
            break;
        case SYS_setsockopt:
            r->rax = do_sys_setsockopt(a1, a2, a3, a4, a5);
            break;
        case SYS_getsockopt:
            r->rax = do_sys_getsockopt(a1, a2, a3, a4, a5);
            break;
        case SYS_getsockname:
            r->rax = do_sys_getsockname(a1, a2, a3);
            break;
        case SYS_getpeername:
            r->rax = do_sys_getpeername(a1, a2, a3);
            break;
        case SYS_listen:
            r->rax = do_sys_listen(a1, a2);
            break;
        case SYS_accept:
            r->rax = do_sys_accept(a1, a2, a3);
            break;
        case SYS_accept4:
            r->rax = do_sys_accept4(a1, a2, a3, a4);
            break;
        case SYS_fallocate:
            r->rax = do_sys_fallocate(a1, a2, a3, a4);
            break;
        case SYS_shutdown:
            r->rax = 0;
            break;
        case SYS_socketpair:
            r->rax = do_sys_socketpair(a1, a2, a3, a4);
            break;
        case SYS_access:
            r->rax = do_sys_access(a1, a2);
            break;
        case SYS_faccessat2:
            r->rax = do_sys_faccessat2(a1, a2, a3, a4);
            break;
        case SYS_pipe:
            r->rax = do_sys_pipe(a1, 0);
            break;
        case SYS_pipe2:
            r->rax = do_sys_pipe(a1, a2);
            break;
        case SYS_dup:
            r->rax = do_sys_dup(a1);
            break;
        case SYS_dup2:
            r->rax = do_sys_dup2(a1, a2);
            break;
        case SYS_dup3:
            r->rax = do_sys_dup3(a1, a2, a3);
            break;
        case SYS_nanosleep:
            r->rax = do_sys_nanosleep(a1);
            break;
        case SYS_getitimer:
            r->rax = do_sys_getitimer(a1, a2);
            break;
        case SYS_clock_nanosleep:
            r->rax = do_sys_clock_nanosleep(a1, a2, a3, a4);
            break;
        case SYS_alarm:
            r->rax = do_sys_alarm(a1);
            break;
        case SYS_setitimer:
            r->rax = do_sys_setitimer(a1, a2, a3);
            break;
        case SYS_times:
            r->rax = do_sys_times(a1);
            break;
        case SYS_unshare:
            r->rax = do_sys_unshare(a1);
            break;
        case SYS_clone:
        case SYS_vfork:
            r->rax = do_sys_fork_compat(r);
            break;
        case SYS_kill:
            r->rax = do_sys_kill(a1, a2);
            break;
        case SYS_uname:
            r->rax = do_sys_uname(a1);
            break;
        case SYS_capget:
            r->rax = do_sys_capget(a1, a2);
            break;
        case SYS_capset:
            r->rax = do_sys_capset(a1, a2);
            break;
        case SYS_pivot_root:
            r->rax = do_sys_pivot_root(a1, a2);
            break;
        case SYS_prctl:
            r->rax = do_sys_prctl(a1, a2, a3, a4, a5);
            break;
        case SYS_sethostname:
            r->rax = do_sys_sethostname(a1, a2);
            break;
        case SYS_setrlimit:
            r->rax = do_sys_setrlimit(a1, a2);
            break;
        case SYS_truncate:
            r->rax = do_sys_truncate(a1, a2);
            break;
        case SYS_ftruncate:
            r->rax = do_sys_ftruncate(a1, a2);
            break;
        case SYS_sigaltstack:
            r->rax = do_sys_sigaltstack(a1, a2);
            break;
        case SYS_fcntl:
            r->rax = do_sys_fcntl(a1, a2, a3);
            break;
        case SYS_fsync:
        case SYS_fdatasync:
            r->rax = do_sys_fsync(a1);
            break;
        case SYS_statfs:
            r->rax = do_sys_statfs_linux(a1, a2);
            break;
        case SYS_fstatfs:
            r->rax = do_sys_fstatfs_linux(a1, a2);
            break;
        case SYS_getpriority:
            r->rax = do_sys_getpriority(a1, a2);
            break;
        case SYS_setpriority:
            r->rax = do_sys_setpriority(a1, a2, a3);
            break;
        case SYS_sched_getparam:
            r->rax = do_sys_sched_getparam(a1, a2);
            break;
        case SYS_sched_setscheduler:
            r->rax = do_sys_sched_setscheduler(a1, a2, a3);
            break;
        case SYS_getdents64:
            r->rax = do_sys_getdents64(a1, a2, a3);
            break;
        case SYS_pidfd_send_signal:
            r->rax = do_sys_pidfd_send_signal(a1, a2, a3, a4);
            break;
        case SYS_io_uring_setup:
            r->rax = do_sys_io_uring_setup(a1, a2);
            break;
        case SYS_io_uring_enter:
            r->rax = do_sys_io_uring_enter(a1, a2, a3, a4, a5, a6);
            break;
        case SYS_io_uring_register:
            r->rax = do_sys_io_uring_register(a1, a2, a3, a4);
            break;
        case SYS_set_tid_address:
            r->rax = (uint64_t)process_getpid();
            break;
        case SYS_gettid:
            r->rax = do_sys_gettid();
            break;
        case SYS_futex:
            r->rax = do_sys_futex(a1, a2, a3, a4, a5, a6);
            break;
        case SYS_sched_setaffinity:
            r->rax = do_sys_sched_setaffinity(a1, a2, a3);
            break;
        case SYS_sched_getaffinity:
            r->rax = do_sys_sched_getaffinity(a1, a2, a3);
            break;
        case SYS_clock_gettime:
            r->rax = do_sys_clock_gettime(a1, a2);
            break;
        case SYS_getrandom:
            r->rax = do_sys_getrandom(a1, a2, a3);
            break;
        case SYS_sync:
            r->rax = do_sys_sync();
            break;
        case SYS_syncfs:
            r->rax = do_sys_syncfs(a1);
            break;
        case SYS_setns:
            r->rax = do_sys_setns(a1, a2);
            break;
        case SYS_getcpu:
            r->rax = do_sys_getcpu(a1, a2, a3);
            break;
        case SYS_exit_group:
            fd_proc_release(process_getpid());
            scheduler_kill_current_and_yield((int)a1);
            break;
        case SYS_tgkill:
            r->rax = do_sys_tgkill(a1, a2, a3);
            break;
        case SYS_openat:
            r->rax = do_sys_openat(a1, a2, a3, a4);
            break;
        case SYS_openat2:
            r->rax = do_sys_openat2(a1, a2, a3, a4);
            break;
        case SYS_pidfd_open:
            r->rax = do_sys_pidfd_open(a1, a2);
            break;
        case SYS_clone3:
            r->rax = do_sys_clone3(a1, a2, r);
            break;
        case SYS_execveat:
            r->rax = do_sys_execveat(a1, a2, a3, a4, a5);
            if (r->rax == 0) SCRUB_REGS_AFTER_EXEC();
            break;
        case SYS_waitid:
            r->rax = do_sys_waitid(a1, a2, a3, a4, a5);
            break;
        case SYS_renameat:
            r->rax = do_sys_renameat(a1, a2, a3, a4);
            break;
        case SYS_renameat2:
            r->rax = do_sys_renameat2(a1, a2, a3, a4, a5);
            break;
        case SYS_linkat:
            r->rax = do_sys_linkat(a1, a2, a3, a4, a5);
            break;
        case SYS_readlinkat:
            r->rax = do_sys_readlinkat(a1, a2, a3, a4);
            break;
        case SYS_newfstatat:
            r->rax = do_sys_newfstatat(a1, a2, a3, a4);
            break;
        case SYS_statx:
            r->rax = do_sys_statx(a1, a2, a3, a4, a5);
            break;
        case SYS_copy_file_range:
            r->rax = do_sys_copy_file_range(a1, a2, a3, a4, a5, a6);
            break;
        case SYS_utimensat:
            r->rax = do_sys_utimensat(a1, a2, a3, a4);
            break;
        case SYS_set_robust_list:
            r->rax = 0;
            break;
        case SYS_prlimit64:
            r->rax = do_sys_prlimit64(a1, a2, a3, a4);
            break;
        case SYS_umask:
            r->rax = do_sys_umask(a1);
            break;
        case SYS_getuid:
            r->rax = process_getuid();
            break;
        case SYS_getgid:
            r->rax = process_getgid();
            break;
        case SYS_geteuid:
            r->rax = process_geteuid();
            break;
        case SYS_getegid:
            r->rax = process_getegid();
            break;
        case SYS_setuid:
            r->rax = do_sys_setuid(a1);
            break;
        case SYS_setgid:
            r->rax = do_sys_setgid(a1);
            break;
        case SYS_setreuid:
            r->rax = do_sys_setreuid(a1, a2);
            break;
        case SYS_setregid:
            r->rax = do_sys_setregid(a1, a2);
            break;
        case SYS_getgroups:
            r->rax = do_sys_getgroups(a1, a2);
            break;
        case SYS_setgroups:
            r->rax = do_sys_setgroups(a1, a2);
            break;
        case SYS_setresuid:
            r->rax = do_sys_setresuid(a1, a2, a3);
            break;
        case SYS_getresuid:
            r->rax = do_sys_getresuid(a1, a2, a3);
            break;
        case SYS_setresgid:
            r->rax = do_sys_setresgid(a1, a2, a3);
            break;
        case SYS_getresgid:
            r->rax = do_sys_getresgid(a1, a2, a3);
            break;
        case SYS_getppid:
            r->rax = (uint64_t)process_getppid();
            break;
        case SYS_setpgid:
            r->rax = do_sys_setpgid(a1, a2);
            break;
        case SYS_getpgrp:
            r->rax = do_sys_getpgrp();
            break;
        case SYS_getpgid:
            r->rax = do_sys_getpgid(a1);
            break;
        case SYS_setfsuid:
            r->rax = do_sys_setfsuid(a1);
            break;
        case SYS_setfsgid:
            r->rax = do_sys_setfsgid(a1);
            break;
        case SYS_setsid:
            r->rax = do_sys_setsid();
            break;
        case SYS_getsid:
            r->rax = do_sys_getsid(a1);
            break;
        case SYS_arch_prctl:
            r->rax = do_sys_arch_prctl(a1, a2);
            break;
        case SYS_mount:
            r->rax = do_sys_linux_mount(a1, a2, a3, a4, a5);
            break;

        default:
            r->rax = (uint64_t)-ENOSYS;
            break;
    }
    r->rflags &= ~(1ull << 8); /* never return to user with TF set */
    maybe_deliver_signal_on_sysret(r);
    {
        task_t *cur = process_current_task();
        if (cur && !cur->is_idle && cur->state == TASK_ZOMBIE) {
            scheduler_yield();
            for (;;) __asm__ __volatile__("sti; hlt");
        }
        if (cur && !cur->is_idle && cur->need_resched) {
            scheduler_yield();
        }
    }
    syscall_trace(nr, (int64_t)r->rax);
    g_syscall_debug_nr = 0;
    g_syscall_debug_rip = 0;
    g_syscall_debug_pid = 0;
#undef SCRUB_REGS_AFTER_EXEC
}

void syscall_init(void) {
    tty_reset_defaults();
    net_init_defaults();
    isr_register_interrupt_handler(128, syscall_handler);
}

void syscall_tty_irq_poll(void) {
    uint32_t pending = keyboard_take_sigint_pending();
    if (!pending) return;
    if (g_tty_foreground_pgid > 0) {
        (void)process_send_signal_pgid(g_tty_foreground_pgid, LINUX_SIGINT);
    }
}
