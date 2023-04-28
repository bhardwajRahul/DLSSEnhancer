#define WIN32_LEAN_AND_MEAN
#include <ntstatus.h>
#define WIN32_NO_STATUS
#include <Windows.h>
#include <winternl.h>

#include <spdlog/spdlog.h>
#include <Patterns.h>

#include "DLSSTweaks.hpp"

void DlssNvidiaPresetOverrides::zero_customized_values()
{
	// DlssNvidiaPresetOverrides struct we change seems to last the whole lifetime of the game
	// So we'll back up the orig values in case user later decides to set them back to default
	if (!settings.dlss.nvidiaOverrides.has_value())
	{
		settings.dlss.nvidiaOverrides = *this;

		if (overrideDLAA || overrideQuality || overrideBalanced || overridePerformance || overrideUltraPerformance)
		{
			spdlog::debug("NVIDIA default presets for current app:");
			if (overrideDLAA)
				spdlog::debug(" - DLAA: {}", utility::DLSS_PresetEnumToName(overrideDLAA));
			if (overrideQuality)
				spdlog::debug(" - Quality: {}", utility::DLSS_PresetEnumToName(overrideQuality));
			if (overrideBalanced)
				spdlog::debug(" - Balanced: {}", utility::DLSS_PresetEnumToName(overrideBalanced));
			if (overridePerformance)
				spdlog::debug(" - Performance: {}", utility::DLSS_PresetEnumToName(overridePerformance));
			if (overrideUltraPerformance)
				spdlog::debug(" - UltraPerformance: {}", utility::DLSS_PresetEnumToName(overrideUltraPerformance));
		}
	}

	// Then zero out NV-provided override if user has set their own override for that level
	overrideDLAA = settings.presetDLAA ? 0 : settings.dlss.nvidiaOverrides->overrideDLAA;
	overrideQuality = settings.presetQuality ? 0 : settings.dlss.nvidiaOverrides->overrideQuality;
	overrideBalanced = settings.presetBalanced ? 0 : settings.dlss.nvidiaOverrides->overrideBalanced;
	overridePerformance = settings.presetPerformance ? 0 : settings.dlss.nvidiaOverrides->overridePerformance;
	overrideUltraPerformance = settings.presetUltraPerformance ? 0 : settings.dlss.nvidiaOverrides->overrideUltraPerformance;
}

namespace nvngx_dlss
{
// Allow force enabling/disabling the DLSS debug display via RegQueryValueExW hook
// (fallback method if we couldn't find the indicator value check pattern in the DLL)
LSTATUS(__stdcall* RegQueryValueExW_Orig)(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData);
LSTATUS __stdcall RegQueryValueExW_Hook(HKEY hKey, LPCWSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData)
{
	LSTATUS ret = RegQueryValueExW_Orig(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData);
	if (settings.overrideDlssHud == 0 || _wcsicmp(lpValueName, L"ShowDlssIndicator") != 0)
		return ret;

	if (lpcbData && *lpcbData >= 4 && lpData)
	{
		DWORD* outData = (DWORD*)lpData;
		if (settings.overrideDlssHud >= 1)
			*outData = 0x400;
		else if (settings.overrideDlssHud < 0)
			*outData = 0;
		return ERROR_SUCCESS; // always return success for DLSS to accept the result
	}

	return ret;
}

SafetyHookInline DLSS_GetIndicatorValue_Hook;
uint32_t __fastcall DLSS_GetIndicatorValue(void* thisptr, uint32_t* OutValue)
{
	auto ret = DLSS_GetIndicatorValue_Hook.thiscall<uint32_t>(thisptr, OutValue);
	if (settings.overrideDlssHud == 0)
		return ret;
	*OutValue = settings.overrideDlssHud > 0 ? 0x400 : 0;
	return ret;
}

// Hooks that allow us to override the NV-provided DLSS3.1 presets, without needing us to override the whole AppID for the game
// I'd imagine leaving games AppID might help DLSS keep using certain game-specific things, and also probably let any DLSS telemetry be more accurate too
// @NVIDIA, please consider adding a parameter to tell DLSS to ignore the NV / DRS provided ones instead
// (or a parameter which lets it always use the game provided NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_XXX param)
// Don't really enjoy needing to inline hook your code for this, but it's necessary atm...
SafetyHookMid DlssPresetOverrideFunc_LaterVersion_Hook;
int8_t DlssPresetOverrideFunc_MovOffset1 = -0x38;
int8_t DlssPresetOverrideFunc_MovOffset2 = -0x3C;
void DlssPresetOverrideFunc_LaterVersion(SafetyHookContext& ctx)
{
	// Code that we overwrote
	*(uint64_t*)(ctx.rbp + DlssPresetOverrideFunc_MovOffset1) = ctx.rdi;
	*(uint32_t*)(ctx.rbp + DlssPresetOverrideFunc_MovOffset2) = uint32_t(ctx.rcx);

	auto* nv_settings = (DlssNvidiaPresetOverrides*)ctx.rax;
	nv_settings->zero_customized_values();
}

// Seems earlier DLSS 3.1 builds (3.1.2 and older?) don't use the func above, but some kind of inlined version
// So instead we'll hook the code that would call into it instead
SafetyHookMid DlssPresetOverrideFunc_EarlyVersion_Hook;
void DlssPresetOverrideFunc_Version3_1_2(SafetyHookContext& ctx)
{
	// Code that we overwrote
	*(uint32_t*)(ctx.rbx + 0x1C) = uint32_t(ctx.rcx);
	ctx.rax = *(uint64_t*)ctx.rdi;

	auto* nv_settings = (DlssNvidiaPresetOverrides*)ctx.rax;
	nv_settings->zero_customized_values();
}
void DlssPresetOverrideFunc_Version3_1_1(SafetyHookContext& ctx)
{
	// Code that we overwrote
	*(uint32_t*)(ctx.rdi + 0x1C) = uint32_t(ctx.rcx);
	//ctx.rax = *(uint64_t*)ctx.rsi;

	auto* nv_settings = (DlssNvidiaPresetOverrides*)ctx.rax;
	nv_settings->zero_customized_values();
}

// Hook that overrides the method DLSS uses to find the preset from the rendering resolution
// Seems DLSS just checks what the ratio is against display res, then maps certain ratio ranges to specific quality level
// (eg. if you had UltraPerf set to 0.666, DLSS is hardcoded to treat 0.666 as Quality, so would use the preset assigned to Quality instead)
// Instead we'll hook that code so we can check against the users customized ratios, and fetch the correct preset for each level
// 
// NOTE: this currently only works on 3.1.11 dev/release
//   some reason earlier versions duplicated the code that picks (and inits?) the preset, across all 5 blocks that check each ratio range
//   making it a lot harder for us to be able to hook it and change the preset (since we'd also have to copy the code that inits it all too..)
//   for now I'm fine leaving this as a 3.1.11+ only hook
SafetyHookMid CreateDlssInstance_PresetSelection_Hook;
uint8_t CreateDlssInstance_PresetSelection_Register = 0x83; // register used to access DLSS struct in this func, unfortunately changes between dev/release
void CreateDlssInstance_PresetSelection(SafetyHookContext& ctx)
{
	uint8_t* dlssStruct = 0;
	if (CreateDlssInstance_PresetSelection_Register == 0x83) // 0x83 = rbx
		dlssStruct = (uint8_t*)ctx.rbx;
	else if (CreateDlssInstance_PresetSelection_Register == 0x86) // 0x86 = rsi
		dlssStruct = (uint8_t*)ctx.rsi;

	if (!dlssStruct)
		return;

	// Code that we overwrote (TODO: does safetyhook midhook even require this?)
	*(uint32_t*)(dlssStruct + 0x1F8) = 3;

	if (!settings.overrideQualityLevels)
		return;

	int dlssWidth = *(int*)(dlssStruct + 0x48);
	int dlssHeight = *(int*)(dlssStruct + 0x4C);
	int displayWidth = *(int*)(dlssStruct + 0x60);
	int displayHeight = *(int*)(dlssStruct + 0x64);

	std::optional<unsigned int> presetValue;
	presetValue.reset();

	for (auto kvp : qualityLevelResolutionsCurrent)
	{
		// Check that both height & width are within 1 pixel of each other either way
		bool widthIsClose = abs(kvp.second.first - dlssWidth) <= 1;
		bool heightIsClose = abs(kvp.second.second - dlssHeight) <= 1;
		if (widthIsClose && heightIsClose)
		{
			auto preset = kvp.first;

			if (preset == NVSDK_NGX_PerfQuality_Value_MaxPerf && settings.presetPerformance)
				presetValue = settings.presetPerformance;
			else if (preset == NVSDK_NGX_PerfQuality_Value_Balanced && settings.presetBalanced)
				presetValue = settings.presetBalanced;
			else if (preset == NVSDK_NGX_PerfQuality_Value_MaxQuality && settings.presetQuality)
				presetValue = settings.presetQuality;
			else if (preset == NVSDK_NGX_PerfQuality_Value_UltraPerformance && settings.presetUltraPerformance)
				presetValue = settings.presetUltraPerformance;
			else if (preset == NVSDK_NGX_PerfQuality_Value_UltraQuality && settings.presetUltraQuality)
				presetValue = settings.presetUltraQuality;

			break;
		}
	}

	if (!presetValue.has_value())
	{
		// No match found for DLSS presets, check whether this could be DLAA
		if (settings.presetDLAA)
		{
			bool widthIsClose = abs(displayWidth - dlssWidth) <= 1;
			bool heightIsClose = abs(displayHeight - dlssHeight) <= 1;
			if (widthIsClose && heightIsClose)
				presetValue = settings.presetDLAA;
		}

		// If no value override set, return now so DLSS will use whatever it was going to originally
		if (!presetValue.has_value())
			return;
	}

	if (CreateDlssInstance_PresetSelection_Register == 0x83) // release DLL, preset stored in rdx
		ctx.rdx = *presetValue;
	else if (CreateDlssInstance_PresetSelection_Register == 0x86) // dev DLL, preset stored in r15
		ctx.r15 = *presetValue;
}

SafetyHookMid dlssIndicatorHudHook{};
bool hook(HMODULE ngx_module)
{
	// Search for & hook the function that overrides the DLSS presets with ones set by NV
	// So that users can set custom DLSS presets without needing to override the whole app ID
	// (if OverrideAppId is set there shouldn't be any need for this)
	if (!settings.overrideAppId)
	{
		auto pattern = hook::pattern((void*)ngx_module, "41 0F 45 CE 48 89 7D ? 89 4D ? 48 8D 0D");
		if (pattern.size())
		{
			uint8_t* match = pattern.count(1).get_first<uint8_t>();
			DlssPresetOverrideFunc_MovOffset1 = int8_t(match[7]);
			DlssPresetOverrideFunc_MovOffset2 = int8_t(match[10]);

			DlssPresetOverrideFunc_LaterVersion_Hook = safetyhook::create_mid(match + 4, DlssPresetOverrideFunc_LaterVersion);

			spdlog::info("nvngx_dlss: applied DLSS preset override hook (> v3.1.2)");
		}
		else
		{
			// Couldn't find the preset override func, seems it might be inlined inside earlier DLLs...
			// Search for & hook the inlined code instead
			// (unfortunately registers changed between 3.1.1 & 3.1.2, and probably the ones between 3.1.2 and 3.1.11 too, ugh)
			pattern = hook::pattern((void*)ngx_module, "89 4D ? 8B 08 89 ? 1C 48 8B ?");
			if (!pattern.size())
				spdlog::warn("nvngx_dlss: failed to apply DLSS preset override hooks, recommend enabling OverrideAppId instead");
			else
			{
				uint8_t* match = pattern.count(1).get_first<uint8_t>();
				uint8_t* midhookAddress = match + 5;
				uint8_t destReg = match[6];
				if (destReg == 0x4B) // rbx, 3.1.2
				{
					DlssPresetOverrideFunc_EarlyVersion_Hook = safetyhook::create_mid(midhookAddress, DlssPresetOverrideFunc_Version3_1_2);
					spdlog::info("nvngx_dlss: applied DLSS preset override hook (v3.1.2)");
				}
				else if (destReg == 0x4F) // rdi, 3.1.1
				{
					DlssPresetOverrideFunc_EarlyVersion_Hook = safetyhook::create_mid(midhookAddress, DlssPresetOverrideFunc_Version3_1_1);
					spdlog::info("nvngx_dlss: applied DLSS preset override hook (v3.1.1)");
				}
				else
				{
					spdlog::warn("nvngx_dlss: failed to find DLSS preset override code, recommend enabling OverrideAppId instead");
				}
			}
		}
	}

	// Hook to override the preset DLSS picks based on ratio, so we can check against users customized ratios/resolutions instead
	bool presetSelectPatternSuccess = false;
	auto presetSelectPattern = hook::pattern((void*)ngx_module, "1C C7 ? F8 01 00 00 03 00 00 00");
	if (presetSelectPattern.size())
	{
		uint8_t* match = presetSelectPattern.count(1).get_first<uint8_t>(1);
		CreateDlssInstance_PresetSelection_Register = match[1];

		// TODO: better way of handling CreateDlssInstance_PresetSelection_Register

		if (CreateDlssInstance_PresetSelection_Register != 0x83 && CreateDlssInstance_PresetSelection_Register != 0x86)
		{
			spdlog::error("nvngx_dlss: DLSS preset selection hook failed (unknown register 0x{:X})", CreateDlssInstance_PresetSelection_Register);
		}
		else
		{
			CreateDlssInstance_PresetSelection_Hook = safetyhook::create_mid(match, CreateDlssInstance_PresetSelection);

			spdlog::info("nvngx_dlss: applied DLSS resolution-to-preset selection hook");

			presetSelectPatternSuccess = true;
		}
	}
	
	if (!presetSelectPatternSuccess)
	{
		spdlog::warn("nvngx_dlss: failed to hook DLSS3.1.11+ preset selection code, presets may act strange when used with customized DLSSQualityLevels - recommend updating to DLSS 3.1.11+!");
	}

	// OverrideDlssHud hooks
	// This pattern finds 2 matches in latest DLSS, but only 1 in older ones
	// The one we're trying to find seems to always be first match
	// TODO: scan for RegQueryValue call and grab the offset from that, use offset instead of wildcards below
	auto indicatorValueCheck = hook::pattern((void*)ngx_module, "8B 81 ? ? ? ? 89 02 33 C0 C3");
	if ((!settings.disableIniMonitoring || settings.overrideDlssHud == 2) && indicatorValueCheck.size())
	{
		if (!settings.disableIniMonitoring)
			spdlog::debug("nvngx_dlss: applying hud hook via vftable hook...");
		else if (settings.overrideDlssHud == 2)
			spdlog::debug("nvngx_dlss: OverrideDlssHud == 2, applying hud hook via vftable hook...");

		auto indicatorValueCheck_addr = indicatorValueCheck.get(0).get<void>();
		DLSS_GetIndicatorValue_Hook = safetyhook::create_inline(indicatorValueCheck_addr, DLSS_GetIndicatorValue);

		// Unfortunately it's not enough to just hook the function, HUD render code seems to have an optimization where it checks funcptr and inlines code if it matches
		// So we also need to search for the address of the function, find vftable that holds it, and overwrite entry to point at our hook

		// Ugly way of converting our address to a pattern string...
		uint8_t* p = (uint8_t*)&indicatorValueCheck_addr;

		std::stringstream ss;
		ss << std::hex << std::setw(2) << std::setfill('0') << (int)p[0];
		for (int i = 1; i < 8; ++i)
			ss << " " << std::setw(2) << std::setfill('0') << (int)p[i];

		auto pattern = ss.str();

		auto indicatorValueCheckMemberVf = hook::pattern((void*)ngx_module, pattern);
		for (int i = 0; i < indicatorValueCheckMemberVf.size(); i++)
		{
			auto vfAddr = indicatorValueCheckMemberVf.get(i).get<uintptr_t>(0);
			UnprotectMemory unprotect{ (uintptr_t)vfAddr, sizeof(uintptr_t) };
			*vfAddr = (uintptr_t)&DLSS_GetIndicatorValue;
		}

		spdlog::info("nvngx_dlss: applied debug hud overlay hook via vftable hook");
	}
	else
	{
		// Failed to locate indicator value check inside DLSS
		// Fallback to RegQueryValueExW hook so we can override the ShowDlssIndicator value
		HMODULE advapi = GetModuleHandleA("advapi32.dll");
		RegQueryValueExW_Orig = advapi ? (decltype(RegQueryValueExW_Orig))GetProcAddress(advapi, "RegQueryValueExW") : nullptr;
		if (!RegQueryValueExW_Orig || !utility::HookIAT(ngx_module, "advapi32.dll", RegQueryValueExW_Orig, RegQueryValueExW_Hook))
			spdlog::warn("nvngx_dlss: failed to hook DLSS HUD functions, OverrideDlssHud might not be available.");
		else
			spdlog::info("nvngx_dlss: applied debug hud overlay hook via registry");
	}

	return true;
}

void unhook(HMODULE ngx_module)
{
	dlssIndicatorHudHook.reset();

	spdlog::debug("nvngx_dlss: finished unhook");
}

SafetyHookInline dllmain;
BOOL APIENTRY hooked_dllmain(HMODULE hModule, int ul_reason_for_call, LPVOID lpReserved)
{
	BOOL res = dllmain.stdcall<BOOL>(hModule, ul_reason_for_call, lpReserved);

	if (ul_reason_for_call == DLL_PROCESS_DETACH)
	{
		unhook(hModule);
		dllmain.reset();
	}

	return res;
}

// Installs DllMain hook onto nvngx_dlss
void init(HMODULE ngx_module)
{
	if (settings.disableAllTweaks)
		return;

	hook(ngx_module);
	dllmain = safetyhook::create_inline(utility::ModuleEntryPoint(ngx_module), hooked_dllmain);
}
};

namespace nvngx_dlssg
{
std::mutex module_handle_mtx;
HMODULE module_handle = NULL;

// Currently only patches out watermark text on select builds
void settings_changed()
{
	std::scoped_lock lock{ module_handle_mtx };

	if (!module_handle)
		return;

	char patch = settings.disableDevWatermark ? 0 : 0x4E;

	// Search for DLSSG watermark text and null it if found
	auto pattern = hook::pattern((void*)module_handle,
		"56 49 44 49 41 20 43 4F 4E 46 49 44 45 4E 54 49 41 4C 20 2D 20 50 52 4F 56 49 44 45 44 20 55 4E 44 45 52 20 4E 44 41");

	int count = pattern.size();
	if (!count)
	{
		spdlog::warn("nvngx_dlssg: DisableDevWatermark failed, couldn't locate watermark string inside module");
		return;
	}

	int changed = 0;
	for (int i = 0; i < count; i++)
	{
		char* result = pattern.get(i).get<char>(-1);

		if (result[0] != patch)
		{
			UnprotectMemory unprotect{ (uintptr_t)result, 1 };
			*result = patch;
			changed++;
		}
	}

	if (changed)
		spdlog::info("nvngx_dlssg: DisableDevWatermark patch {} ({}/{} strings patched)", settings.disableDevWatermark ? "applied" : "removed", changed, count);
}
	
SafetyHookInline dllmain;
BOOL APIENTRY hooked_dllmain(HMODULE hModule, int ul_reason_for_call, LPVOID lpReserved)
{
	if (ul_reason_for_call == DLL_PROCESS_ATTACH)
	{
		std::scoped_lock lock{module_handle_mtx};
		module_handle = hModule;
	}

	BOOL res = dllmain.stdcall<BOOL>(hModule, ul_reason_for_call, lpReserved);

	if (ul_reason_for_call == DLL_PROCESS_DETACH)
	{
		std::scoped_lock lock{module_handle_mtx};
		module_handle = NULL;
		dllmain.reset();
	}

	return res;
}

void init(HMODULE ngx_module)
{
	if (settings.disableAllTweaks)
		return;

	{
		std::scoped_lock lock{ module_handle_mtx };
		module_handle = ngx_module;
	}
	settings_changed();

	dllmain = safetyhook::create_inline(utility::ModuleEntryPoint(ngx_module), hooked_dllmain);
}
};
