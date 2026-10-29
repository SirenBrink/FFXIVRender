// Direct NR snippet boundary. It does not load or patch Streamline or the driver NGX core.
#pragma once
#include <cstdint>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct NVSDK_NGX_Parameter;

namespace xivrender::nr {
struct Session;
inline constexpr std::uint32_t success = 1;
inline constexpr std::uint32_t invalid_argument = 0xf1000001;
inline constexpr std::uint32_t invalid_model = 0xf1000002;
inline constexpr std::uint32_t load_failed = 0xf1000003;
inline constexpr std::uint32_t missing_export = 0xf1000004;
inline constexpr std::uint32_t busy = 0xf1000005;
inline constexpr std::uint32_t foreign_feature = 0xf1000006;
inline constexpr std::uint32_t internal_error = 0xf1000007;
} // namespace xivrender::nr

// Parameters, commands and device must remain alive until Close succeeds. The
// owner serializes calls and must fence all submitted creation/evaluation work
// before Release. Close refuses live features. No function waits on the GPU.
// Open accepts one explicit absolute model path, including unsigned compatibility
// models supplied by the user. It never changes process DLL search settings.
// Use a fresh owned parameter block for each generation, and discard it after
// Close: populated callbacks point into that exact model module.
// Open can return a nonnull session with a failure if rollback shutdown fails;
// retain that session and retry Close instead of unloading its caller DLL.
#ifdef native_nr_bridge_EXPORTS
#define XIVNR_API __declspec(dllexport)
#else
#define XIVNR_API
#endif
extern "C" {
// Only reads callbacks from an already initialized SR model into an owned block.
// These never initialize/shut down SR or touch the game's parameter block.
XIVNR_API std::uint32_t __cdecl XIVSR_PopulateCallbacks(void *module, NVSDK_NGX_Parameter *parameters);
XIVNR_API std::uint32_t __cdecl XIVSR_InvokeCallback(void *callback, NVSDK_NGX_Parameter *parameters);
XIVNR_API std::uint32_t __cdecl XIVNR_Open(const wchar_t *model, const wchar_t *data, ID3D12Device *device,
                                           std::uint64_t application_id, NVSDK_NGX_Parameter *parameters,
                                           xivrender::nr::Session **session);
XIVNR_API std::uint32_t __cdecl XIVNR_Create(xivrender::nr::Session *session,
                                             ID3D12GraphicsCommandList *commands,
                                             NVSDK_NGX_Parameter *parameters, void **feature);
XIVNR_API std::uint32_t __cdecl XIVNR_Evaluate(xivrender::nr::Session *session,
                                               ID3D12GraphicsCommandList *commands, const void *feature,
                                               NVSDK_NGX_Parameter *parameters);
XIVNR_API std::uint32_t __cdecl XIVNR_Release(xivrender::nr::Session *session, void *feature);
XIVNR_API std::uint32_t __cdecl XIVNR_Close(xivrender::nr::Session *session);
}
