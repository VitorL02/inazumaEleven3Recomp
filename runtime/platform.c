#include <stdio.h>
#include <string.h>
#include "platform.h"
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
const uint8_t *plat_map_file(const char *path, size_t *size) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(f, &sz) || sz.QuadPart == 0) { CloseHandle(f); return NULL; }
    HANDLE m = CreateFileMappingA(f, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(f);
    if (!m) return NULL;
    void *p = MapViewOfFile(m, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(m);
    if (!p) return NULL;
    *size = (size_t)sz.QuadPart;
    return p;
}
void plat_unmap_file(const uint8_t *p, size_t size) { (void)size; UnmapViewOfFile(p); }
int plat_self_exe(char *out, size_t n) { DWORD k = GetModuleFileNameA(NULL, out, (DWORD)n); return k > 0 && k < n ? 0 : -1; }
double plat_now(void) {
    static LARGE_INTEGER fr; LARGE_INTEGER t;
    if (!fr.QuadPart) QueryPerformanceFrequency(&fr);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)fr.QuadPart;
}
void plat_sleep(double sec) { if (sec > 0) Sleep((DWORD)(sec * 1000)); }
intptr_t plat_spawn_quiet(const char *exe, const char *const argv[]) {
    char cmd[4096] = {0};
    size_t k = 0;
    for (int i = 0; argv[i]; i++) k += (size_t)snprintf(cmd + k, sizeof cmd - k, "%s\"%s\"", i ? " " : "", argv[i]);
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) return 0;
    CloseHandle(pi.hThread);
    return (intptr_t)pi.hProcess;
}
int plat_proc_done(intptr_t h) {
    if (WaitForSingleObject((HANDLE)h, 0) == WAIT_OBJECT_0) { CloseHandle((HANDLE)h); return 1; }
    return 0;
}
void plat_proc_kill(intptr_t h) { TerminateProcess((HANDLE)h, 1); CloseHandle((HANDLE)h); }
int plat_mkdir(const char *path) { return CreateDirectoryA(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS ? 0 : -1; }
void plat_log_to_file(const char *self_exe, const char *name) {
    HANDLE h = GetStdHandle(STD_ERROR_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) return; /* tem console ou saída redirecionada: deixa como está */
    char path[1200]; snprintf(path, sizeof path, "%s", self_exe);
    char *s = strrchr(path, '\\'), *s2 = strrchr(path, '/');
    if (s2 > s) s = s2;
    if (s) s[1] = 0; else path[0] = 0;
    strncat(path, name, sizeof path - strlen(path) - 1);
    if (freopen(path, "w", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
}
static void build_cmd(char *cmd, size_t sz, const char *const argv[]) {
    size_t k = 0; cmd[0] = 0;
    for (int i = 0; argv[i] && k < sz; i++) k += (size_t)snprintf(cmd + k, sz - k, "%s\"%s\"", i ? " " : "", argv[i]);
}
intptr_t plat_spawn_logged(const char *exe, const char *const argv[], const char *log, const char *cwd) {
    static HANDLE job;
    if (!job) {
        job = CreateJobObjectA(NULL, NULL);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li; memset(&li, 0, sizeof li);
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (job) SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
    HANDLE lf = CreateFileA(log, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    static char cmd[8192]; build_cmd(cmd, sizeof cmd, argv);
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    if (lf != INVALID_HANDLE_VALUE) { si.dwFlags = STARTF_USESTDHANDLES; si.hStdOutput = si.hStdError = lf; si.hStdInput = NULL; }
    BOOL ok = CreateProcessA(exe, cmd, NULL, NULL, lf != INVALID_HANDLE_VALUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, cwd, &si, &pi);
    if (lf != INVALID_HANDLE_VALUE) CloseHandle(lf);
    if (!ok) return 0;
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread); CloseHandle(pi.hThread);
    return (intptr_t)pi.hProcess;
}
int plat_spawn_detached(const char *exe, const char *const argv[], const char *cwd) {
    static char cmd[8192]; build_cmd(cmd, sizeof cmd, argv);
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof si); si.cb = sizeof si;
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, cwd, &si, &pi)) return -1;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return 0;
}
int plat_file_exists(const char *path) { DWORD a = GetFileAttributesA(path); return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY); }
int plat_pick_file(char *out, size_t n) {
    OPENFILENAMEA o; memset(&o, 0, sizeof o);
    out[0] = 0;
    o.lStructSize = sizeof o; o.lpstrFile = out; o.nMaxFile = (DWORD)n;
    o.lpstrFilter = "ROM do Nintendo DS (*.nds)\0*.nds\0Todos os arquivos\0*.*\0";
    o.lpstrTitle = "Escolha a ROM de Inazuma Eleven 3";
    o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    return GetOpenFileNameA(&o) ? 0 : -1;
}
#else
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
const uint8_t *plat_map_file(const char *path, size_t *size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || st.st_size <= 0) { close(fd); return NULL; }
    void *m = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) return NULL;
    *size = (size_t)st.st_size;
    return m;
}
void plat_unmap_file(const uint8_t *p, size_t size) { munmap((void *)p, size); }
int plat_self_exe(char *out, size_t n) {
#ifdef __APPLE__
    uint32_t sz = (uint32_t)n; return _NSGetExecutablePath(out, &sz);
#else
    ssize_t k = readlink("/proc/self/exe", out, n - 1);
    if (k <= 0) return -1;
    out[k] = 0; return 0;
#endif
}
double plat_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
void plat_sleep(double sec) {
    if (sec <= 0) return;
    struct timespec ts = {(time_t)sec, (long)((sec - (time_t)sec) * 1e9)};
    nanosleep(&ts, NULL);
}
intptr_t plat_spawn_quiet(const char *exe, const char *const argv[]) {
    extern char **environ;
    pid_t pid;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    int ok = posix_spawn(&pid, exe, &fa, NULL, (char *const *)argv, environ) == 0;
    posix_spawn_file_actions_destroy(&fa);
    return ok ? (intptr_t)pid : 0;
}
int plat_proc_done(intptr_t h) { int st; return waitpid((pid_t)h, &st, WNOHANG) == (pid_t)h; }
void plat_proc_kill(intptr_t h) { int st; kill((pid_t)h, SIGKILL); waitpid((pid_t)h, &st, 0); }
void plat_log_to_file(const char *self_exe, const char *name) { (void)self_exe; (void)name; }
int plat_mkdir(const char *path) { return mkdir(path, 0755) == 0 || errno == EEXIST ? 0 : -1; }
static pid_t g_logged_pid;
static void kill_logged(void) { if (g_logged_pid > 0) kill(-g_logged_pid, SIGKILL); }
intptr_t plat_spawn_logged(const char *exe, const char *const argv[], const char *log, const char *cwd) {
    pid_t pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) {
        setpgid(0, 0); /* grupo próprio: o launcher encerra o preparo inteiro (python, zig) ao sair */
        if (cwd && chdir(cwd)) _exit(127);
        int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        execv(exe, (char *const *)argv);
        _exit(127);
    }
    setpgid(pid, pid);
    if (!g_logged_pid) atexit(kill_logged);
    g_logged_pid = pid;
    return (intptr_t)pid;
}
int plat_spawn_detached(const char *exe, const char *const argv[], const char *cwd) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        setsid();
        if (cwd && chdir(cwd)) _exit(127);
        execv(exe, (char *const *)argv);
        _exit(127);
    }
    return 0;
}
int plat_file_exists(const char *path) { struct stat st; return stat(path, &st) == 0 && S_ISREG(st.st_mode); }
int plat_pick_file(char *out, size_t n) {
    FILE *p = popen("zenity --file-selection --title='Escolha a ROM de Inazuma Eleven 3' --file-filter='*.nds' 2>/dev/null", "r");
    if (!p) return -1;
    out[0] = 0;
    if (!fgets(out, (int)n, p)) out[0] = 0;
    pclose(p);
    size_t k = strlen(out);
    while (k && (out[k - 1] == '\n' || out[k - 1] == '\r')) out[--k] = 0;
    return k ? 0 : -1;
}
#endif
