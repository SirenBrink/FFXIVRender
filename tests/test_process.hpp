#pragma once
#ifdef _WIN32
#include <windows.h>
#include <cstdlib>
struct TestProcessPolicy {
    TestProcessPolicy() {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        _set_error_mode(_OUT_TO_STDERR);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    }
};
inline TestProcessPolicy test_process_policy;
#endif
