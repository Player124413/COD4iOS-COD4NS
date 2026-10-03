// Host tests for the Android performance subsystem.
//
// Everything in ports/android/perf is deliberately free of Android APIs so the
// decisions it makes can be checked here, on a desktop, against synthetic
// workloads. These are unit tests of the control logic, not evidence that any
// particular phone holds 60 fps.
//
// Build and run with ports/android/scripts/host_tests.sh, or through CTest
// with -DKISAK_BUILD_PORT_TESTS=ON.

#include "../perf/cpu_topology.h"
#include "../perf/device_profile.h"
#include "../perf/frame_pacer.h"
#include "../perf/perf_director.h"
#include "../perf/resolution_scaler.h"
#include "../perf/thermal_governor.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>

using namespace kisak::perf;

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const char *what, const char *file, int line)
{
    ++g_checks;
    if (!condition)
    {
        ++g_failures;
        std::printf("FAIL: %s (%s:%d)\n", what, file, line);
    }
}

void CheckNear(double actual, double expected, double tolerance, const char *what, const char *file, int line)
{
    ++g_checks;
    if (!(std::fabs(actual - expected) <= tolerance))
    {
        ++g_failures;
        std::printf("FAIL: %s: expected %.4f +/- %.4f, got %.4f (%s:%d)\n", what, expected, tolerance, actual, file,
                    line);
    }
}

#define CHECK(expr) Check((expr), #expr, __FILE__, __LINE__)
#define CHECK_NEAR(a, b, tol) CheckNear((a), (b), (tol), #a " ~= " #b, __FILE__, __LINE__)

// ---------------------------------------------------------------------------
// Frame pacer

void TestCadenceSelection()
{
    // 60 Hz panel: present every refresh.
    auto c = FramePacer::ChooseCadence(60, 60.0, true);
    CHECK(c.swapInterval == 1);
    CHECK_NEAR(c.presentedFps, 60.0, 0.01);
    CHECK(!c.needsModeChange);

    // 120 Hz: every second refresh is exactly 60, no mode change needed.
    c = FramePacer::ChooseCadence(60, 120.0, true);
    CHECK(c.swapInterval == 2);
    CHECK_NEAR(c.presentedFps, 60.0, 0.01);
    CHECK(!c.needsModeChange);

    // 144 Hz: 144/2 = 72 and 144/3 = 48; neither is 60, so ask for a mode.
    c = FramePacer::ChooseCadence(60, 144.0, true);
    CHECK(c.needsModeChange);
    CHECK_NEAR(c.requestedDisplayHz, 60.0, 0.01);

    // 90 Hz: same problem (90 or 45).
    c = FramePacer::ChooseCadence(60, 90.0, true);
    CHECK(c.needsModeChange);

    // A panel reporting 119.998 is a 120 Hz panel.
    c = FramePacer::ChooseCadence(60, 119.998, true);
    CHECK(c.swapInterval == 2);
    CHECK(!c.needsModeChange);

    // 240 Hz divides by 4.
    c = FramePacer::ChooseCadence(60, 240.0, true);
    CHECK(c.swapInterval == 4);
    CHECK_NEAR(c.presentedFps, 60.0, 0.01);

    // Asking for 120 on a 120 Hz panel must not divide.
    c = FramePacer::ChooseCadence(120, 120.0, true);
    CHECK(c.swapInterval == 1);
    CHECK_NEAR(c.presentedFps, 120.0, 0.01);

    // Divisors disabled: free-run, capped at the panel.
    c = FramePacer::ChooseCadence(60, 120.0, false);
    CHECK(c.swapInterval == 1);
}

// Runs `frames` frames that each cost `costNs`, returning the pacer afterwards.
FramePacer RunFrames(int frames, int64_t costNs, int64_t budgetNs)
{
    FramePacer::Config config;
    config.targetFps = 60;
    config.displayHz = 60.0;
    FramePacer pacer(config);

    int64_t now = 0;
    for (int i = 0; i < frames; ++i)
    {
        pacer.BeginFrame(now);
        pacer.EndCpuWork(now + costNs / 2);
        pacer.ReportGpuTime(costNs / 2);
        // A frame that fits is still presented on the vsync boundary.
        const int64_t end = now + std::max(costNs, budgetNs);
        pacer.EndFrame(end);
        now = end;
    }
    return pacer;
}

void TestPacerStats()
{
    const int64_t budget = FpsToPeriodNs(60);
    FramePacer pacer = RunFrames(120, 8 * kNsPerMs, budget);
    const FramePacer::Stats stats = pacer.stats();
    CHECK(stats.samples == 60); // default history window
    CHECK_NEAR(stats.averageFrameMs, 16.666, 0.2);
    CHECK_NEAR(stats.missRatio, 0.0, 0.001);
    CHECK_NEAR(stats.presentedFps, 60.0, 0.01);

    // Budget comes from the cadence, not the raw target.
    CHECK(pacer.frameBudgetNs() == budget);
}

void TestPacerDetectsOverrun()
{
    FramePacer::Config config;
    config.targetFps = 60;
    config.displayHz = 60.0;
    FramePacer pacer(config);

    int64_t now = 0;
    for (int i = 0; i < 90; ++i)
    {
        pacer.BeginFrame(now);
        // 25 ms of work against a 16.67 ms budget.
        const int64_t cost = 25 * kNsPerMs;
        pacer.EndCpuWork(now + 20 * kNsPerMs);
        pacer.ReportGpuTime(5 * kNsPerMs);
        pacer.EndFrame(now + cost);
        now += cost;
    }
    const FramePacer::Stats stats = pacer.stats();
    CHECK_NEAR(stats.missRatio, 1.0, 0.001);
    CHECK(stats.bottleneck == Bottleneck::Cpu);
}

void TestPacerBottleneckClassification()
{
    FramePacer pacer;
    int64_t now = 0;
    for (int i = 0; i < 80; ++i)
    {
        pacer.BeginFrame(now);
        pacer.EndCpuWork(now + 4 * kNsPerMs);  // cheap CPU
        pacer.ReportGpuTime(14 * kNsPerMs);    // expensive GPU
        pacer.EndFrame(now + 16 * kNsPerMs);
        now += 16 * kNsPerMs;
    }
    CHECK(pacer.stats().bottleneck == Bottleneck::Gpu);
}

void TestSleepBudgetHoldsBackMargin()
{
    FramePacer::Config config;
    config.targetFps = 60;
    config.displayHz = 60.0;
    config.safetyMarginFraction = 0.12;
    FramePacer pacer(config);

    // Teach it that frames cost 8 ms.
    int64_t now = 0;
    for (int i = 0; i < 70; ++i)
    {
        pacer.BeginFrame(now);
        pacer.EndCpuWork(now + 8 * kNsPerMs);
        pacer.EndFrame(now + 8 * kNsPerMs);
        now += FpsToPeriodNs(60);
    }

    const int64_t deadline = pacer.BeginFrame(now);
    CHECK(deadline == now + FpsToPeriodNs(60));
    // Budget 16.67, predicted cost 8, margin 2 => start after ~6.67 ms.
    const int64_t sleep = pacer.SleepBudgetNs(now);
    CHECK_NEAR(NsToMs(sleep), 6.67, 0.6);
    // Never negative, even when the caller is already late.
    CHECK(pacer.SleepBudgetNs(now + 100 * kNsPerMs) == 0);

    // Short waits spin, long waits sleep.
    CHECK(pacer.ShouldSpin(500000));
    CHECK(!pacer.ShouldSpin(5 * kNsPerMs));
    CHECK(!pacer.ShouldSpin(0));
}

void TestPacerNeverReservesMoreThanBudget()
{
    // A pathological 100 ms frame must not make the pacer reserve 100 ms and
    // stop sleeping forever.
    FramePacer pacer = RunFrames(80, 100 * kNsPerMs, FpsToPeriodNs(60));
    CHECK(pacer.PredictedFrameCostNs() <= pacer.frameBudgetNs());
}

// ---------------------------------------------------------------------------
// Resolution scaler

void TestScalerReducesWhenGpuBound()
{
    ResolutionScaler::Config config;
    config.dwellFrames = 2;
    ResolutionScaler scaler(config);
    scaler.SetNativeResolution(2400, 1080);
    CHECK(scaler.width() == 2400);

    const int64_t budget = FpsToPeriodNs(60);
    FrameSample sample;
    sample.gpuNs = 30 * kNsPerMs; // ~180% of budget
    sample.frameNs = 30 * kNsPerMs;
    sample.missedDeadline = true;

    bool reduced = false;
    for (int i = 0; i < 40; ++i)
    {
        const auto decision = scaler.Update(sample, Bottleneck::Gpu, budget);
        reduced = reduced || decision.changed;
    }
    CHECK(reduced);
    CHECK(scaler.scale() < 0.99f);
    // Dimensions stay aligned so the blit and tile bins stay cheap.
    CHECK(scaler.width() % 8 == 0);
    CHECK(scaler.height() % 8 == 0);
    // And it never falls through the floor.
    CHECK(scaler.scale() >= config.minScale - 1e-5f);
}

void TestScalerIgnoresCpuBoundFrames()
{
    ResolutionScaler::Config config;
    config.dwellFrames = 1;
    ResolutionScaler scaler(config);
    scaler.SetNativeResolution(1920, 1080);

    FrameSample sample;
    sample.gpuNs = 4 * kNsPerMs;
    sample.frameNs = 30 * kNsPerMs;
    sample.missedDeadline = true;

    for (int i = 0; i < 60; ++i)
        scaler.Update(sample, Bottleneck::Cpu, FpsToPeriodNs(60));

    // The GPU had plenty of headroom: dropping pixels would cost sharpness
    // and fix nothing.
    CHECK_NEAR(scaler.scale(), 1.0, 1e-4);
}

void TestScalerRecoversSlowly()
{
    ResolutionScaler::Config config;
    config.dwellFrames = 2;
    config.raiseConfirmFrames = 30;
    ResolutionScaler scaler(config);
    scaler.SetNativeResolution(1920, 1080);

    const int64_t budget = FpsToPeriodNs(60);

    FrameSample heavy;
    heavy.gpuNs = 26 * kNsPerMs;
    heavy.frameNs = 26 * kNsPerMs;
    heavy.missedDeadline = true;
    for (int i = 0; i < 30; ++i)
        scaler.Update(heavy, Bottleneck::Gpu, budget);
    const float dropped = scaler.scale();
    CHECK(dropped < 1.0f);

    FrameSample light;
    light.gpuNs = 6 * kNsPerMs;
    light.frameNs = 6 * kNsPerMs;

    // A handful of fast frames must not immediately undo the drop.
    for (int i = 0; i < 10; ++i)
        scaler.Update(light, Bottleneck::Gpu, budget);
    CHECK_NEAR(scaler.scale(), dropped, 1e-4);

    // Sustained headroom does.
    for (int i = 0; i < 400; ++i)
        scaler.Update(light, Bottleneck::Gpu, budget);
    CHECK(scaler.scale() > dropped);
}

void TestScalerDoesNotOscillate()
{
    // Workload parked exactly at the budget: the classic oscillation case.
    ResolutionScaler scaler;
    scaler.SetNativeResolution(1920, 1080);
    const int64_t budget = FpsToPeriodNs(60);

    int changes = 0;
    for (int i = 0; i < 600; ++i)
    {
        FrameSample sample;
        // Cost tracks the pixel count, as it would on a real GPU.
        const double area = static_cast<double>(scaler.scale()) * static_cast<double>(scaler.scale());
        sample.gpuNs = static_cast<int64_t>(17.0 * area * static_cast<double>(kNsPerMs));
        sample.frameNs = sample.gpuNs;
        sample.missedDeadline = sample.frameNs > budget;
        if (scaler.Update(sample, Bottleneck::Gpu, budget).changed)
            ++changes;
    }
    // It should settle, not flip every dwell window. Ten seconds of frames
    // with a handful of adjustments is converged behaviour.
    CHECK(changes < 12);
    CHECK(scaler.scale() < 1.0f);
}

void TestScalerRespectsCeilingAndDisable()
{
    ResolutionScaler scaler;
    scaler.SetNativeResolution(1600, 720);
    scaler.SetScaleCeiling(0.8f);
    CHECK(scaler.scale() <= 0.8f + 1e-4f);

    FrameSample light;
    light.gpuNs = 2 * kNsPerMs;
    light.frameNs = 2 * kNsPerMs;
    for (int i = 0; i < 500; ++i)
        scaler.Update(light, Bottleneck::Gpu, FpsToPeriodNs(60));
    CHECK(scaler.scale() <= 0.8f + 1e-4f);

    scaler.SetEnabled(false);
    FrameSample heavy;
    heavy.gpuNs = 40 * kNsPerMs;
    heavy.frameNs = 40 * kNsPerMs;
    heavy.missedDeadline = true;
    const float before = scaler.scale();
    for (int i = 0; i < 100; ++i)
        scaler.Update(heavy, Bottleneck::Gpu, FpsToPeriodNs(60));
    CHECK_NEAR(scaler.scale(), before, 1e-4);
}

// ---------------------------------------------------------------------------
// Thermal governor

void TestThermalClampsAndHolds()
{
    ThermalGovernor::Config config;
    config.releaseDwellFrames = 10;
    ThermalGovernor governor(config);

    CHECK_NEAR(governor.Update().scaleCeiling, 1.0, 1e-4);

    governor.OnThermalStatus(ThermalStatus::Moderate);
    auto state = governor.Update();
    CHECK(state.throttling);
    CHECK(state.scaleCeiling <= 0.76f);
    CHECK(state.targetFps == 60); // resolution goes before frame rate

    // Still hot: no release even after the dwell expires.
    for (int i = 0; i < 50; ++i)
        state = governor.Update();
    CHECK(state.scaleCeiling <= 0.76f);

    // Cooled down: release, but gradually.
    governor.OnThermalStatus(ThermalStatus::None);
    const float afterCooling = governor.Update().scaleCeiling;
    CHECK(afterCooling <= 0.81f);
    for (int i = 0; i < 500; ++i)
        state = governor.Update();
    CHECK_NEAR(state.scaleCeiling, 1.0, 1e-3);
    CHECK(!state.throttling);
}

void TestThermalSevereDropsFrameRate()
{
    ThermalGovernor governor;
    governor.OnThermalStatus(ThermalStatus::Severe);
    const auto state = governor.Update();
    CHECK(state.targetFps < 60);
    CHECK(state.scaleCeiling < 0.7f);
}

void TestThermalHeadroomIsGradual()
{
    ThermalGovernor governor;
    governor.OnThermalHeadroom(0.5f);
    CHECK_NEAR(governor.Update().scaleCeiling, 1.0, 1e-4);

    governor.OnThermalHeadroom(0.90f);
    const float mild = governor.Update().scaleCeiling;
    CHECK(mild < 1.0f);
    CHECK(mild > 0.7f);

    governor.OnThermalHeadroom(1.0f);
    const float severe = governor.Update().scaleCeiling;
    CHECK(severe < mild);

    // An unavailable API (negative) must not disturb the state.
    const float before = governor.state().scaleCeiling;
    governor.OnThermalHeadroom(-1.0f);
    CHECK_NEAR(governor.Update().scaleCeiling, before, 1e-4);
}

// ---------------------------------------------------------------------------
// CPU topology

SysfsReader FixtureReader(const std::map<std::string, std::string> &files)
{
    return [files](const std::string &path, std::string &out) {
        const auto it = files.find(path);
        if (it == files.end())
            return false;
        out = it->second;
        return true;
    };
}

std::map<std::string, std::string> ClusterFixture(const std::vector<int64_t> &khzPerCore)
{
    std::map<std::string, std::string> files;
    for (std::size_t i = 0; i < khzPerCore.size(); ++i)
    {
        char path[128];
        std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%zu/cpufreq/cpuinfo_max_freq", i);
        files[path] = std::to_string(khzPerCore[i]);
    }
    return files;
}

void TestTopologyThreeClusters()
{
    // 1 + 3 + 4, the standard flagship layout.
    const auto files = ClusterFixture({ 1800000, 1800000, 1800000, 1800000,
                                        2400000, 2400000, 2400000, 3000000 });
    const CpuTopology topology = DiscoverTopology(8, FixtureReader(files));

    CHECK(topology.coreCount() == 8);
    CHECK(topology.little.size() == 4);
    CHECK(topology.big.size() == 3);
    CHECK(topology.prime.size() == 1);
    CHECK(topology.prime[0] == 7);
    CHECK(topology.heterogeneous());

    const auto render = topology.AffinityFor(ThreadRole::Render);
    CHECK(render.size() == 4); // prime + big
    CHECK(std::find(render.begin(), render.end(), 7) != render.end());
    CHECK(std::find(render.begin(), render.end(), 0) == render.end());

    const auto worker = topology.AffinityFor(ThreadRole::Worker);
    // Workers stay off the prime core so they cannot preempt rendering there.
    CHECK(std::find(worker.begin(), worker.end(), 7) == worker.end());
    CHECK(worker.size() == 3);

    const auto audio = topology.AffinityFor(ThreadRole::Audio);
    CHECK(audio.size() == 4);
    CHECK(audio[0] == 0);
}

void TestTopologyTwoClusters()
{
    // 4 + 4, typical mid-range: no prime tier.
    const auto files = ClusterFixture({ 1600000, 1600000, 1600000, 1600000,
                                        2200000, 2200000, 2200000, 2200000 });
    const CpuTopology topology = DiscoverTopology(8, FixtureReader(files));
    CHECK(topology.little.size() == 4);
    CHECK(topology.big.size() == 4);
    CHECK(topology.prime.empty());
    CHECK(topology.AffinityFor(ThreadRole::Render).size() == 4);
    CHECK(topology.AffinityFor(ThreadRole::Worker).size() == 4);
}

void TestTopologyUniformAndUnreadable()
{
    const auto files = ClusterFixture({ 2000000, 2000000, 2000000, 2000000 });
    CpuTopology topology = DiscoverTopology(4, FixtureReader(files));
    CHECK(!topology.heterogeneous() || topology.little.empty());
    // A uniform machine must not exile anything to a "little" cluster that
    // does not exist.
    CHECK(topology.AffinityFor(ThreadRole::Audio).size() == 4);

    // Kernel hides cpufreq entirely: still safe, still every core.
    topology = DiscoverTopology(8, [](const std::string &, std::string &) { return false; });
    CHECK(topology.coreCount() == 8);
    CHECK(topology.AffinityFor(ThreadRole::Render).size() == 8);
}

void TestTopologyPrefersCapacity()
{
    // Two clusters at the same clock but different IPC; only cpu_capacity
    // distinguishes them, and it must win.
    std::map<std::string, std::string> files = ClusterFixture({ 2000000, 2000000, 2000000, 2000000 });
    files["/sys/devices/system/cpu/cpu0/cpu_capacity"] = "350";
    files["/sys/devices/system/cpu/cpu1/cpu_capacity"] = "350";
    files["/sys/devices/system/cpu/cpu2/cpu_capacity"] = "1024";
    files["/sys/devices/system/cpu/cpu3/cpu_capacity"] = "1024";
    const CpuTopology topology = DiscoverTopology(4, FixtureReader(files));
    CHECK(topology.little.size() == 2);
    CHECK(topology.little[0] == 0);
    CHECK(!topology.big.empty() || !topology.prime.empty());
}

// ---------------------------------------------------------------------------
// Device profiles

DeviceDescriptor MakeDevice(const char *renderer, uint32_t ramMB, int cores, int bigCores)
{
    DeviceDescriptor d;
    d.gpuRenderer = renderer;
    d.gpuVendor = "Qualcomm";
    d.totalRamMB = ramMB;
    d.totalCores = cores;
    d.bigCores = bigCores;
    d.maxFrequencyKhz = 2800000;
    d.vulkanApiVersion = 0x00401000;
    d.displayWidth = 2400;
    d.displayHeight = 1080;
    d.displayHz = 120.0;
    return d;
}

void TestGpuClassification()
{
    CHECK(ClassifyGpu("Adreno (TM) 750", "Qualcomm") == QualityTier::Ultra);
    CHECK(ClassifyGpu("Adreno (TM) 660", "Qualcomm") == QualityTier::Ultra);
    CHECK(ClassifyGpu("Adreno (TM) 650", "Qualcomm") == QualityTier::High);
    CHECK(ClassifyGpu("Adreno (TM) 619", "Qualcomm") == QualityTier::Medium);
    CHECK(ClassifyGpu("Adreno (TM) 506", "Qualcomm") == QualityTier::Low);
    CHECK(ClassifyGpu("Adreno (TM) 405", "Qualcomm") == QualityTier::Minimum);

    CHECK(ClassifyGpu("Mali-G715-Immortalis MC11", "ARM") == QualityTier::Ultra);
    CHECK(ClassifyGpu("Mali-G78 MP14", "ARM") == QualityTier::High);
    CHECK(ClassifyGpu("Mali-G57 MC2", "ARM") == QualityTier::Medium);
    CHECK(ClassifyGpu("Mali-T860", "ARM") == QualityTier::Low);
    CHECK(ClassifyGpu("Mali-T720", "ARM") == QualityTier::Minimum);

    CHECK(ClassifyGpu("Samsung Xclipse 920", "Samsung") == QualityTier::Ultra);
    CHECK(ClassifyGpu("PowerVR Rogue GE8320", "Imagination") == QualityTier::Low);

    // Unknown hardware must land somewhere playable rather than refusing.
    CHECK(ClassifyGpu("Some Future GPU", "Nobody") == QualityTier::Medium);
}

void TestDeviceDemotion()
{
    // Flagship GPU, tiny memory: the working set, not the GPU, is the limit.
    DeviceDescriptor d = MakeDevice("Adreno (TM) 750", 3000, 8, 4);
    CHECK(ClassifyDevice(d) == QualityTier::Low);

    // Same GPU with enough memory.
    d = MakeDevice("Adreno (TM) 750", 12000, 8, 4);
    CHECK(ClassifyDevice(d) == QualityTier::Ultra);

    // No big cores: the serial main thread cannot feed the GPU.
    d = MakeDevice("Adreno (TM) 750", 8000, 8, 0);
    CHECK(ClassifyDevice(d) == QualityTier::Low);

    // No Vulkan: GLES fallback costs enough to matter.
    d = MakeDevice("Adreno (TM) 750", 8000, 8, 4);
    d.vulkanApiVersion = 0;
    CHECK(ClassifyDevice(d) == QualityTier::Medium);
}

void TestProfileContents()
{
    const DeviceDescriptor flagship = MakeDevice("Adreno (TM) 750", 12000, 8, 4);
    const QualityProfile ultra = ProfileForDevice(flagship);
    CHECK(ultra.tier == QualityTier::Ultra);
    CHECK(ultra.targetFps == 60);
    CHECK(ultra.shadows == 2);
    CHECK(ultra.specular);

    DeviceDescriptor weak = MakeDevice("Adreno (TM) 405", 2048, 4, 0);
    weak.displayWidth = 1280;
    weak.displayHeight = 720;
    const QualityProfile minimum = ProfileForDevice(weak);
    CHECK(minimum.tier == QualityTier::Minimum);
    CHECK(minimum.shadows == 0);
    CHECK(!minimum.postProcess);
    CHECK(minimum.renderScale < 0.7f);
    // Even the weakest tier still aims at 60; it gets there by shedding work,
    // not by lowering the target.
    CHECK(minimum.targetFps == 60);

    // A 1440p panel is capped so the port does not render 78% more pixels for
    // no visible gain.
    DeviceDescriptor qhd = MakeDevice("Adreno (TM) 750", 12000, 8, 4);
    qhd.displayWidth = 3200;
    qhd.displayHeight = 1440;
    const QualityProfile capped = ProfileForDevice(qhd);
    CHECK(capped.renderScale < 0.9f);

    // The console text must be parseable: one command per line, no blanks.
    const std::string commands = ultra.ToConsoleCommands();
    CHECK(commands.find("seta r_picmip 0\n") != std::string::npos);
    CHECK(commands.find("seta com_maxfps 60\n") != std::string::npos);
    CHECK(commands.find("seta r_androidRenderScale") != std::string::npos);
    CHECK(commands.back() == '\n');
}

// ---------------------------------------------------------------------------
// Director integration

void TestDirectorEndToEnd()
{
    PerfDirector director;
    DeviceDescriptor device = MakeDevice("Adreno (TM) 619", 6000, 8, 4);
    device.displayHz = 120.0;
    QualityProfile profile = ProfileForDevice(device);
    profile.dynamicResolution = true;
    director.Initialise(device, profile);

    // 120 Hz panel, 60 fps target: the pacer must halve, not free-run.
    CHECK(director.pacer().cadence().swapInterval == 2);
    CHECK_NEAR(director.pacer().cadence().presentedFps, 60.0, 0.01);

    const float startScale = director.scaler().scale();

    // Simulate a GPU-bound overload and confirm resolution gives way.
    int64_t now = 0;
    for (int i = 0; i < 300; ++i)
    {
        director.BeginFrame(now);
        director.EndCpuWork(now + 5 * kNsPerMs);
        director.ReportGpuTime(24 * kNsPerMs);
        const auto result = director.EndFrame(now + 26 * kNsPerMs);
        now += 26 * kNsPerMs;
        (void)result;
    }
    CHECK(director.scaler().scale() < startScale);
    CHECK(director.pacer().stats().bottleneck == Bottleneck::Gpu);

    // Thermal clamp must bound the scaler even if frames get cheap again.
    director.OnThermalStatus(ThermalStatus::Moderate);
    for (int i = 0; i < 2000; ++i)
    {
        director.BeginFrame(now);
        director.EndCpuWork(now + 3 * kNsPerMs);
        director.ReportGpuTime(4 * kNsPerMs);
        director.EndFrame(now + FpsToPeriodNs(60));
        now += FpsToPeriodNs(60);
    }
    CHECK(director.scaler().scale() <= director.thermal().state().scaleCeiling + 1e-4f);

    const std::string status = director.StatusLine();
    CHECK(status.find("fps") != std::string::npos);
    CHECK(status.find("scale") != std::string::npos);
}

} // namespace

int main()
{
    TestCadenceSelection();
    TestPacerStats();
    TestPacerDetectsOverrun();
    TestPacerBottleneckClassification();
    TestSleepBudgetHoldsBackMargin();
    TestPacerNeverReservesMoreThanBudget();

    TestScalerReducesWhenGpuBound();
    TestScalerIgnoresCpuBoundFrames();
    TestScalerRecoversSlowly();
    TestScalerDoesNotOscillate();
    TestScalerRespectsCeilingAndDisable();

    TestThermalClampsAndHolds();
    TestThermalSevereDropsFrameRate();
    TestThermalHeadroomIsGradual();

    TestTopologyThreeClusters();
    TestTopologyTwoClusters();
    TestTopologyUniformAndUnreadable();
    TestTopologyPrefersCapacity();

    TestGpuClassification();
    TestDeviceDemotion();
    TestProfileContents();

    TestDirectorEndToEnd();

    std::printf("%s: %d checks, %d failures\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
