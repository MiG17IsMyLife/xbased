/*
 * xbased — the guest's memory and Windows' address space randomization
 * (win_aslr.h).
 */
#ifdef _WIN32
#include "native/app/win_aslr.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef PROCESS_CREATION_MITIGATION_POLICY_BOTTOM_UP_ASLR_ALWAYS_ON
#define PROCESS_CREATION_MITIGATION_POLICY_BOTTOM_UP_ASLR_ALWAYS_ON (0x00000001ull << 16)
#endif
#ifndef PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_ON
#define PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_ON (0x00000001ull << 20)
#endif

bool xb_quote_arg(char *cmd, size_t capacity, const char *arg)
{
    const size_t original = strlen(cmd);
    size_t n = original;
    if (capacity <= n || capacity - n < (n ? 4u : 3u)) {
        return false;
    }
    if (n) {
        cmd[n++] = ' ';
    }
    cmd[n++] = '"';
    for (;;) {
        size_t slashes = 0;
        while (*arg == '\\') {
            slashes++;
            arg++;
        }
        if (*arg == '"' || !*arg) {
            slashes *= 2;
        }
        const size_t chars = *arg == '"' ? 2 : *arg ? 1 : 0;
        if (capacity - n < slashes + chars + 2) {
            cmd[original] = '\0';
            return false;
        }
        memset(cmd + n, '\\', slashes);
        n += slashes;
        if (!*arg) {
            break;
        }
        if (*arg == '"') {
            cmd[n++] = '\\';
        }
        cmd[n++] = *arg++;
    }
    cmd[n++] = '"';
    cmd[n] = '\0';
    return true;
}

bool xb_spawn_under(const char *exe, char *cmd, const char *dir, void *parent_window, PROCESS_INFORMATION *pi)
{
    STARTUPINFOEXA si;
    ZeroMemory(&si, sizeof(si));
    si.StartupInfo.cb = sizeof(si);
    /* the window's process as the new one's parent: its process tree (what
     * OBS's game capture records the sound of, the window's process and its
     * children's) has the new one in it */
    HANDLE parent = NULL;
    DWORD parent_pid = 0;
    if (parent_window && GetWindowThreadProcessId((HWND)parent_window, &parent_pid) && parent_pid) {
        parent = OpenProcess(PROCESS_CREATE_PROCESS, FALSE, parent_pid);
    }
    char buf[512];
    SIZE_T size = 0;
    const DWORD attrs = parent ? 2 : 1;
    InitializeProcThreadAttributeList(NULL, attrs, 0, &size);
    LPPROC_THREAD_ATTRIBUTE_LIST list = size <= sizeof(buf) ? (LPPROC_THREAD_ATTRIBUTE_LIST)buf : NULL;
    DWORD64 policy = PROCESS_CREATION_MITIGATION_POLICY_BOTTOM_UP_ASLR_ALWAYS_ON |
                     PROCESS_CREATION_MITIGATION_POLICY_HIGH_ENTROPY_ASLR_ALWAYS_ON;
    bool with_parent = false;
    if (list && InitializeProcThreadAttributeList(list, attrs, 0, &size)) {
        if (UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY, &policy, sizeof(policy),
                                      NULL, NULL)) {
            si.lpAttributeList = list;
        }
        if (parent && UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &parent,
                                                sizeof(parent), NULL, NULL)) {
            si.lpAttributeList = list;
            with_parent = true;
        }
    }
    bool ok = false;
    char cwd[MAX_PATH];
    if (si.lpAttributeList) {
        /* this process's environment and folder, given: not the parent's */
        LPWCH env = with_parent ? GetEnvironmentStringsW() : NULL;
        if (with_parent && !dir && GetCurrentDirectoryA(sizeof(cwd), cwd) - 1u < sizeof(cwd) - 1u) {
            dir = cwd;
        }
        ok = CreateProcessA(exe, cmd, NULL, NULL, FALSE,
                            EXTENDED_STARTUPINFO_PRESENT | (env ? CREATE_UNICODE_ENVIRONMENT : 0), env, dir,
                            &si.StartupInfo, pi);
        if (env) {
            FreeEnvironmentStringsW(env);
        }
        DeleteProcThreadAttributeList(list);
    }
    if (parent) {
        CloseHandle(parent);
    }
    if (!ok && with_parent) {
        return xb_spawn(exe, cmd, dir, pi);   /* the parent refused it: this process's child, as before */
    }
    if (!ok) {
        /* a Windows that doesn't take the attribute: as before */
        si.StartupInfo.cb = sizeof(STARTUPINFOA);
        ok = CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si.StartupInfo, pi);
    }
    return ok;
}

bool xb_spawn(const char *exe, char *cmd, const char *dir, PROCESS_INFORMATION *pi)
{
    return xb_spawn_under(exe, cmd, dir, NULL, pi);
}

bool xb_aslr_low_taken(void)
{
    uint64_t at = 0x10000;
    while (at < (1ull << 32)) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void *)(uintptr_t)at, &mbi, sizeof(mbi))) {
            return false;
        }
        const uint64_t lo = (uint64_t)(uintptr_t)mbi.BaseAddress, hi = lo + mbi.RegionSize;
        /* KUSER_SHARED_DATA and its neighbour, in every process, are no
         * part of the guest's (vm_win.cpp reserves around them) */
        if (mbi.State != MEM_FREE && !(lo >= 0x7FFE0000ull && hi <= 0x7FFF0000ull)) {
            return true;
        }
        at = hi;
    }
    return false;
}

void xb_aslr_check(void)
{
    if (!xb_aslr_low_taken()) {
        return;
    }
    PROCESS_MITIGATION_ASLR_POLICY aslr;
    if (GetProcessMitigationPolicy(GetCurrentProcess(), ProcessASLRPolicy, &aslr, sizeof(aslr)) &&
        aslr.EnableBottomUpRandomization && aslr.EnableHighEntropy) {
        return;     /* on already: the boot says what holds the memory (vm_win.cpp) */
    }
    char exe[MAX_PATH];
    if (GetEnvironmentVariableA("XBASED_ASLR_RUN", NULL, 0) || !GetModuleFileNameA(NULL, exe, sizeof(exe))) {
        return;     /* the run started again already: once */
    }
    SetEnvironmentVariableA("XBASED_ASLR_RUN", "1");
    static char cmd[32768];
    snprintf(cmd, sizeof(cmd), "%s", GetCommandLineA());
    PROCESS_INFORMATION pi;
    if (!xb_spawn(exe, cmd, NULL, &pi)) {
        return;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    TerminateProcess(GetCurrentProcess(), code);
}
#endif
