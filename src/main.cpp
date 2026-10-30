// Fixes a crash in Engine Fixes' allocator (EngineFixes.dll, tbbmalloc's large-object cache: movdqa to a stack slot
// that is 8 off 16-byte alignment). Engine Fixes' "BSLightingShaderPropertyShadowMap" fix jumps from the middle of
// BSLightingShaderProperty::GetRenderPasses_ShadowMapOrMask into a generated stub that calls its detour after
// `push rbx; sub rsp, 0x20` - the push leaves the stack 8 off the alignment Win64 code expects at a call, on every
// call. Most code does not care; tbbmalloc's cache cleanup, reached from the detour's allocation, stores SSE registers
// to the stack with aligned moves and faults. Upstream report: aers/EngineFixesSkyrim64 issue #102.
// The plugin finds Engine Fixes' stub through the jump at the hook site, checks it byte for byte, and points the jump
// at its own stub: the same register setup and the same continuation, with the stack aligned before the call. The
// detour is reached through a wrapper that counts calls entered with a misaligned stack (expected: none). Any other
// bytes there (Engine Fixes absent, the fix off, an Engine Fixes release with its own stub changed) leave it inactive.

#include <intrin.h>

namespace
{
	using Detour_t = void*(void*, void*);

	Detour_t*                  detour = nullptr;
	std::atomic<std::uint64_t> calls{ 0 };
	std::atomic<std::uint64_t> misaligned{ 0 };

	bool Milestone(std::uint64_t a_n)  // 1, 10, 100, ...
	{
		while (a_n % 10 == 0) {
			a_n /= 10;
		}
		return a_n == 1;
	}

	void* Wrapper(void* a_property, void* a_geometry)
	{
		// at a function's entry the return address sits 8 off a 16-byte boundary when the caller kept the alignment
		const bool bad = (reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress()) & 0xF) != 8;
		const auto bads = bad ? misaligned.fetch_add(1, std::memory_order_relaxed) + 1 : misaligned.load(std::memory_order_relaxed);
		// ponytail: a log line at the 1st, 10th, 100th ... call - the detour runs per shadow-casting shape per frame
		if (const auto n = calls.fetch_add(1, std::memory_order_relaxed) + 1; Milestone(n)) {
			logger::info("ShadowMap detour: {} calls, {} entered with a misaligned stack"sv, n, bads);
		}
		return detour(a_property, a_geometry);
	}

	// Engine Fixes 7.0.19 / 7.0.20 (src/fixes/bslightingshaderproperty_shadowmap.h, Patch): ? = an address
	constexpr std::array<std::int16_t, 51> kEFStub{
		0x48, 0x89, 0xF1,                                // mov rcx, rsi
		0x48, 0x8B, 0x94, 0x24, 0xF8, 0x00, 0x00, 0x00,  // mov rdx, [rsp+0xF8]
		0x49, 0xB8, -1, -1, -1, -1, -1, -1, -1, -1,      // mov r8, detour
		0x53,                                            // push rbx
		0x48, 0x83, 0xEC, 0x20,                          // sub rsp, 0x20
		0x41, 0xFF, 0xD0,                                // call r8
		0x48, 0x83, 0xC4, 0x20,                          // add rsp, 0x20
		0x5B,                                            // pop rbx
		0x49, 0x89, 0xC7,                                // mov r15, rax
		0xFF, 0x25, 0x00, 0x00, 0x00, 0x00,              // jmp [rip]
		-1, -1, -1, -1, -1, -1, -1, -1                   // continuation
	};
	constexpr std::size_t kDetourAt = 13;
	constexpr std::size_t kContinueAt = 43;

	bool Matches(const std::uint8_t* a_code)
	{
		for (std::size_t i = 0; i < kEFStub.size(); ++i) {
			if (kEFStub[i] >= 0 && a_code[i] != kEFStub[i]) {
				return false;
			}
		}
		return true;
	}

	// the jump target, through CommonLib's 14-byte `jmp [rip]` thunk when write_jmp<5> needed one
	const std::uint8_t* Follow(std::uintptr_t a_site)
	{
		auto* p = reinterpret_cast<const std::uint8_t*>(a_site);
		if (p[0] != 0xE9) {
			return nullptr;
		}
		p += 5 + *reinterpret_cast<const std::int32_t*>(p + 1);
		if (p[0] == 0xFF && p[1] == 0x25 && *reinterpret_cast<const std::int32_t*>(p + 2) == 0) {
			p = *reinterpret_cast<const std::uint8_t* const*>(p + 6);
		}
		return p;
	}

	void Append(std::vector<std::uint8_t>& a_code, std::uintptr_t a_value)
	{
		const auto* p = reinterpret_cast<const std::uint8_t*>(&a_value);
		a_code.insert(a_code.end(), p, p + sizeof(a_value));
	}

	std::vector<std::uint8_t> Stub(std::uintptr_t a_continue)
	{
		std::vector<std::uint8_t> s{
			0x48, 0x89, 0xF1,                                // mov rcx, rsi
			0x48, 0x8B, 0x94, 0x24, 0xF8, 0x00, 0x00, 0x00,  // mov rdx, [rsp+0xF8] (before any push)
			0x53,                                            // push rbx
			0x55,                                            // push rbp
			0x48, 0x89, 0xE5,                                // mov rbp, rsp
#ifdef REPRODUCE_MISALIGNMENT
			0x48, 0x83, 0xEC, 0x28,  // sub rsp, 0x28: Engine Fixes' 8-byte offset kept (probe build only)
#else
			0x48, 0x83, 0xE4, 0xF0,  // and rsp, -16
			0x48, 0x83, 0xEC, 0x20,  // sub rsp, 0x20 (shadow space)
#endif
			0x48, 0xB8  // mov rax, Wrapper
		};
		Append(s, reinterpret_cast<std::uintptr_t>(&Wrapper));
		s.insert(s.end(), {
			0xFF, 0xD0,                         // call rax
			0x48, 0x89, 0xEC,                   // mov rsp, rbp
			0x5D,                               // pop rbp
			0x5B,                               // pop rbx
			0x49, 0x89, 0xC7,                   // mov r15, rax
			0xFF, 0x25, 0x00, 0x00, 0x00, 0x00  // jmp [rip]
		});
		Append(s, a_continue);
		return s;
	}
}

SKSEPluginInfo(
	.Version = Plugin::VERSION,
	.Name = Plugin::NAME,
	.Author = "Yuof"sv,
	.RuntimeCompatibility = SKSE::VersionIndependence::AddressLibrary)

SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse, { .log = true });
	logger::info("{} v{} on {}"sv, Plugin::NAME, Plugin::VERSION.string(), REL::Module::get().version().string());
#ifdef REPRODUCE_MISALIGNMENT
	logger::warn("probe build: the stub keeps Engine Fixes' misalignment on purpose"sv);
#endif
	// Engine Fixes installs its fixes in its preload, before any plugin's load: the jump is already there
	const REL::Relocation<std::uintptr_t> function{ RELOCATION_ID(99872, 106517) };
	const auto                            site = function.address() + REL::VariantOffset(0x291, 0x295, 0).offset();
	const auto                            continuation = site + REL::VariantOffset(0x7EB, 0x802, 0).offset();  // EF: a_target (= the site) + VAR_NUM(0x7EB, 0x802)
	const auto*                           ef = Follow(site);
	if (!ef) {
		logger::info("no jump at the ShadowMap hook site (Engine Fixes' BSLightingShaderPropertyShadowMap fix not installed): inactive"sv);
		return true;
	}
	if (!Matches(ef) || *reinterpret_cast<const std::uintptr_t*>(ef + kContinueAt) != continuation) {
		std::string bytes;
		for (std::size_t i = 0; i < kEFStub.size(); ++i) {
			bytes += std::format("{:02X} ", ef[i]);
		}
		logger::info("self-check failed (the code behind the ShadowMap hook is not Engine Fixes 7.0.19 / 7.0.20's stub - fixed upstream, or another hook): inactive"sv);
		logger::info("found there: {}"sv, bytes);
		return true;
	}
	detour = *reinterpret_cast<Detour_t* const*>(ef + kDetourAt);
	const auto stub = Stub(continuation);
	SKSE::AllocTrampoline(stub.size() + 14);
	auto& trampoline = SKSE::GetTrampoline();
	auto* mem = trampoline.allocate(stub.size());
	std::memcpy(mem, stub.data(), stub.size());
	trampoline.write_branch<5>(site, reinterpret_cast<std::uintptr_t>(mem));
	logger::info("self-check passed, Engine Fixes' ShadowMap detour at {:X} now called with an aligned stack"sv,
		reinterpret_cast<std::uintptr_t>(detour));
	return true;
}
