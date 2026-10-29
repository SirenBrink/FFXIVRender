#include <windows.h>
#include <shellapi.h>
extern int wmain(int, wchar_t **);
extern bool desktop_auto_start;
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    desktop_auto_start = true;
    int count = 0;
    auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    const int result = wmain(count, args);
    LocalFree(args);
    return result;
}
