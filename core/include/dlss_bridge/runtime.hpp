#pragma once

#include <stddef.h>
#include <stdint.h>

namespace dlss_bridge {

enum class ExecutionMode : uint8_t { Auto = 0, SameGpu = 1, SecondaryGpu = 2 };
enum class OutputTransport : uint8_t { Native = 0, R11G11B10Float = 1 };
enum class NeuralQueueMode : uint8_t { Split = 0, Unified = 1 };
enum class NeuralPlacement : uint8_t { AfterSr = 0, BeforeSr = 1, DeferredDlss = 2 };
enum class SelectorKind : uint8_t { Auto = 0, Game = 1, Index = 2, Luid = 3, Uuid = 4, Pci = 5 };
enum class FrameState : uint8_t {
    Free, Captured, InputsReady, Uploading, Executing, Downloading,
    Reinserted, Retired, NeuralRepeat, Abandoned
};

struct AdapterSelector {
    SelectorKind kind;
    int index;
    uint32_t luid_high;
    uint32_t luid_low;
    char text[96];
};

struct RuntimeConfig {
    int mode;
    int flags;
    int subrects;
    int verbose;
    int sync;
    int game_eval;              // legacy input; strict policy overrides it
    int neural_adapter;         // legacy input; converted to an index selector
    int require_neural_result;
    int ring_slots;
    int latency_budget_ms;
    ExecutionMode execution_mode;
    OutputTransport output_transport;
    NeuralQueueMode neural_queue_mode;
    NeuralPlacement neural_placement;
    float neural_working_scale;
    int neural_passes;
    char neural_runtime_variant[64];
    AdapterSelector compute_adapter;

    RuntimeConfig();
    bool Apply(const char *key, const char *value);
};

struct FrameSlot {
    uint64_t frame_id;
    uint64_t generation;
    FrameState state;
    int result;
};

class Scheduler {
public:
    Scheduler(FrameSlot *slots, size_t slot_count);
    FrameSlot *Acquire(uint64_t frame_id, uint64_t generation);
    bool Transition(FrameSlot &slot, FrameState next);
    void Retire(FrameSlot &slot);
    size_t Busy() const;
private:
    FrameSlot *slots_;
    size_t count_;
    size_t cursor_;
};

bool ParseAdapterSelector(const char *text, AdapterSelector *out);
const char *ExecutionModeName(ExecutionMode mode);
const char *OutputTransportName(OutputTransport format);
const char *NeuralQueueModeName(NeuralQueueMode mode);
const char *NeuralPlacementName(NeuralPlacement placement);
bool IsStrictNeural(const RuntimeConfig &cfg);

} // namespace dlss_bridge
