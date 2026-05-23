#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <time.h>
#include <sys/sysmacros.h>
#include <sys/reboot.h>
#include <sys/ioctl.h>
#include <sys/select.h>

volatile sig_atomic_t current_runlevel = 3; 
volatile sig_atomic_t child_died = 0;
volatile sig_atomic_t shutdown_requested = 0;

struct timespec boot_start;

/* массив пидов для консолей */
pid_t shell_pids[2] = {0, 0};
const char *tty_names[2] = {"tty1", "tty2"};

static void setup_signals(void);
static void do_log(const char *fmt, ...);
static void print_status(const char *color, const char *bracket_text, const char *msg);
static void silence_kernel_logs(void);
static int do_mount(const char *source, const char *target, const char *fstype, unsigned long flags);
static void parse_cmdline(void);
static void setup_console_and_tty(void);
static void set_env(void);
static void set_hostname(void);
static void install_busybox_symlinks(void);
static void load_kernel_modules(void);
static void clear_screen(void);
static void print_banner(void);
static void run_init_scripts(void);
static pid_t spawn_shell(const char *tty);
static void emergency_shell(void);
static void graceful_shutdown(void);
static int run_busybox(char *const argv[]);
static void setup_ipc(void);
static void update_status_file(void);

static int telinit_main(int argc, char *argv[]);
static int instatus_main(int argc, char *argv[]);

static void install_own_symlinks(void) {
    char exe_path[256];
    
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    if (len != -1) {
        exe_path[len] = '\0';
    } else {
        strcpy(exe_path, "/sbin/init");
    }

    symlink(exe_path, "/sbin/telinit");
    if (symlink(exe_path, "/usr/bin/telinit") != 0 && errno != EEXIST) {
        do_log("warning: cannot create /usr/bin/telinit: %s\n", strerror(errno));
    }
    if (symlink(exe_path, "/usr/bin/in-status") != 0 && errno != EEXIST) {
        do_log("warning: cannot create /usr/bin/in-status: %s\n", strerror(errno));
    }
}

int main(int argc, char *argv[]) {
    char *progname = strrchr(argv[0], '/');
    progname = progname ? progname + 1 : argv[0];

    if (strcmp(progname, "telinit") == 0) return telinit_main(argc, argv);
    if (strcmp(progname, "in-status") == 0) return instatus_main(argc, argv);

    if (getpid() != 1) {
        fprintf(stderr, "error: init must be run as PID 1\n");
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &boot_start);
    setup_signals();
    reboot(RB_DISABLE_CAD);

    /* базовые ФС */
    do_mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC);
    
    /* === ВАЖНО: ГЛУШИМ ЛОГИ ЯДРА ЧТОБЫ НЕ МУСОРИЛИ НА ЭКРАН === */
    silence_kernel_logs();

    do_mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC);
    do_mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID);
    
    mkdir("/run", 0755);
    do_mount("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC);

    mkdir("/sys/fs/cgroup", 0755);
    do_mount("cgroup2", "/sys/fs/cgroup", "cgroup2", MS_NOSUID | MS_NODEV | MS_NOEXEC);

    mknod("/dev/fb0", S_IFCHR | 0666, makedev(29, 0));
    mknod("/dev/null", S_IFCHR | 0666, makedev(1, 3));
    mknod("/dev/zero", S_IFCHR | 0666, makedev(1, 5));
    mknod("/dev/tty", S_IFCHR | 0666, makedev(5, 0));
    
    mkdir("/dev/pts", 0755);
    do_mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC);

    mknod("/dev/tty1", S_IFCHR | 0666, makedev(4, 1));
    mknod("/dev/tty2", S_IFCHR | 0666, makedev(4, 2));

    mkdir("/var", 0755);
    mkdir("/var/log", 0755);

    do_log("================================\n");
    do_log("init started. wilix oneinit v0.8\n");

    parse_cmdline();
    setup_ipc(); 

    if (current_runlevel == 0 || current_runlevel == 6) graceful_shutdown();
    if (current_runlevel == 1) {
        do_log("runlevel 1 requested. dropping to emergency shell.\n");
        emergency_shell();
        current_runlevel = 0;
        graceful_shutdown();
    }

    setup_console_and_tty();
    set_env();
    set_hostname();

    install_busybox_symlinks();
    load_kernel_modules();
    install_own_symlinks();

    /* Очистка и отрисовка красивого интерфейса */
    clear_screen();
    print_banner();

    /* Запуск служб с красивым выводом */
    run_init_scripts();

    /* Секьюрность */
    if (access("/usr/bin/doas", F_OK) == 0) {
        chown("/usr/bin/doas", 0, 0);
        chmod("/usr/bin/doas", 04755); 
    }
    if (access("/etc/doas.conf", F_OK) == 0) {
        chown("/etc/doas.conf", 0, 0);
        chmod("/etc/doas.conf", 0600);
    }

    update_status_file();

    struct timespec boot_end;
    clock_gettime(CLOCK_MONOTONIC, &boot_end);
    double elapsed = (boot_end.tv_sec - boot_start.tv_sec) + (boot_end.tv_nsec - boot_start.tv_nsec) / 1e9;
    
    printf("\n");
    print_status("1;36", " INFO ", "System initialized.");
    printf("         \033[1;30mBoot took %.2f seconds.\033[0m\n\n", elapsed);
    
    do_log("entering main init loop. runlevel = %d\n", current_runlevel);

    int fifo_fd = open("/run/initctl", O_RDWR | O_NONBLOCK);

    /* Главный цикл */
    while (!shutdown_requested) {
        if (current_runlevel == 3 || current_runlevel == 5) {
            for (int i = 0; i < 2; i++) {
                if (shell_pids[i] == 0) {
                    shell_pids[i] = spawn_shell(tty_names[i]);
                }
            }
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        if (fifo_fd >= 0) FD_SET(fifo_fd, &rfds);

        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;

        int retval = select((fifo_fd >= 0 ? fifo_fd + 1 : 0), &rfds, NULL, NULL, &tv);

        if (retval > 0 && fifo_fd >= 0 && FD_ISSET(fifo_fd, &rfds)) {
            char cmd;
            if (read(fifo_fd, &cmd, 1) == 1) {
                if (cmd >= '0' && cmd <= '6') {
                    current_runlevel = cmd - '0';
                    update_status_file();
                    if (current_runlevel == 0 || current_runlevel == 6) shutdown_requested = 1;
                    else if (current_runlevel == 1) {
                        kill(-1, SIGTERM);
                        emergency_shell();
                        shutdown_requested = 1; 
                    }
                }
            }
        }

        if (child_died) {
            child_died = 0;
            int status;
            pid_t p;
            while ((p = waitpid(-1, &status, WNOHANG)) > 0) {
                for (int i = 0; i < 2; i++) {
                    if (p == shell_pids[i]) shell_pids[i] = 0; 
                }
            }
        }
    }

    if (fifo_fd >= 0) close(fifo_fd);
    graceful_shutdown();
    return 0;
}

/* ================== УТИЛИТЫ УПРАВЛЕНИЯ ================== */

static int telinit_main(int argc, char *argv[]) {
    if (argc < 2) { printf("usage: telinit [0-6]\n"); return 1; }
    char cmd = argv[1][0];
    if (cmd < '0' || cmd > '6') { fprintf(stderr, "invalid runlevel\n"); return 1; }
    int fd = open("/run/initctl", O_WRONLY);
    if (fd < 0) return 1;
    write(fd, &cmd, 1);
    close(fd);
    return 0;
}

static int instatus_main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    printf("\033[1;36m=== wilix v0.8 init status ===\033[0m\n");
    FILE *f = fopen("/run/init_status", "r");
    if (!f) return 1;
    char line[256];
    while(fgets(line, sizeof(line), f)) printf("%s", line);
    fclose(f);
    printf("\033[1;36m==============================\033[0m\n");
    return 0;
}

/* ================== ВНУТРЯНКА INIT ================== */

/* Глушим ядро, чтобы printk не мусорил поверх нашего баннера */
static void silence_kernel_logs(void) {
    int fd = open("/proc/sys/kernel/printk", O_WRONLY);
    if (fd >= 0) {
        write(fd, "3 4 1 3\n", 8); /* Оставляем только критические ошибки ядра */
        close(fd);
    }
}

/* Красивый вывод статуса + запись в лог файл */
static void print_status(const char *color, const char *bracket_text, const char *msg) {
    printf("[\033[%sm%s\033[0m] %s\n", color, bracket_text, msg);
    fflush(stdout); /* Обязательно выталкиваем на экран перед возможным скриптом */
    do_log("[%s] %s\n", bracket_text, msg); 
}

/* Запись ТОЛЬКО в файл, чтобы не дублировать на экране и не мусорить в kmsg */
static void do_log(const char *fmt, ...) {
    va_list args;
    FILE *f = fopen("/var/log/init.log", "a");
    if (f) {
        time_t t = time(NULL);
        struct tm *tm = localtime(&t);
        fprintf(f, "[%02d:%02d:%02d] init: ", tm->tm_hour, tm->tm_min, tm->tm_sec);
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fclose(f);
    }
}

static void clear_screen(void) {
    write(STDOUT_FILENO, "\033[2J\033[H", 7);
}

static void print_banner(void) {
    /* Синяя полоса */
    printf("\033[0;34m========================================\033[0m\n");
    /* Голубой текст */
    printf("\033[1;36m  wilix v0.8 \033[1;30m//\033[1;36m oneinit v0.8\033[0m\n\n");
    printf("  welcome!\n");
    /* Градиентная полоса (От темного синего к светлому и голубому) */
    printf("\033[0;34m========\033[1;34m========\033[0;36m========\033[1;36m================\033[0m\n\n");
}

static void run_init_scripts(void) {
    struct dirent **namelist;
    int n;

    if (access("/etc/init.d", F_OK) != 0) return;

    n = scandir("/etc/init.d", &namelist, NULL, alphasort);
    if (n < 0) return;

    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            int is_S = (namelist[i]->d_name[0] == 'S');
            if ((pass == 0 && !is_S) || (pass == 1 && (is_S || namelist[i]->d_name[0] == '.'))) {
                continue;
            }

            char path[512];
            snprintf(path, sizeof(path), "/etc/init.d/%s", namelist[i]->d_name);
            if (access(path, X_OK) != 0) continue;

            /* Желтый WAIT с ровными отступами */
            print_status("1;33", "  WAIT  ", namelist[i]->d_name);
            
            pid_t pid = fork();
            if (pid == 0) {
                if (pass == 0) execl(path, namelist[i]->d_name, "start", (char *)NULL);
                else execl(path, namelist[i]->d_name, (char *)NULL);
                _exit(1);
            }
            
            int status;
            waitpid(pid, &status, 0);
            
            /* Проверка кода возврата скрипта */
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                print_status("1;32", "   OK   ", namelist[i]->d_name);
            } else {
                print_status("1;31", "  FAIL  ", namelist[i]->d_name);
            }
        }
    }

    for (int i = 0; i < n; i++) free(namelist[i]);
    free(namelist);
}

/* ОСТАЛЬНЫЕ ВСПОМОГАТЕЛЬНЫЕ ФУНКЦИИ (БЕЗ ИЗМЕНЕНИЙ) */
static void setup_ipc(void) {
    unlink("/run/initctl");
    if (mkfifo("/run/initctl", 0600) != 0) do_log("failed to create /run/initctl\n");
}

static void update_status_file(void) {
    FILE *f = fopen("/run/init_status", "w");
    if (f) {
        fprintf(f, "PID 1: running\nCurrent Runlevel: %d\n", current_runlevel);
        fclose(f);
    }
}

static void sig_handler(int sig) {
    if (sig == SIGCHLD) child_died = 1;
    else if (sig == SIGTERM || sig == SIGUSR1) { shutdown_requested = 1; current_runlevel = 0; }
    else if (sig == SIGINT) { shutdown_requested = 1; current_runlevel = 6; }
}

static void setup_signals(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP; 
    sigaction(SIGCHLD, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL); 
    sigaction(SIGUSR1, &sa, NULL); 
    signal(SIGQUIT, SIG_IGN);
    signal(SIGTSTP, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
}

static void parse_cmdline(void) {
    FILE *f = fopen("/proc/cmdline", "r");
    if (!f) return;
    char line[1024];
    if (fgets(line, sizeof(line), f)) {
        if (strstr(line, "runlevel=0") || strstr(line, " 0")) current_runlevel = 0;
        else if (strstr(line, "runlevel=1") || strstr(line, " single") || strstr(line, " 1")) current_runlevel = 1;
        else if (strstr(line, "runlevel=3") || strstr(line, " 3")) current_runlevel = 3;
        else if (strstr(line, "runlevel=5") || strstr(line, " 5")) current_runlevel = 5; 
        else if (strstr(line, "runlevel=6") || strstr(line, " 6")) current_runlevel = 6;
    }
    fclose(f);
}

static void graceful_shutdown(void) {
    printf("\n\033[1;31m*** INITIATING SHUTDOWN SEQUENCE ***\033[0m\n");
    printf("sending SIGTERM to all processes...\n");
    kill(-1, SIGTERM);
    sync(); 
    sleep(2); 

    printf("sending SIGKILL...\n");
    kill(-1, SIGKILL);
    sync();
    
    printf("unmounting filesystems...\n");
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/busybox", "busybox", "umount", "-a", "-r", (char *)NULL);
        _exit(1);
    }
    waitpid(pid, NULL, 0);

    if (current_runlevel == 6) {
        printf("rebooting system...\n");
        reboot(RB_AUTOBOOT);
    } else {
        printf("powering off...\n");
        reboot(RB_POWER_OFF);
    }
    while(1) pause(); 
}

static void emergency_shell(void) {
    printf("\033[1;31mDROPPING TO EMERGENCY SHELL!\033[0m\n");
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open("/dev/tty1", O_RDWR);
        if (fd >= 0) {
            dup2(fd, STDIN_FILENO); dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO);
            if (fd > 2) close(fd);
        }
        setenv("PS1", "(emergency) # ", 1);
        execl("/bin/busybox", "busybox", "sh", (char *)NULL);
        _exit(1); 
    }
    waitpid(pid, NULL, 0);
}

static int do_mount(const char *source, const char *target, const char *fstype, unsigned long flags) {
    if (mount(source, target, fstype, flags, NULL) != 0 && errno != EBUSY) {
        do_log("mount fail: %s on %s\n", source, target);
        return -1;
    }
    return 0;
}

static void setup_console_and_tty(void) {
    int fd = open("/dev/tty1", O_RDWR);
    if (fd >= 0) {
        pid_t pid = fork();
        if (pid == 0) {
            dup2(fd, STDIN_FILENO); dup2(fd, STDOUT_FILENO);
            execl("/bin/busybox", "busybox", "stty", "sane", (char *)NULL);
            _exit(1);
        }
        waitpid(pid, NULL, 0);
        dup2(fd, STDIN_FILENO); dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO);
        ioctl(fd, TIOCSCTTY, 1);
        if (fd > 2) close(fd);
    }
}

static void set_env(void) {
    setenv("PATH", "/bin:/sbin:/usr/bin:/usr/sbin", 1);
    setenv("HOME", "/", 1);
    setenv("SSL_CERT_FILE", "/etc/ssl/certs/ca-certificates.crt", 1);
}

static void set_hostname(void) {
    char hname[256] = "wilix";
    FILE *f = fopen("/etc/hostname", "r");
    if (f) {
        if (fgets(hname, sizeof(hname), f)) hname[strcspn(hname, "\r\n")] = 0;
        fclose(f);
    }
    if (hname[0] == '\0') strcpy(hname, "wilix");
    sethostname(hname, strlen(hname));
}

static void install_busybox_symlinks(void) {
    int pfd[2];
    if (pipe(pfd) < 0) {
        do_log("error: cannot create pipe for busybox setup: %s\n", strerror(errno));
        return;
    }

    print_status("1;33", "  WAIT  ", "installing busybox symlinks");

    pid_t pid = fork();
    if (pid == 0) {
        // Дочерний процесс: перенаправляем stdout в пайп и вызываем busybox --list
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        close(pfd[1]);
        execl("/bin/busybox", "busybox", "--list", (char *)NULL);
        _exit(1);
    }

    // Родительский процесс
    close(pfd[1]);
    FILE *stream = fdopen(pfd[0], "r");
    if (stream) {
        char cmd[128];
        char link_path[256];
        
        // Читаем каждую команду, которую выплюнул busybox --list
        while (fgets(cmd, sizeof(cmd), stream)) {
            // Убираем символ переноса строки \n
            cmd[strcspn(cmd, "\r\n")] = 0;
            if (cmd[0] == '\0') continue;

            // Формируем путь, куда положить ссылку (все пихаем в /bin)
            snprintf(link_path, sizeof(link_path), "/bin/%s", cmd);

            // Создаем симлинк. Если он уже есть, errno = EEXIST, это нормально, игнорируем
            if (symlink("/bin/busybox", link_path) != 0 && errno != EEXIST) {
                // Если не получилось, можно тихонько логгировать, но экран не засираем
                do_log("warning: failed to link %s: %s\n", link_path, strerror(errno));
            }
        }
        fclose(stream);
    }
    close(pfd[0]);
    waitpid(pid, NULL, 0);

    print_status("1;32", "   OK   ", "all busybox commands linked to /bin");
}

static void load_kernel_modules(void) {
    struct utsname uts;
    if (uname(&uts) != 0) {
        do_log("failed to get kernel version for modules\n");
        return;
    }

    // Вызываем "modprobe -a", чтобы загрузить конкретные критические модули
    // Busybox modprobe сам найдет их в /lib/modules/YOUR_KERNEL_VERSION/
    print_status("1;33", "  WAIT  ", "loading graphics modules");
    
    pid_t pid = fork();
    if (pid == 0) {
        // Загружаем наш графический стек одной командой
        execl("/bin/busybox", "busybox", "modprobe", "-a", "simpledrm", "bochs", "xe", (char *)NULL);
        _exit(1);
    }
    
    int status;
    waitpid(pid, &status, 0);
    
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        print_status("1;32", "   OK   ", "graphics modules loaded");
    } else {
        print_status("1;31", "  FAIL  ", "failed to load graphics modules");
    }
}

static pid_t spawn_shell(const char *tty) {
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        usleep(100000);
        char devpath[32];
        snprintf(devpath, sizeof(devpath), "/dev/%s", tty);
        execl("/bin/busybox", "busybox", "getty", "38400", tty, (char *)NULL);
        
        int fd = open(devpath, O_RDWR);
        if (fd >= 0) {
            ioctl(fd, TIOCSCTTY, 1);
            dup2(fd, STDIN_FILENO); dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO);
            if (fd > 2) close(fd);
            execl("/bin/busybox", "busybox", "login", "-p", (char *)NULL);
            execl("/bin/sh", "sh", (char *)NULL);
        }
        _exit(1);
    }
    return pid;
}

static int run_busybox(char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) {
        execv(argv[0], argv);
        _exit(1);
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
