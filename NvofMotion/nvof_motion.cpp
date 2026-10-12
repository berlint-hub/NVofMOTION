// NVOF Motion - phase 1a: probe skeleton.
//
// Goal of this phase: validate the whole chain with near-zero crash risk:
// build in CI (ReShade headers only, no CUDA toolkit yet), load in game,
// probe for the NVIDIA driver runtime (nvofapi64.dll) and report status.
// No frame processing happens yet.
//
// Later phases:
//  1b - CUDA context + NvOF session create/destroy + caps query
//   2 - frame capture -> NvOFExecute on worker thread -> R16G16F binding

#include <windows.h>

#include <mutex>
#include <string>

#include <imgui.h>
#include <reshade.hpp>

// NOTE: NvOF headers (nvOpticalFlowCommon.h / nvOpticalFlowCuda.h) arrive in
// phase 1b together with the CUDA toolkit. Phase 1a only probes for the
// driver runtime via LoadLibrary/GetProcAddress, so no NvOF headers needed.

using namespace reshade;
using namespace reshade::api;

namespace {

std::mutex g_mutex;
HMODULE g_nvof_dll = nullptr;
bool g_entry_found = false;
bool g_d3d12_seen = false;
unsigned int g_enabled = 1;
std::string g_status = "not initialized yet";

void set_status_locked(const char *text, reshade::log::level level)
{
	g_status.assign(text);
	reshade::log::message(level, text);
}

} // namespace

static void on_init_device(device *device)
{
	if (g_enabled == 0)
		return;
	if (device->get_api() != device_api::d3d12)
		return; // v1 supports D3D12 only; stay silent elsewhere

	std::lock_guard<std::mutex> lock(g_mutex);
	g_d3d12_seen = true;

	if (g_nvof_dll != nullptr)
		return; // already probed

	g_nvof_dll = LoadLibraryW(L"nvofapi64.dll");
	if (g_nvof_dll == nullptr)
	{
		set_status_locked("nvofapi64.dll NOT found - no NVIDIA driver with NvOF, staying idle", reshade::log::level::warning);
		return;
	}

	g_entry_found = GetProcAddress(g_nvof_dll, "NvOFAPICreateInstanceCuda") != nullptr;
	if (g_entry_found)
		set_status_locked("NvOF entry found - HW optical flow available (phase 1 probe, no processing yet)", reshade::log::level::info);
	else
		set_status_locked("nvofapi64.dll loaded but NvOFAPICreateInstanceCuda missing", reshade::log::level::warning);
}

static void on_destroy_device(device *)
{
	// Nothing to tear down in phase 1a; the probed DLL stays loaded until unload.
}

static void draw_overlay(effect_runtime *)
{
	std::string status;
	bool d3d12_seen = false;
	bool entry_found = false;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		status = g_status;
		d3d12_seen = g_d3d12_seen;
		entry_found = g_entry_found;
	}

	ImGui::TextUnformatted("NVOF Motion (phase 1: probe only)");

	bool enabled = g_enabled != 0;
	if (ImGui::Checkbox("Enable", &enabled))
	{
		g_enabled = enabled ? 1u : 0u;
		reshade::set_config_value(nullptr, "NVOF", "Enable", g_enabled);
	}

	ImGui::Text("D3D12 device seen: %s", d3d12_seen ? "yes" : "no");
	ImGui::Text("NvOF entry: %s", entry_found ? "found" : "not found");
	ImGui::TextWrapped("%s", status.c_str());
}

static void register_addon_nvof()
{
	reshade::register_overlay(nullptr, draw_overlay);
	reshade::register_event<reshade::addon_event::init_device>(on_init_device);
	reshade::register_event<reshade::addon_event::destroy_device>(on_destroy_device);
}

static void unregister_addon_nvof()
{
	reshade::unregister_event<reshade::addon_event::init_device>(on_init_device);
	reshade::unregister_event<reshade::addon_event::destroy_device>(on_destroy_device);
}

extern "C" __declspec(dllexport) const char *NAME = "NVOF Motion";
extern "C" __declspec(dllexport) const char *DESCRIPTION = "HW optical-flow motion vectors via NVIDIA NVOFA (phase 1: probe only).";

BOOL APIENTRY DllMain(HMODULE hModule, DWORD fdwReason, LPVOID)
{
	switch (fdwReason)
	{
	case DLL_PROCESS_ATTACH:
		if (!reshade::register_addon(hModule))
			return FALSE;
		reshade::get_config_value(nullptr, "NVOF", "Enable", g_enabled);
		register_addon_nvof();
		break;
	case DLL_PROCESS_DETACH:
		unregister_addon_nvof();
		reshade::unregister_addon(hModule);
		// NOTE: g_nvof_dll is intentionally NOT freed here; FreeLibrary inside
		// DllMain would risk a loader-lock deadlock. OS reclaims it on exit.
		break;
	}

	return TRUE;
}
