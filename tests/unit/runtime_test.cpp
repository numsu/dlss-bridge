#include "dlss_bridge/runtime.hpp"
#include "dlss_bridge/components.hpp"
#include <assert.h>
#include <string.h>

using namespace dlss_bridge;

int main()
{
    RuntimeComposition components{};
    assert(components.Configure("injected-vulkan"));
    assert(components.SelectTransport(true));
    assert(components.Ready());
    assert(!components.Configure("missing-host"));
    RuntimeConfig cfg;
    assert(cfg.Apply("ring_slots", "8"));
    assert(cfg.ring_slots == 8);
    assert(!cfg.Apply("ring_slots", "1"));
    assert(!cfg.Apply("ring_slots", "9"));
    assert(cfg.Apply("neural_pipeline_frames", "3"));
    assert(cfg.neural_pipeline_frames == 3);
    assert(cfg.Valid());
    assert(!cfg.Apply("neural_pipeline_frames", "0"));
    assert(!cfg.Apply("neural_pipeline_frames", "8"));
    assert(!cfg.Apply("latency_budget_ms", "0"));
    assert(!cfg.Apply("latency_budget_ms", "126"));
    assert(!cfg.Apply("latency_budget_ms", "12ms"));
    assert(cfg.Apply("latency_budget_ms", "20"));
    assert(cfg.Apply("execution_mode", "secondary_gpu"));
    assert(cfg.execution_mode == ExecutionMode::SecondaryGpu);
    assert(cfg.Apply("compute_adapter", "luid:00000001:89abcdef"));
    assert(cfg.compute_adapter.kind == SelectorKind::Luid);
    assert(cfg.compute_adapter.luid_high == 1);
    assert(cfg.compute_adapter.luid_low == 0x89abcdefu);
    assert(!cfg.Apply("compute_adapter", "unstable-name"));
    assert(!cfg.Apply("compute_adapter", "uuid:GPU-example"));
    assert(cfg.output_transport == OutputTransport::Native);
    assert(cfg.Apply("output_transport", "r11g11b10_float"));
    assert(cfg.output_transport == OutputTransport::R11G11B10Float);
    assert(!cfg.Apply("output_transport", "lossy_unknown"));
    assert(cfg.neural_queue_mode == NeuralQueueMode::Split);
    assert(cfg.Apply("neural_queue_mode", "unified"));
    assert(cfg.neural_queue_mode == NeuralQueueMode::Unified);
    assert(!cfg.Apply("neural_queue_mode", "unknown"));
    assert(cfg.Apply("gpu_timestamps", "0"));
    assert(cfg.gpu_timestamps == 0);
    assert(!cfg.Apply("gpu_timestamps", "2"));
    assert(!cfg.Apply("verbose", "yes"));
    RuntimeConfig inconsistent;
    assert(inconsistent.Apply("ring_slots", "2"));
    assert(!inconsistent.Valid());

    FrameSlot slots[3];
    Scheduler scheduler(slots, 3);
    FrameSlot *a = scheduler.Acquire(10, 1);
    assert(a && a->state == FrameState::Captured);
    assert(scheduler.Transition(*a, FrameState::InputsReady));
    assert(!scheduler.Transition(*a, FrameState::Downloading));
    assert(scheduler.Transition(*a, FrameState::Uploading));
    assert(scheduler.Transition(*a, FrameState::Executing));
    assert(scheduler.Transition(*a, FrameState::Downloading));
    assert(scheduler.Transition(*a, FrameState::Reinserted));
    assert(scheduler.Transition(*a, FrameState::Retired));
    assert(scheduler.Busy() == 0);
    return 0;
}
