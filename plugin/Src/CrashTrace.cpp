#include "CrashTrace.h"
#include "BlackBox.h"

#include <Windows.h>
#include <DbgHelp.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace {
char g_Path[MAX_PATH] {};
char g_CaughtPath[MAX_PATH] {}; // faults the mod caught: never a crash, so never in g_Path
char g_BoxPath[MAX_PATH] {};
char g_DumpPath[MAX_PATH] {};
char g_Header[160] {};
std::atomic<int> g_Reports { 0 };
std::atomic<int> g_Guarded { 0 };
std::atomic<bool> g_Dumped { false };
LPTOP_LEVEL_EXCEPTION_FILTER g_Previous = nullptr;
HMODULE g_Self = nullptr;
bool g_WriteDump = true;

using MiniDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
    PMINIDUMP_CALLBACK_INFORMATION);
MiniDumpFn g_MiniDump = nullptr;

// The mod's own functions (HitmanCoop.sym beside the DLL: "rva name" lines, sorted), read once at
// start so a crash inside the mod names the function without the developer's symbol file.
std::vector<uint32_t> g_SymbolRvas;
std::vector<uint32_t> g_SymbolNames; // offsets into g_SymbolText
std::vector<char> g_SymbolText;

void LoadSymbols(const char* p_Path) {
    FILE* s_File = nullptr;

    if (fopen_s(&s_File, p_Path, "rb") != 0 || !s_File)
        return;

    char s_Line[512];

    while (std::fgets(s_Line, sizeof(s_Line), s_File)) {
        char* s_End = nullptr;
        const unsigned long s_Rva = std::strtoul(s_Line, &s_End, 16);

        if (s_End == s_Line || *s_End != ' ')
            continue;

        ++s_End;
        size_t s_Length = std::strlen(s_End);

        while (s_Length && (s_End[s_Length - 1] == '\n' || s_End[s_Length - 1] == '\r'))
            --s_Length;

        if (!s_Length || (!g_SymbolRvas.empty() && s_Rva < g_SymbolRvas.back()))
            continue;

        g_SymbolRvas.push_back(static_cast<uint32_t>(s_Rva));
        g_SymbolNames.push_back(static_cast<uint32_t>(g_SymbolText.size()));
        g_SymbolText.insert(g_SymbolText.end(), s_End, s_End + s_Length);
        g_SymbolText.push_back(0);
    }

    std::fclose(s_File);
}

// "SdkWorld::UpdatePoses+0x1c" for an address in the mod, or nothing.
void Symbol(DWORD64 p_Address, char* p_Out, size_t p_Size) {
    p_Out[0] = 0;

    if (!g_Self || g_SymbolRvas.empty() || p_Address < reinterpret_cast<DWORD64>(g_Self))
        return;

    const DWORD64 s_Rva = p_Address - reinterpret_cast<DWORD64>(g_Self);

    if (s_Rva > 0xFFFFFFFFull)
        return;

    const auto s_It = std::upper_bound(g_SymbolRvas.begin(), g_SymbolRvas.end(), static_cast<uint32_t>(s_Rva));

    if (s_It == g_SymbolRvas.begin())
        return;

    const size_t s_Index = static_cast<size_t>(s_It - g_SymbolRvas.begin()) - 1;
    std::snprintf(p_Out, p_Size, "  %s+0x%llx", &g_SymbolText[g_SymbolNames[s_Index]],
        static_cast<unsigned long long>(s_Rva - g_SymbolRvas[s_Index]));
}

// "HITMAN3.exe+0x1234", or the raw address when it isn't in a loaded module.
HMODULE Describe(DWORD64 p_Address, char* p_Out, size_t p_Size) {
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

    return s_Module;
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

// Readable memory at p_Address, p_Size bytes: copied under SEH.
bool Peek(DWORD64 p_Address, uint8_t* p_Out, size_t p_Size) {
    __try {
        std::memcpy(p_Out, reinterpret_cast<const void*>(p_Address), p_Size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void WriteDump(EXCEPTION_POINTERS* p_Info) {
    if (!g_WriteDump || !g_MiniDump || !g_DumpPath[0] || g_Dumped.exchange(true))
        return;

    const HANDLE s_File = CreateFileA(g_DumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (s_File == INVALID_HANDLE_VALUE)
        return;

    MINIDUMP_EXCEPTION_INFORMATION s_Exception {};
    s_Exception.ThreadId = GetCurrentThreadId();
    s_Exception.ExceptionPointers = p_Info;
    s_Exception.ClientPointers = FALSE;
    g_MiniDump(GetCurrentProcess(), GetCurrentProcessId(), s_File,
        static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithUnloadedModules), &s_Exception, nullptr,
        nullptr);
    CloseHandle(s_File);
}

void Write(EXCEPTION_POINTERS* p_Info, const char* p_Kind) {
    // A fault the mod expects and catches (a guarded call into the game): one line in the flight
    // recorder, and a short report for the first few, never a dump.
    const char* s_GuardedWhat = BlackBox::GuardedWhat();

    if (s_GuardedWhat) {
        BlackBox::Note("guard", "fault caught in %s at %p", s_GuardedWhat, p_Info->ExceptionRecord->ExceptionAddress);

        if (g_Guarded.fetch_add(1) >= 3)
            return;
    }
    else if (g_Reports.fetch_add(1) >= 4) {
        return;
    }

    FILE* s_File = nullptr;

    if (fopen_s(&s_File, s_GuardedWhat ? g_CaughtPath : g_Path, "a") != 0 || !s_File)
        return;

    const DWORD s_Code = p_Info->ExceptionRecord->ExceptionCode;
    const char* s_Name = Name(s_Code);
    char s_Where[MAX_PATH + 32];
    char s_Symbol[300];
    Describe(reinterpret_cast<DWORD64>(p_Info->ExceptionRecord->ExceptionAddress), s_Where, sizeof(s_Where));

    if (g_Header[0])
        std::fprintf(s_File, "%s\n", g_Header);

    if (s_GuardedWhat)
        std::fprintf(s_File, "(caught by the mod, the game goes on: a fault inside %s)\n", s_GuardedWhat);

    if (s_Code == EXCEPTION_ACCESS_VIOLATION || s_Code == EXCEPTION_IN_PAGE_ERROR)
        std::fprintf(s_File, "%s at %s (%s 0x%llx), thread %lu%s\n", s_Name, s_Where,
            p_Info->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
            static_cast<unsigned long long>(p_Info->ExceptionRecord->ExceptionInformation[1]), GetCurrentThreadId(), p_Kind);
    else
        std::fprintf(s_File, "%s (0x%08lx) at %s, thread %lu%s\n", s_Name ? s_Name : "exception", s_Code, s_Where, GetCurrentThreadId(), p_Kind);

    const char* s_Section = BlackBox::Section();
    std::fprintf(s_File, "the mod was in: %s\n", s_Section && s_Section[0] ? s_Section : "(not in the mod's frame update)");

    // The registers and the instruction itself: enough to tell what was null on any game build.
    const CONTEXT& c = *p_Info->ContextRecord;
    std::fprintf(s_File, "  rax=%llx rcx=%llx rdx=%llx r8=%llx r9=%llx r10=%llx r11=%llx rsp=%llx\n", c.Rax, c.Rcx, c.Rdx, c.R8, c.R9, c.R10,
        c.R11, c.Rsp);

    uint8_t s_Bytes[16];

    if (Peek(c.Rip, s_Bytes, sizeof(s_Bytes))) {
        std::fprintf(s_File, "  code:");

        for (const uint8_t s_Byte : s_Bytes)
            std::fprintf(s_File, " %02X", s_Byte);

        std::fprintf(s_File, "\n");
    }

    // Walk the faulting thread's stack from the exception context (not on a stack overflow: no room).
    CONTEXT s_Context = *p_Info->ContextRecord;

    for (int i = 0; i < 32 && s_Context.Rip && s_Code != EXCEPTION_STACK_OVERFLOW; ++i) {
        const HMODULE s_Module = Describe(s_Context.Rip, s_Where, sizeof(s_Where));
        DWORD64 s_ImageBase = 0;
        PRUNTIME_FUNCTION s_Function = RtlLookupFunctionEntry(s_Context.Rip, &s_ImageBase, nullptr);
        s_Symbol[0] = 0;

        if (s_Module == g_Self)
            Symbol(s_Context.Rip, s_Symbol, sizeof(s_Symbol));
        else if (s_Function && s_Module)
            std::snprintf(s_Symbol, sizeof(s_Symbol), "  (function at +0x%lx)", s_Function->BeginAddress);

        std::fprintf(s_File, "  #%02d %-26s rbx=%llx rbp=%llx rsi=%llx rdi=%llx r12=%llx r13=%llx r14=%llx r15=%llx%s\n", i,
            s_Where, s_Context.Rbx, s_Context.Rbp, s_Context.Rsi, s_Context.Rdi, s_Context.R12, s_Context.R13,
            s_Context.R14, s_Context.R15, s_Symbol);

        if (!s_Function) {
            // Leaf function: the return address is on top of the stack.
            uint8_t s_Return[8];

            if (!Peek(s_Context.Rsp, s_Return, sizeof(s_Return)))
                break;

            std::memcpy(&s_Context.Rip, s_Return, sizeof(s_Return));
            s_Context.Rsp += 8;
            continue;
        }

        void* s_HandlerData = nullptr;
        DWORD64 s_EstablisherFrame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, s_ImageBase, s_Context.Rip, s_Function, &s_Context, &s_HandlerData,
            &s_EstablisherFrame, nullptr);
    }

    if (!s_GuardedWhat) {
        std::fprintf(s_File, "the last things the mod did (all of it: %s):\n", g_BoxPath[0] ? strrchr(g_BoxPath, '\\') ? strrchr(g_BoxPath, '\\') + 1 : g_BoxPath : "-");
        std::fflush(s_File);
        std::fclose(s_File);

        // The tail of the flight recorder into the report, and all of it beside the report.
        const HANDLE s_Tail = CreateFileA(g_Path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (s_Tail != INVALID_HANDLE_VALUE) {
            BlackBox::WriteRaw(s_Tail, 40);
            DWORD s_Written = 0;
            WriteFile(s_Tail, "\r\n", 2, &s_Written, nullptr);
            CloseHandle(s_Tail);
        }

        if (g_BoxPath[0]) {
            const HANDLE s_Box = CreateFileA(g_BoxPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

            if (s_Box != INVALID_HANDLE_VALUE) {
                BlackBox::WriteRaw(s_Box, 0);
                CloseHandle(s_Box);
            }
        }

        WriteDump(p_Info);
        return;
    }

    std::fprintf(s_File, "\n");
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

void CrashTrace::Install(const std::string& p_Path, const std::string& p_Header, const std::string& p_BlackBoxPath, const std::string& p_DumpPath,
    bool p_WriteDump) {
    strncpy_s(g_Path, p_Path.c_str(), _TRUNCATE);
    {
        // "HitmanCoop-crash.txt" -> "HitmanCoop-crash-caught.txt"; anything else gets ".caught".
        std::string s_Caught = p_Path;
        const size_t s_Dot = s_Caught.size() >= 4 ? s_Caught.size() - 4 : std::string::npos;

        if (s_Dot != std::string::npos && _stricmp(s_Caught.c_str() + s_Dot, ".txt") == 0)
            s_Caught.insert(s_Dot, "-caught");
        else
            s_Caught += ".caught";

        strncpy_s(g_CaughtPath, s_Caught.c_str(), _TRUNCATE);
    }
    strncpy_s(g_BoxPath, p_BlackBoxPath.c_str(), _TRUNCATE);
    strncpy_s(g_DumpPath, p_DumpPath.c_str(), _TRUNCATE);
    g_WriteDump = p_WriteDump;

    // Who crashed, and when: two players' reports side by side.
    char s_Time[32] {};
    const std::time_t s_Now = std::time(nullptr);
    std::tm s_Tm {};

    if (localtime_s(&s_Tm, &s_Now) == 0)
        std::strftime(s_Time, sizeof(s_Time), "%Y-%m-%d %H:%M:%S", &s_Tm);

    std::snprintf(g_Header, sizeof(g_Header), "%s (game started %s)", p_Header.c_str(), s_Time);

    // The mod's own module and its function names.
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&CrashTrace::Install), &g_Self);

    if (g_Self && g_SymbolRvas.empty()) {
        char s_Dll[MAX_PATH] {};

        if (GetModuleFileNameA(g_Self, s_Dll, MAX_PATH)) {
            std::string s_Symbols(s_Dll);
            const size_t s_Dot = s_Symbols.find_last_of('.');
            LoadSymbols((s_Symbols.substr(0, s_Dot) + ".sym").c_str());
        }
    }

    // Loaded now: not something to do from inside a crash.
    if (p_WriteDump && !g_MiniDump)
        if (const HMODULE s_DbgHelp = LoadLibraryA("dbghelp.dll"))
            g_MiniDump = reinterpret_cast<MiniDumpFn>(GetProcAddress(s_DbgHelp, "MiniDumpWriteDump"));

    AddVectoredExceptionHandler(1, OnException);
    g_Previous = SetUnhandledExceptionFilter(OnUnhandled);
}

size_t CrashTrace::SymbolCount() {
    return g_SymbolRvas.size();
}

int CrashTrace::Reports() {
    return g_Reports.load();
}

int CrashTrace::CaughtFaults() {
    return g_Guarded.load();
}
