#include "xivrender/fg_backend.hpp"
#include "xivrender/xefg_pacing.hpp"
#include <array>
#include <cstring>
namespace xivrender::fg {
namespace {
struct Patch { unsigned rva; std::vector<unsigned char> old, edit; };
const std::array<Patch, 5> patches{
    {{0x20da4f, {0x0f, 0x85, 0xcc, 0, 0, 0}, {0xe9, 0xcd, 0, 0, 0, 0x90}},
     {0x1a5de4, {0x74, 0x09}, {0xeb, 0x06}},
     {0x1a517d, {0xbb, 3, 0, 0, 0}, {0xbb, 3, 0, 0, 0}},
     {0x1a45c2, {0xc7, 0x87, 0x6c, 1, 0, 0, 1, 0, 0, 0}, {0xc7, 0x87, 0x6c, 1, 0, 0, 3, 0, 0, 0}},
     {0x20973b, {0xb8, 1, 0, 0, 0}, {0xb8, 3, 0, 0, 0}}}};
HMODULE patched = nullptr;
}
double xefg_render_time_ms() {
    return XeFGPacing::RenderTimeMs();
}
void xefg_note_fed_frame_time(double milliseconds) {
    XeFGPacing::NoteFedFrameTime(milliseconds);
}
void xefg_request_pacing_recovery() {
    XeFGPacing::RequestRecovery();
}
bool unlock_xefg(HMODULE module, std::string &error) {
    if (!module) {
        error = "XeFG runtime is not loaded";
        return false;
    }
    auto *base = reinterpret_cast<unsigned char *>(module);
    const auto *dos = reinterpret_cast<const IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 || dos->e_lfanew > 4096)
        return false;
    const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->FileHeader.TimeDateStamp != 0x69cb0f4d || nt->OptionalHeader.SizeOfImage != 0x15ed000) {
        error = "XeFG 4X unlock requires the verified 1.3.1.78 runtime";
        return false;
    }
    if (patched == module)
        return true;
    // Exact build, executable section bounds and every pristine instruction are
    // verified before the first edit. No on-disk provider is modified.
    for (const auto &p : patches) {
        bool executable = false;
        const auto *section = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            if ((section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) && p.rva >= section[i].VirtualAddress &&
                p.rva + p.old.size() <= section[i].VirtualAddress + section[i].Misc.VirtualSize)
                executable = true;
        if (!executable || std::memcmp(base + p.rva, p.old.data(), p.old.size())) {
            error = "XeFG unlock instruction verification failed; runtime unchanged";
            return false;
        }
    }
    unsigned written = 0;
    for (const auto &p : patches) {
        ++written;
        if (!XeFGPacing::WriteVerified(base + p.rva, p.edit.data(), unsigned(p.edit.size()))) {
            for (unsigned i = 0; i < written; ++i)
                XeFGPacing::WriteVerified(base + patches[i].rva, patches[i].old.data(),
                                          unsigned(patches[i].old.size()));
            error = "XeFG unlock write failed; rolled back";
            return false;
        }
    }
    if (!XeFGPacing::Install(base)) {
        for (const auto &p : patches)
            XeFGPacing::WriteVerified(base + p.rva, p.old.data(), unsigned(p.old.size()));
        error = "XeFG per-frame pacing could not be installed";
        return false;
    }
    patched = module;
    return true;
}
bool restore_xefg(HMODULE module, std::string &error) {
    if (!patched) return true;
    if (patched != module) { error = "XeFG patch owner mismatch"; return false; }
    auto *base = reinterpret_cast<unsigned char *>(module);
    if (!XeFGPacing::Uninstall()) { error = "XeFG pacing hook restoration failed"; return false; }
    for (const auto &p : patches) {
        if (std::memcmp(base + p.rva, p.old.data(), p.old.size()) == 0) continue;
        if (std::memcmp(base + p.rva, p.edit.data(), p.edit.size()) ||
            !XeFGPacing::WriteVerified(base + p.rva, p.old.data(), unsigned(p.old.size()))) {
            error = "XeFG capability patch restoration failed";
            return false;
        }
    }
    patched = nullptr;
    return true;
}
} // namespace xivrender::fg
