#include "CrashTrace.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>

namespace {
char g_Path[MAX_PATH] {};
std::atomic<int> g_Reports { 0 };

// "HITMAN3.exe+0x1234", or the raw address when it isn't in a loaded module.
void Describe(DWORD64 p_Address, char* p_Out, size_t p_Size) {
    HMODULE s_Module = nullptr;

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(p_Address), &s_Module) && s_Module) {
        char s_Name[MAX_PATH] {};
        GetModuleFileNameA(s_Module, s_Name, MAX_PATH);
        const char* s_Base = strrchr(s_Name, '\\');
        std::snprintf(p_Out, p_Size, "%s+0x%llx", s_Base ? s_Base + 1 : s_Name,
            static_cast<unsigned long long>(p_Address - reinterpret_cast<DWORD64>(s_Module)));
    }
    else {
        std::snprintf(p_Out, p_Size, "0x%llx", static_cast<unsigned long long>(p_Address));
    }
}

LONG CALLBACK OnException(EXCEPTION_POINTERS* p_Info) {
    if (p_Info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || g_Reports.fetch_add(1) >= 4)
        return EXCEPTION_CONTINUE_SEARCH;

    FILE* s_File = nullptr;

    if (fopen_s(&s_File, g_Path, "a") != 0 || !s_File)
        return EXCEPTION_CONTINUE_SEARCH;

    char s_Where[MAX_PATH + 32];
    Describe(reinterpret_cast<DWORD64>(p_Info->ExceptionRecord->ExceptionAddress), s_Where, sizeof(s_Where));
    std::fprintf(s_File, "access violation at %s (%s 0x%llx), thread %lu\n", s_Where,
        p_Info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
        static_cast<unsigned long long>(p_Info->ExceptionRecord->ExceptionInformation[1]), GetCurrentThreadId());

    // Walk the faulting thread's stack from the exception context.
    CONTEXT s_Context = *p_Info->ContextRecord;

    for (int i = 0; i < 24 && s_Context.Rip; ++i) {
        Describe(s_Context.Rip, s_Where, sizeof(s_Where));
        std::fprintf(s_File, "  #%02d %-26s rbx=%llx rbp=%llx rsi=%llx rdi=%llx r12=%llx r13=%llx r14=%llx r15=%llx\n", i,
            s_Where, s_Context.Rbx, s_Context.Rbp, s_Context.Rsi, s_Context.Rdi, s_Context.R12, s_Context.R13,
            s_Context.R14, s_Context.R15);

        DWORD64 s_ImageBase = 0;
        PRUNTIME_FUNCTION s_Function = RtlLookupFunctionEntry(s_Context.Rip, &s_ImageBase, nullptr);

        if (!s_Function) {
            // Leaf function: the return address is on top of the stack.
            s_Context.Rip = *reinterpret_cast<DWORD64*>(s_Context.Rsp);
            s_Context.Rsp += 8;
            continue;
        }

        void* s_HandlerData = nullptr;
        DWORD64 s_EstablisherFrame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, s_ImageBase, s_Context.Rip, s_Function, &s_Context, &s_HandlerData,
            &s_EstablisherFrame, nullptr);
    }

    std::fclose(s_File);
    return EXCEPTION_CONTINUE_SEARCH;
}
}

void CrashTrace::Install(const std::string& p_Path) {
    strncpy_s(g_Path, p_Path.c_str(), _TRUNCATE);
    AddVectoredExceptionHandler(1, OnException);
}
