#include "emulator.h"

#include "common/abi.h"
#include "common/assert.h"
#include "common/commonSubsystem.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/singleton.h"
#include "common/stringUtils.h"
#include "common/subsystems.h"
#include "common/systemInfo.h"
#include "common/threads.h"
#include "common/waitWatch.h"
#include "graphics/guest_gpu/graphicsRun.h"
#include "graphics/host_gpu/objects/label.h"
#include "graphics/presentation/window.h"
#include "kernel/fileSystem.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "libs/agc.h"
#include "libs/audio.h"
#include "libs/controller.h"
#include "libs/libs.h"
#include "libs/network.h"
#include "loader/runtimeLinker.h"
#include "loader/systemContent.h"
#include "loader/timer.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include <cstdlib>
#include <filesystem>

namespace Emulator {

static void PrintSystemInfo() {
	Common::SystemInfo info = Common::GetSystemInfo();

	LOGF("ProcessorName = %s\n", info.ProcessorName.c_str());
}

static void KytyClose() {
	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();

	rt->Clear();

	LOGF("done!\n");

	Common::SubsystemsListSingleton::Instance()->ShutdownAll();
}

static void MountOrCreateDir(const std::filesystem::path& dir, const std::string& point) {
	if (!Common::File::IsDirectoryExisting(dir)) {
		Common::File::CreateDirectories(dir);
	}

	EXIT_NOT_IMPLEMENTED(!Common::File::IsDirectoryExisting(dir));

	Libs::LibKernel::FileSystem::Mount(dir, point);
	auto dir_text = Common::PathToString(dir);
	LOGF("Mounted %s -> %s\n", point.c_str(), dir_text.c_str());
}

static void MountSandboxDirs() {
	std::string title_id;
	if (!Loader::SystemContentParamSfoGetString("TITLE_ID", &title_id) || title_id.empty()) {
		title_id = "UNKNOWN";
	}

	MountOrCreateDir("_DownloadData/" + title_id, "/download0");
	MountOrCreateDir("_TempData/" + title_id, "/temp0");
	MountOrCreateDir("_TempData/" + title_id, "/temp");
}

static bool ClearDirectoryContents(const std::filesystem::path& dir) {
	bool ok = true;

	for (const auto& entry: Common::File::GetDirEntries(dir)) {
		if (entry.name == "." || entry.name == "..") {
			continue;
		}

		auto path = dir / entry.name;

		if (entry.is_file) {
			Common::File::RemoveReadonly(path);
			ok = Common::File::DeleteFile(path) && ok;
		} else {
			ok = ClearDirectoryContents(path) && ok;
			ok = Common::File::DeleteDirectory(path) && ok;
		}
	}

	return ok;
}

static void ClearDebugTextureFolder() {
	const std::string debug_texture_folder = "_Textures";

	if (!Common::File::IsDirectoryExisting(debug_texture_folder)) {
		Common::File::CreateDirectories(debug_texture_folder);
		return;
	}

	if (!ClearDirectoryContents(debug_texture_folder)) {
		LOGF_COLOR(Log::Color::BrightYellow, "TextureDump: failed to completely clear %s\n",
		           debug_texture_folder.c_str());
	}
}

static void Init(const Config::ConfigOptions& cfg) {
	EXIT_IF(!Common::Thread::IsMainThread());

	auto* slist = Common::SubsystemsList::Instance();

	auto* audio       = Libs::Audio::AudioSubsystem::Instance();
	auto* config      = Config::ConfigSubsystem::Instance();
	auto* controller  = Libs::Controller::ControllerSubsystem::Instance();
	auto* core        = Common::CommonSubsystem::Instance();
	auto* file_system = Libs::LibKernel::FileSystem::FileSystemSubsystem::Instance();
	auto* graphics    = Libs::Graphics::GraphicsSubsystem::Instance();
	auto* log         = Log::LogSubsystem::Instance();
	auto* memory      = Libs::LibKernel::Memory::MemorySubsystem::Instance();
	auto* network     = Libs::Network::NetworkSubsystem::Instance();
	auto* profiler    = Profiler::ProfilerSubsystem::Instance();
	auto* pthread     = Libs::LibKernel::PthreadSubsystem::Instance();
	auto* timer       = Loader::Timer::TimerSubsystem::Instance();

	slist->Add(config, {core});
	slist->InitAll(true);

	Config::Load(cfg);

	slist->Add(audio, {core, log, pthread, memory});
	slist->Add(controller, {core, log, config});
	slist->Add(file_system, {core, log, pthread});
	slist->Add(graphics, {core, log, pthread, memory, config, profiler, controller});
	slist->Add(log, {core, config});
	slist->Add(memory, {core, log});
	slist->Add(network, {core, log, pthread});
	slist->Add(profiler, {core, config});
	slist->Add(pthread, {core, log, timer});
	slist->Add(timer, {core, log});

	slist->InitAll(true);
}

static void LoadElf(const std::filesystem::path& elf, bool dbg_print_reloc = false,
                    const std::filesystem::path& save_name = {}) {
	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();

	auto* program = rt->LoadProgram(
	    Libs::LibKernel::FileSystem::GetRealFilename(Common::PathToGenericString(elf)));

	if (dbg_print_reloc) {
		program->dbg_print_reloc = true;
	}

	if (!save_name.empty()) {
		rt->SaveProgram(program, Libs::LibKernel::FileSystem::GetRealFilename(
		                             Common::PathToGenericString(save_name)));
	}
}

// Diagnostic watchdog: when the presented-frame counter stops advancing, dump every guest thread's
// current blocking primitive so we can distinguish a deadlock (threads parked on a wait) from a
// thrash (threads still churning) from a GPU stall (labels pending). Prints on stdout.
static void WatchdogRun(void* /*unused*/) {
	int      last_frame     = -1;
	uint64_t last_change_ms = Kyty::WaitWatch::NowMs();
	bool     dumped         = false;
	int      save_tick      = 0;
	namespace DS            = Kyty::WaitWatch::DrainStats;
	uint64_t p_pause = 0, p_pause_ns = 0, p_wait_ns = 0, p_bw = 0, p_bw_ns = 0, p_ld_ns = 0,
	         p_incde = 0, p_fault = 0, p_cpf = 0, p_sub = 0, p_rb = 0, p_ftot = 0, p_funi = 0,
	         p_draws = 0, p_disp = 0, p_ftx = 0;
	for (;;) {
		// KYTY_DIAG: sample the GPU worker's live scope at ~5ms granularity across the 1s window and
		// tally it, so the DRAIN line shows WHERE the worker's wall-clock actually goes (park latency
		// is dominated by whatever the worker is doing between pause requests).
		std::map<const char*, int> worker_hist;
		int                        worker_samples = 0;
		for (int s = 0; s < 200; s++) {
			Common::Thread::Sleep(5);
			const char* k = Kyty::WaitWatch::ScopeKindByName("GpuWorker");
			worker_hist[k != nullptr ? k : "?"]++;
			worker_samples++;
		}
		std::string worker_top;
		{
			std::vector<std::pair<const char*, int>> v(worker_hist.begin(), worker_hist.end());
			std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
			for (size_t i = 0; i < v.size() && i < 5; i++) {
				char buf[64];
				std::snprintf(buf, sizeof(buf), "%s=%d ", v[i].first != nullptr ? v[i].first : "?",
				              v[i].second);
				worker_top += buf;
			}
		}
		{
			const uint64_t pause    = DS::pause_count.load(std::memory_order_relaxed);
			const uint64_t pause_ns = DS::pause_ns.load(std::memory_order_relaxed);
			const uint64_t wait_ns  = DS::waitidle_ns.load(std::memory_order_relaxed);
			const uint64_t bw       = DS::bufwait_count.load(std::memory_order_relaxed);
			const uint64_t bw_ns    = DS::bufwait_ns.load(std::memory_order_relaxed);
			const uint64_t ld_ns    = DS::labeldrain_ns.load(std::memory_order_relaxed);
			const uint64_t incde    = DS::incde_count.load(std::memory_order_relaxed);
			const uint64_t fault    = DS::fault_count.load(std::memory_order_relaxed);
			const uint64_t cpf      = DS::cp_fault_count.load(std::memory_order_relaxed);
			const uint64_t sub      = DS::submits_done.load(std::memory_order_relaxed);
			const uint64_t rb       = DS::readback_count.load(std::memory_order_relaxed);
			const uint64_t ftot     = Kyty::WaitWatch::FaultStats::total.load(std::memory_order_relaxed);
			const uint64_t funi     = Kyty::WaitWatch::FaultStats::unique.load(std::memory_order_relaxed);
			std::printf("=== DRAIN/s frame=%d faults=%llu cpFaults=%llu readbacks=%llu submits=%llu "
			            "fTot=%llu fNew=%llu "
			            "pause=%llu(%.1fms) waitidle=%.1fms bufwait=%llu(%.1fms) labeldrain=%.1fms "
			            "incDe=%llu ===\n",
			            Libs::Graphics::WindowGetPresentedFrame(),
			            static_cast<unsigned long long>(fault - p_fault),
			            static_cast<unsigned long long>(cpf - p_cpf),
			            static_cast<unsigned long long>(rb - p_rb),
			            static_cast<unsigned long long>(sub - p_sub),
			            static_cast<unsigned long long>(ftot - p_ftot),
			            static_cast<unsigned long long>(funi - p_funi),
			            static_cast<unsigned long long>(pause - p_pause),
			            static_cast<double>(pause_ns - p_pause_ns) / 1e6,
			            static_cast<double>(wait_ns - p_wait_ns) / 1e6,
			            static_cast<unsigned long long>(bw - p_bw),
			            static_cast<double>(bw_ns - p_bw_ns) / 1e6,
			            static_cast<double>(ld_ns - p_ld_ns) / 1e6,
			            static_cast<unsigned long long>(incde - p_incde));
			const uint64_t draws = DS::draw_count.load(std::memory_order_relaxed);
			const uint64_t disp  = DS::dispatch_count.load(std::memory_order_relaxed);
			const uint64_t ftx   = DS::find_texture_calls.load(std::memory_order_relaxed);
			const uint64_t nimg  = DS::texture_image_count.load(std::memory_order_relaxed);
			std::printf("=== WORKER/s samples=%d draws=%llu dispatches=%llu findTex=%llu images=%llu | "
			            "%s===\n",
			            worker_samples, static_cast<unsigned long long>(draws - p_draws),
			            static_cast<unsigned long long>(disp - p_disp),
			            static_cast<unsigned long long>(ftx - p_ftx),
			            static_cast<unsigned long long>(nimg), worker_top.c_str());
			std::fflush(stdout);
			p_draws = draws, p_disp = disp, p_ftx = ftx;
			p_pause = pause, p_pause_ns = pause_ns, p_wait_ns = wait_ns, p_bw = bw, p_bw_ns = bw_ns,
			p_ld_ns = ld_ns, p_incde = incde, p_fault = fault, p_cpf = cpf, p_sub = sub, p_rb = rb,
			p_ftot = ftot, p_funi = funi;
		}
		// Persist the driver pipeline cache periodically so it survives even a hard kill.
		if (++save_tick >= 15) {
			save_tick = 0;
			Libs::Graphics::SavePipelineCache();
		}
		const int      frame = Libs::Graphics::WindowGetPresentedFrame();
		const uint64_t now   = Kyty::WaitWatch::NowMs();
		if (frame != last_frame) {
			last_frame     = frame;
			last_change_ms = now;
			dumped         = false;
			continue;
		}
		const uint64_t stalled = now - last_change_ms;
		if (stalled >= 5000 && !dumped) {
			const uint64_t f_total  = Kyty::WaitWatch::FaultStats::total.load(std::memory_order_relaxed);
			const uint64_t f_unique = Kyty::WaitWatch::FaultStats::unique.load(std::memory_order_relaxed);
			std::printf("=== WATCHDOG: presented frame %d stalled %llums, gpu_labels_pending=%llu ===\n",
			            frame, static_cast<unsigned long long>(stalled),
			            static_cast<unsigned long long>(Libs::Graphics::LabelPendingCount()));
			std::printf("=== WATCHDOG faults: total=%llu unique_pages=%llu refault_ratio=%.2f ===\n",
			            static_cast<unsigned long long>(f_total),
			            static_cast<unsigned long long>(f_unique),
			            f_unique != 0 ? static_cast<double>(f_total) / static_cast<double>(f_unique)
			                          : 0.0);
			Kyty::WaitWatch::Dump("stall#1");
			Libs::Graphics::GraphicsRunDumpGpuWait();
			Common::Thread::Sleep(2000);
			std::printf("=== WATCHDOG: gpu_labels_pending=%llu (2s later) ===\n",
			            static_cast<unsigned long long>(Libs::Graphics::LabelPendingCount()));
			Kyty::WaitWatch::Dump("stall#2");
			Libs::Graphics::GraphicsRunDumpGpuWait();
			dumped = true;
		}
	}
}

static void Execute() {
	Common::Thread watchdog(WatchdogRun, nullptr);
	watchdog.Detach();

	int thread_model = 1;

	if (thread_model == 0) {
		Common::Thread t([](void* /*unused*/) { Libs::Graphics::WindowRun(); }, nullptr);
		t.Detach();
		auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
		rt->Execute();
	} else {
		Common::Thread t(
		    [](void* /*unused*/) {
			    auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
			    rt->Execute();
		    },
		    nullptr);
		t.Detach();
		Libs::Graphics::WindowRun();
		t.Join();
	}
}

void Run(const RunOptions& options) {
	if (options.app0_dir.empty()) {
		EXIT("app0 directory is required\n");
	}

	if (options.elf.empty()) {
		EXIT("ELF is required\n");
	}

	Init(options.config);

	ClearDebugTextureFolder();

	PrintSystemInfo();

	int ok = atexit(KytyClose);
	EXIT_NOT_IMPLEMENTED(ok != 0);

	Libs::LibKernel::FileSystem::Mount(options.app0_dir, "/app0");
	Libs::LibKernel::FileSystem::Mount(options.app0_dir, "/hostapp");

	auto param_json = options.app0_dir / "sce_sys" / "param.json";
	if (Common::File::IsFileExisting(param_json)) {
		Loader::SystemContentLoadParamSfo(param_json);
		if (auto flexible_memory_size = Loader::SystemContentGetFlexibleMemorySize();
		    flexible_memory_size != 0) {
			Libs::LibKernel::Memory::SetFlexibleMemorySize(flexible_memory_size);
		}
	}

	MountSandboxDirs();

	auto* rt = Common::Singleton<Loader::RuntimeLinker>::Instance();
	Libs::InitAll(rt->Symbols());

	LoadElf(options.elf);

	Execute();
}

} // namespace Emulator
