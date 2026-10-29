#include "xivrender/fg_backend.hpp"
#include <windows.h>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace xivrender::fg;
namespace {
void require(bool okay, const std::string &error) { if (!okay) throw std::runtime_error(error); }
void write_byte(unsigned char *at, unsigned char value) {
    DWORD protection = 0, ignored = 0;
    require(VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &protection) != FALSE, "Test page protection");
    *at = value;
    FlushInstructionCache(GetCurrentProcess(), at, 1);
    require(VirtualProtect(at, 1, protection, &ignored) != FALSE, "Restore page protection");
}
}
int wmain(int argc, wchar_t **argv) try {
    require(argc == 2, "Usage: xefg_lifecycle_probe full-path-to-libxess_fg.dll");
    for (unsigned cycle = 0; cycle < 6; ++cycle) {
        const auto module = LoadLibraryExW(argv[1], nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        require(module != nullptr, "Load verified XeFG runtime");
        auto *base = reinterpret_cast<unsigned char *>(module);
        auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
        auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
        struct Section { unsigned offset; std::vector<unsigned char> bytes; };
        std::vector<Section> pristine;
        auto *sections = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            if (sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
                const auto offset = sections[i].VirtualAddress;
                pristine.push_back({offset, {base + offset, base + offset + sections[i].Misc.VirtualSize}});
            }
        require(!pristine.empty(), "No executable section to verify");
        std::string error;
        require(unlock_xefg(module, error), error);
        require(base[0x25c0] == 0xff && base[0x25c1] == 0x25, "Present hook not installed");
        if (!cycle) {
            // A foreign edit must survive failed teardown. Restore the test's
            // byte afterwards, then verify the retry completes all restoration.
            const auto ours = base[0x25c0];
            write_byte(base + 0x25c0, 0xcc);
            require(!restore_xefg(module, error) && base[0x25c0] == 0xcc,
                    "Teardown overwrote a hook it did not own");
            write_byte(base + 0x25c0, ours);
        }
        require(restore_xefg(module, error), error);
        for (const auto &s : pristine)
            require(std::memcmp(base + s.offset, s.bytes.data(), s.bytes.size()) == 0,
                    "XeFG executable bytes did not return to their original state");
        require(restore_xefg(module, error), "Repeated restoration must be harmless");
        require(FreeLibrary(module) != FALSE, "XeFG module release failed");
        HMODULE retained = nullptr;
        require(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(module), &retained),
                "XeFG module remained loaded after the last owner released it");
    }
    std::cout << "PASS: six XeFG patch/restore/unload cycles; exact executable-byte restoration; foreign-hook refusal\n";
    return 0;
} catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
