#include "CrashTrace.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <ctime>

namespace {
char g_Path[MAX_PATH] {};
char g_Header[160] {};
std::atomic<int> g_Reports { 0 };
LPTOP_LEVEL_EXCEPTION_FILTER g_Previous = nullptr;

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

const char* Name(DWORD p_Code) {
    switch (p_Code) {
        case EXCEPTION_ACCESS_VIOLATION: return "access violation";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "illegal instruction";
        case EXCEPTION_PRIV_INSTRUCTION: return "privileged instruction";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "integer divide by zero";
        case EXCEPTION_IN_PAGE_ERROR: return "in-page error";
        case EXCEPTION_STACK_OVERFLOW: return "stack overflow";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "array bounds exceeded";
        case 0xC0000374: return "heap corruption";
        case 0xE06D7363: return "unhandled C++ exception";
        default: return nullptr;
    }
}

void Write(EXCEPTION_POINTERS* p_Info, const char* p_Kind) {
    if (g_Reports.fetch_add(1) >= 4)
        return;

    FILE* s_File = nullptr;

    if (fopen_s(&s_File, g_Path, "a") != 0 || !s_File)
        return;

    const DWORD s_Code = p_Info->ExceptionRecord->ExceptionCode;
    const char* s_Name = Name(s_Code);
    char s_Where[MAX_PATH + 32];
    Describe(reinterpret_cast<DWORD64>(p_Info->ExceptionRecord->ExceptionAddress), s_Where, sizeof(s_Where));

    if (g_Header[0])
        std::fprintf(s_File, "%s\n", g_Header);

    if (s_Code == EXCEPTION_ACCESS_VIOLATION || s_Code == EXCEPTION_IN_PAGE_ERROR)
        std::fprintf(s_File, "%s at %s (%s 0x%llx), thread %lu%s\n", s_Name, s_Where,
            p_Info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
            static_cast<unsigned long long>(p_Info->ExceptionRecord->ExceptionInformation[1]), GetCurrentThreadId(), p_Kind);
    else
        std::fprintf(s_File, "%s (0x%08lx) at %s, thread %lu%s\n", s_Name ? s_Name : "exception", s_Code, s_Where, GetCurrentThreadId(), p_Kind);

    // Walk the faulting thread's stack from the exception context (not on a stack overflow: no room).
    CONTEXT s_Context = *p_Info->ContextRecord;

    for (int i = 0; i < 24 && s_Context.Rip && s_Code != EXCEPTION_STACK_OVERFLOW; ++i) {
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

    std::fflush(s_File);
    std::fclose(s_File);
}

// First sight of a hardware fault (the game handles none of these itself).
LONG CALLBACK OnException(EXCEPTION_POINTERS* p_Info) {
    const DWORD s_Code = p_Info->ExceptionRecord->ExceptionCode;

    if (s_Code != 0xE06D7363 && Name(s_Code))
        Write(p_Info, "");

    return EXCEPTION_CONTINUE_SEARCH;
}

// Anything nobody handled, C++ exceptions included (a player's game went down with no report).
LONG WINAPI OnUnhandled(EXCEPTION_POINTERS* p_Info) {
    if (!Name(p_Info->ExceptionRecord->ExceptionCode) || p_Info->ExceptionRecord->ExceptionCode == 0xE06D7363)
        Write(p_Info, " (unhandled)");

    return g_Previous ? g_Previous(p_Info) : EXCEPTION_CONTINUE_SEARCH;
}
}

void CrashTrace::Install(const std::string& p_Path, const std::string& p_Header) {
    strncpy_s(g_Path, p_Path.c_str(), _TRUNCATE);

    // Who crashed, and when: two players' reports side by side.
    char s_Time[32] {};
    const std::time_t s_Now = std::time(nullptr);
    std::tm s_Tm {};

    if (localtime_s(&s_Tm, &s_Now) == 0)
        std::strftime(s_Time, sizeof(s_Time), "%Y-%m-%d %H:%M:%S", &s_Tm);

    std::snprintf(g_Header, sizeof(g_Header), "%s (game started %s)", p_Header.c_str(), s_Time);
    AddVectoredExceptionHandler(1, OnException);
    g_Previous = SetUnhandledExceptionFilter(OnUnhandled);
}
