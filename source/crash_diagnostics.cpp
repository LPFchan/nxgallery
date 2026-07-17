#include <nxgallery/crash_diagnostics.hpp>

#include <switch.h>

#include <cstdio>
#include <exception>
#include <sys/stat.h>

namespace {

constexpr char kCrashPath[] = "sdmc:/switch/nxgallery/crash.log";
constexpr char kFallbackCrashPath[] = "sdmc:/nxgallery-crash.log";

FILE *open_crash_log() {
    (void)mkdir("sdmc:/switch/nxgallery", 0777);
    FILE *output = std::fopen(kCrashPath, "w");
    return output != nullptr ? output : std::fopen(kFallbackCrashPath, "w");
}

void write_exception(FILE *output, const ThreadExceptionDump *context) {
    if (output == nullptr || context == nullptr) return;
    std::fprintf(output, "NXGALLERY_CRASH kind=exception error_desc=0x%08x\n",
                 context->error_desc);
    std::fprintf(output, "pc=0x%016llx lr=0x%016llx sp=0x%016llx fp=0x%016llx\n",
                 static_cast<unsigned long long>(context->pc.x),
                 static_cast<unsigned long long>(context->lr.x),
                 static_cast<unsigned long long>(context->sp.x),
                 static_cast<unsigned long long>(context->fp.x));
    std::fprintf(output, "esr=0x%08x far=0x%016llx pstate=0x%08x\n",
                 context->esr, static_cast<unsigned long long>(context->far.x),
                 context->pstate);
    for (int index = 0; index < 29; ++index) {
        std::fprintf(output, "x%02d=0x%016llx%s", index,
                     static_cast<unsigned long long>(context->cpu_gprs[index].x),
                     index % 2 == 1 ? "\n" : " ");
    }
    std::fprintf(output, "\n");
    std::fflush(output);
}

void terminate_handler() noexcept {
    FILE *output = open_crash_log();
    if (output != nullptr) {
        std::fprintf(output, "NXGALLERY_CRASH kind=cpp_terminate\n");
        std::fclose(output);
    }
    std::fprintf(stderr, "NXGALLERY_CRASH kind=cpp_terminate\n");
    std::fflush(stderr);
    diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_ShouldNotHappen));
}

}  // namespace

extern "C" {

alignas(16) u8 __nx_exception_stack[0x4000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);

void __libnx_exception_handler(ThreadExceptionDump *context) {
    FILE *output = open_crash_log();
    write_exception(output, context);
    if (output != nullptr) std::fclose(output);
    write_exception(stderr, context);
}

}  // extern "C"

namespace nxgallery {

void install_crash_diagnostics() noexcept { std::set_terminate(terminate_handler); }

}  // namespace nxgallery
