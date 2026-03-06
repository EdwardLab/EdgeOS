#ifndef SYS_PROCESS_H
#define SYS_PROCESS_H

#include <stdint.h>

#define TASK_NAME_MAX 32
#define TASK_CWD_MAX 256
#define PROC_MAX_TASKS 64
#define PROCESS_USER_VMA_MAX 256
#define USER_HEAP_MAX_DELTA (32ULL * 1024ULL * 1024ULL)
#define USER_HEAP_DEFAULT_DELTA (32ULL * 1024ULL * 1024ULL)
#define USER_HEAP_PY_EXTRA_DELTA (8ULL * 1024ULL * 1024ULL)
#define PROCESS_CTTY_NONE 0
#define PROCESS_CTTY_CONSOLE 1
#define PROCESS_CTTY_PTY 2

typedef enum {
    TASK_UNUSED = 0,
    TASK_RUNNABLE,
    TASK_RUNNING,
    TASK_BLOCKED,
    TASK_ZOMBIE,
} task_state_t;

typedef struct cpu_context {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t rip;
    uint64_t rsp;
} cpu_context_t;

typedef struct edge_trap_frame {
    uint64_t rax, rbx, rcx, rdx;
    uint64_t rbp, rdi, rsi;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t int_no;
    uint64_t err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} edge_trap_frame_t;

typedef struct edge_user_vma {
    uint64_t start;
    uint64_t end;
    uint32_t prot;
    uint32_t flags;
} edge_user_vma_t;

typedef struct task_struct {
    int pid;
    int ppid;
    int exit_code;
    task_state_t state;
    char name[TASK_NAME_MAX];
    cpu_context_t context;
    edge_trap_frame_t fork_tf;
    uint64_t cr3;
    uint64_t kernel_stack_top;
    uint64_t user_stack_top;
    uint64_t user_heap_base;
    uint64_t user_brk;
    uint64_t user_heap_limit;
    uint64_t user_mmap_next;
    uint16_t user_vma_count;
    uint16_t _user_vma_pad;
    uint64_t fs_base;
    uint64_t start_entry;
    uint64_t start_argv;
    uint64_t start_envp;
    uint64_t start_at_phdr;
    uint64_t start_at_phnum;
    uint64_t start_at_entry;
    uint64_t start_at_base;
    int start_argc;
    int start_envc;
    int start_pending;
    uint32_t uid;
    uint32_t gid;
    uint32_t euid;
    uint32_t egid;
    uint32_t umask;
    int pgid;
    int sid;
    int ctty_kind;
    int ctty_id;
    char cwd[TASK_CWD_MAX];
    char root[TASK_CWD_MAX];
    uint64_t sigint_handler;
    uint64_t sigalrm_handler;
    uint64_t sigterm_handler;
    uint64_t sigchld_handler;
    uint64_t sigmask;
    uint64_t itimer_real_next_us;
    uint64_t itimer_real_interval_us;
    uint8_t itimer_real_active;
    uint8_t sigint_pending;
    uint8_t sigalrm_pending;
    uint8_t sigterm_pending;
    uint8_t sigchld_pending;
    uint8_t sig_stub_installed;
    uint64_t sigaltstack_sp;
    uint64_t sigaltstack_size;
    uint32_t sigaltstack_flags;
    uint8_t need_resched;
    uint8_t on_runqueue;
    uint8_t is_idle;
    int assigned_cpu;
    struct task_struct *rq_prev;
    struct task_struct *rq_next;
    struct task_struct *parent;
    struct task_struct *first_child;
    struct task_struct *sibling_prev;
    struct task_struct *sibling_next;
    char start_argbuf[32][128];
    char *start_argv_local[33];
    char start_envbuf[32][128];
    char *start_envp_local[33];
    uint8_t fxsave_region[512] __attribute__((aligned(16)));
    edge_user_vma_t user_vmas[PROCESS_USER_VMA_MAX];
} task_t;

void process_init(void);
int process_fork(const edge_trap_frame_t *parent_tf);
int process_set_current(int pid);
int process_getpid(void);
int process_getppid(void);
int process_prepare_exec_current(void);
int process_spawn_exec(const char *path, int argc, char **argv);
void process_exit_current(int code);
int process_wait_any(int *status);
int process_wait_pid(int pid, int *status, int options);
int process_adopt_orphans(int from_ppid, int to_ppid);
int process_kill_pid(int pid, int code);
void process_list_print(void);
const task_t *process_get_task(int pid);
const task_t *process_task_by_index(int index);
task_t *process_current_task(void);
int process_set_fs_base(uint64_t base);
uint64_t process_get_fs_base(void);
uint32_t process_getuid(void);
uint32_t process_getgid(void);
uint32_t process_geteuid(void);
uint32_t process_getegid(void);
uint32_t process_setuid(uint32_t uid);
uint32_t process_setgid(uint32_t gid);
int process_setreuid(uint32_t ruid, uint32_t euid);
int process_setregid(uint32_t rgid, uint32_t egid);
uint32_t process_set_umask(uint32_t new_mask);
int process_setpgid(int pid, int pgid);
int process_getpgid(int pid);
int process_getsid(int pid);
int process_setsid(void);
int process_kill_pgid(int pgid, int code);
int process_send_signal(int pid, int sig);
int process_send_signal_pgid(int pgid, int sig);
int process_set_state(int pid, task_state_t state);
int process_pick_target_cpu(void);

#endif
