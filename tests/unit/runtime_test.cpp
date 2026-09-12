#include "dlss_bridge/runtime.hpp"
#include <assert.h>
#include <string.h>

using namespace dlss_bridge;

int main()
{
    RuntimeConfig cfg;
    assert(cfg.require_neural_result == 1);
    assert(cfg.Apply("execution_mode", "secondary_gpu"));
    assert(cfg.execution_mode == ExecutionMode::SecondaryGpu);
    assert(cfg.Apply("compute_adapter", "luid:00000001:89abcdef"));
    assert(cfg.compute_adapter.kind == SelectorKind::Luid);
    assert(cfg.compute_adapter.luid_high == 1);
    assert(cfg.compute_adapter.luid_low == 0x89abcdefu);
    assert(!cfg.Apply("compute_adapter", "unstable-name"));
    assert(cfg.output_transport == OutputTransport::Native);
    assert(cfg.Apply("output_transport", "r11g11b10_float"));
    assert(cfg.output_transport == OutputTransport::R11G11B10Float);
    assert(!cfg.Apply("output_transport", "lossy_unknown"));
    assert(cfg.neural_queue_mode == NeuralQueueMode::Split);
    assert(cfg.Apply("neural_queue_mode", "unified"));
    assert(cfg.neural_queue_mode == NeuralQueueMode::Unified);
    assert(!cfg.Apply("neural_queue_mode", "unknown"));
    assert(cfg.neural_placement == NeuralPlacement::AfterSr);
    assert(cfg.Apply("neural_placement", "before_sr"));
    assert(cfg.neural_placement == NeuralPlacement::BeforeSr);
    assert(cfg.Apply("neural_working_scale", "0.75"));
    assert(cfg.neural_working_scale == 0.75f);
    assert(!cfg.Apply("neural_working_scale", "0.1"));
    assert(!cfg.Apply("neural_working_scale", "nan"));
    assert(!cfg.Apply("neural_working_scale", "inf"));
    assert(cfg.Apply("neural_passes", "1"));
    assert(!cfg.Apply("neural_passes", "4"));
    assert(cfg.Apply("neural_runtime_variant", "presr-v0.7.7"));
    assert(!strcmp(cfg.neural_runtime_variant, "presr-v0.7.7"));
    assert(!cfg.Apply("neural_runtime_variant", "../unsafe"));

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
