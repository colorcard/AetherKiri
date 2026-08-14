#ifndef AETHERKIRI_ENGINE_SCENARIO_H_
#define AETHERKIRI_ENGINE_SCENARIO_H_

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "engine_api.h"

struct ScenarioStep {
    std::string action;
    std::string name;
    std::string pattern;
    std::string layer_name;
    std::string layer_path;
    std::string property;
    std::string string_value;
    double number_value = 0;
    double x = 0;
    double y = 0;
    int key = 0;
    uint32_t frames = 0;
    uint32_t duration_ms = 0;
    uint32_t timeout_ms = 10000;
};

struct ScenarioProfile {
    uint32_t version = 0;
    std::vector<ScenarioStep> steps;
};

bool LoadScenarioProfile(const std::string &path, ScenarioProfile &profile,
                         std::string &error);

class ScenarioRunner {
public:
    enum class CheckpointStatus { Pending, Completed, Failed };
    using CheckpointCallback = std::function<CheckpointStatus(
        const std::string &, std::string &)>;

    explicit ScenarioRunner(ScenarioProfile profile);
    bool Tick(engine_handle_t engine, uint64_t frame_serial, uint64_t now_ms,
              const std::string &runtime_logs,
              const std::string &visual_snapshot,
              const CheckpointCallback &checkpoint);
    bool finished() const { return finished_; }
    bool failed() const { return failed_; }
    const std::string &failure() const { return failure_; }

private:
    bool SendInput(engine_handle_t engine, uint32_t type, double x, double y,
                   int key);
    bool LayerMatches(const ScenarioStep &step,
                      const std::string &snapshot) const;
    void Fail(const ScenarioStep &step, const std::string &reason);

    ScenarioProfile profile_;
    size_t index_ = 0;
    uint64_t step_started_ms_ = 0;
    uint64_t step_started_frame_ = 0;
    uint64_t key_release_ms_ = 0;
    int held_key_ = 0;
    bool key_hold_completed_ = false;
    uint32_t stable_count_ = 0;
    std::string last_stable_snapshot_;
    bool finished_ = false;
    bool failed_ = false;
    std::string failure_;
    std::vector<std::string> forbidden_log_patterns_;
};

#endif
