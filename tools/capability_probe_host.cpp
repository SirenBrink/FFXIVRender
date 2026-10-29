#include <windows.h>
#include <cstdio>
int wmain(int argc, wchar_t **argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    if (argc != 3) return 2;
    const auto library = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!library) return 3;
    const auto call = reinterpret_cast<int(__cdecl *)(const wchar_t *, int(__cdecl *)())>(GetProcAddress(library, "ProbeOptimal"));
    if (!call) return 4;
    const auto result = call(argv[2], nullptr); FreeLibrary(library); return result;
}
