/*
 * xbased native core — boot and run a title.
 */
#ifndef NATIVE_CORE_CORE_H
#define NATIVE_CORE_CORE_H

/* siginfo's si_code of a SIGFPE the platform's fault handler passes on: a
 * quotient that doesn't fit, not a divisor of zero (fault_win.c) */
#define NC_FPE_OVERFLOW 0x4E43

#include "native/core/options.h"
#include "native/core/vm.h"
#include "native/core/xbe_load.h"
#include "native/core/xdvdfs.h"
#include "native/cpu/cpu.h"
#include "native/kernel/kernel.h"

#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct NcCore {
    NcVm vm;
    NcDisc disc;
    NcKernel kernel;
    NcXbe xbe;
    NcCpu *cpu;
    NcThread *boot_thread;
    bool restored_state;
} NcCore;

/* Sets up guest memory and the kernel, loads the selected XBE and
 * prepares the title's main thread. Takes ownership of disc_fd. */
bool nc_core_boot(NcCore *c, int disc_fd, const NcBootOptions *boot);

/* Runs the guest until it stops; logs the frontier summary. */
void nc_core_run(NcCore *c);

/* The core works through faults: device registers (MMIO), the HLE GPU's
 * dirty tracking and the JIT's call-ret stack all trap. A front end's
 * SIGSEGV/SIGBUS handler calls this first; true means handled (return from
 * the handler), false that the fault is someone else's. */
bool nc_core_fault(NcCore *c, int sig, siginfo_t *info, void *ucontext);
#if defined(__x86_64__)
/* x86-64 (fault_win.c; on Linux over SIGSEGV): a vectored exception handler that hands the
 * process's access violations to nc_core_fault for c. */
void nc_core_fault_install(NcCore *c);
/* Called first by the crash handler (an unhandled exception): the HLE GPU's
 * note of a driver crash (hw/xbox/hle/vk/precision.h). NULL: none. */
extern void (*nc_crash_hook)(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
