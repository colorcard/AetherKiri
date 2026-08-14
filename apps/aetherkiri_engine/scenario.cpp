#include "scenario.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <utility>

namespace {

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Json> array;
    std::map<std::string, Json> object;

    const Json *Get(const std::string &key) const {
        const auto it = object.find(key);
        return it == object.end() ? nullptr : &it->second;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string &text) : text_(text) {}
    bool Parse(Json &out, std::string &error) {
        Skip();
        if(!Value(out)) return Error(error);
        Skip();
        if(pos_ != text_.size()) return SetError("trailing data", error);
        return true;
    }

private:
    void Skip() {
        while(pos_ < text_.size() &&
              (text_[pos_] == ' ' || text_[pos_] == '\n' ||
               text_[pos_] == '\r' || text_[pos_] == '\t')) ++pos_;
    }
    bool Value(Json &out) {
        Skip();
        if(pos_ >= text_.size()) return Remember("unexpected end of input");
        const char ch = text_[pos_];
        if(ch == '{') return ObjectValue(out);
        if(ch == '[') return ArrayValue(out);
        if(ch == '"') { out.type = Json::String; return StringValue(out.string); }
        if(ch == 't' && Consume("true")) { out.type = Json::Bool; out.boolean = true; return true; }
        if(ch == 'f' && Consume("false")) { out.type = Json::Bool; return true; }
        if(ch == 'n' && Consume("null")) { out.type = Json::Null; return true; }
        return NumberValue(out);
    }
    bool ObjectValue(Json &out) {
        out.type = Json::Object; ++pos_; Skip();
        if(Take('}')) return true;
        for(;;) {
            std::string key;
            if(!StringValue(key)) return false;
            Skip();
            if(!Take(':')) return Remember("expected ':'");
            Json value;
            if(!Value(value)) return false;
            out.object.emplace(std::move(key), std::move(value));
            Skip();
            if(Take('}')) return true;
            if(!Take(',')) return Remember("expected ',' or '}'");
            Skip();
        }
    }
    bool ArrayValue(Json &out) {
        out.type = Json::Array; ++pos_; Skip();
        if(Take(']')) return true;
        for(;;) {
            Json value;
            if(!Value(value)) return false;
            out.array.push_back(std::move(value));
            Skip();
            if(Take(']')) return true;
            if(!Take(',')) return Remember("expected ',' or ']'");
            Skip();
        }
    }
    bool StringValue(std::string &out) {
        if(!Take('"')) return Remember("expected string");
        while(pos_ < text_.size()) {
            const unsigned char ch = text_[pos_++];
            if(ch == '"') return true;
            if(ch != '\\') { out.push_back(static_cast<char>(ch)); continue; }
            if(pos_ >= text_.size()) return Remember("unterminated escape");
            const char escaped = text_[pos_++];
            switch(escaped) {
                case '"': case '\\': case '/': out.push_back(escaped); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: return Remember("unsupported string escape");
            }
        }
        return Remember("unterminated string");
    }
    bool NumberValue(Json &out) {
        const size_t start = pos_;
        if(pos_ < text_.size() && text_[pos_] == '-') ++pos_;
        while(pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        if(pos_ < text_.size() && text_[pos_] == '.') {
            ++pos_;
            while(pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
        }
        if(start == pos_) return Remember("expected value");
        try { out.number = std::stod(text_.substr(start, pos_ - start)); }
        catch(...) { return Remember("invalid number"); }
        out.type = Json::Number;
        return true;
    }
    bool Take(char ch) {
        if(pos_ < text_.size() && text_[pos_] == ch) { ++pos_; return true; }
        return false;
    }
    bool Consume(const char *value) {
        const size_t length = std::strlen(value);
        if(text_.compare(pos_, length, value) != 0) return false;
        pos_ += length; return true;
    }
    bool Remember(const char *message) { message_ = message; return false; }
    bool Error(std::string &error) const {
        std::ostringstream out; out << message_ << " at byte " << pos_;
        error = out.str(); return false;
    }
    bool SetError(const char *message, std::string &error) {
        Remember(message); return Error(error);
    }
    const std::string &text_;
    size_t pos_ = 0;
    std::string message_;
};

bool StringField(const Json &obj, const char *key, std::string &out,
                 bool required, std::string &error) {
    const Json *value = obj.Get(key);
    if(!value) {
        if(required) error = std::string("missing string field '") + key + "'";
        return !required;
    }
    if(value->type != Json::String) {
        error = std::string("field '") + key + "' must be a string";
        return false;
    }
    out = value->string; return true;
}

bool NumberField(const Json &obj, const char *key, double &out,
                 std::string &error) {
    const Json *value = obj.Get(key);
    if(!value) return true;
    if(value->type != Json::Number || !std::isfinite(value->number)) {
        error = std::string("field '") + key + "' must be a finite number";
        return false;
    }
    out = value->number; return true;
}

std::string NormalizeSnapshot(const std::string &snapshot) {
    std::string normalized = snapshot;
    const auto frame = normalized.find("\"frame_serial\":");
    if(frame != std::string::npos) {
        auto begin = frame + std::strlen("\"frame_serial\":");
        auto end = begin;
        while(end < normalized.size() && normalized[end] >= '0' && normalized[end] <= '9') ++end;
        normalized.replace(begin, end - begin, "0");
    }
    return normalized;
}

}  // namespace

bool LoadScenarioProfile(const std::string &path, ScenarioProfile &profile,
                         std::string &error) {
    std::ifstream input(path, std::ios::binary);
    if(!input) { error = "cannot open scenario: " + path; return false; }
    const std::string text((std::istreambuf_iterator<char>(input)), {});
    Json root;
    if(!JsonParser(text).Parse(root, error)) return false;
    if(root.type != Json::Object) { error = "scenario root must be an object"; return false; }
    const Json *version = root.Get("version");
    const Json *steps = root.Get("steps");
    if(!version || version->type != Json::Number || version->number != 1) {
        error = "scenario version must be 1"; return false;
    }
    if(!steps || steps->type != Json::Array) { error = "scenario steps must be an array"; return false; }
    profile.version = 1;
    for(size_t i = 0; i < steps->array.size(); ++i) {
        const Json &value = steps->array[i];
        if(value.type != Json::Object) { error = "scenario step must be an object"; return false; }
        ScenarioStep step;
        if(!StringField(value, "action", step.action, true, error) ||
           !StringField(value, "name", step.name, false, error) ||
           !StringField(value, "pattern", step.pattern, false, error) ||
           !StringField(value, "layer_name", step.layer_name, false, error) ||
           !StringField(value, "layer_path", step.layer_path, false, error) ||
           !StringField(value, "property", step.property, false, error) ||
           !StringField(value, "string_value", step.string_value, false, error)) return false;
        double frames = 0, duration = 0, timeout = step.timeout_ms, key = 0;
        if(!NumberField(value, "x", step.x, error) || !NumberField(value, "y", step.y, error) ||
           !NumberField(value, "value", step.number_value, error) ||
           !NumberField(value, "frames", frames, error) ||
           !NumberField(value, "duration_ms", duration, error) ||
           !NumberField(value, "timeout_ms", timeout, error) ||
           !NumberField(value, "key", key, error)) return false;
        if(frames < 0 || duration < 0 || timeout <= 0 || key < 0) {
            error = "scenario numeric fields are out of range"; return false;
        }
        step.frames = static_cast<uint32_t>(frames);
        step.duration_ms = static_cast<uint32_t>(duration);
        step.timeout_ms = static_cast<uint32_t>(timeout);
        step.key = static_cast<int>(key);
        profile.steps.push_back(std::move(step));
    }
    return true;
}

ScenarioRunner::ScenarioRunner(ScenarioProfile profile)
    : profile_(std::move(profile)) {}

bool ScenarioRunner::SendInput(engine_handle_t engine, uint32_t type,
                               double x, double y, int key) {
    engine_input_event_t event{};
    event.struct_size = sizeof(event);
    event.type = type;
    event.x = x; event.y = y; event.key_code = key;
    event.button = type == ENGINE_INPUT_EVENT_POINTER_DOWN ||
                           type == ENGINE_INPUT_EVENT_POINTER_UP ? 0 : -1;
    return engine_send_input(engine, &event) == ENGINE_RESULT_OK;
}

bool ScenarioRunner::LayerMatches(const ScenarioStep &step,
                                  const std::string &snapshot) const {
    Json root;
    std::string error;
    if(!JsonParser(snapshot).Parse(root, error)) return false;
    const Json *layers = root.Get("layers");
    if(!layers || layers->type != Json::Array) return false;
    for(const Json &layer : layers->array) {
        if(layer.type != Json::Object) continue;
        const Json *name = layer.Get("name");
        const Json *path = layer.Get("path");
        if(!step.layer_name.empty() &&
           (!name || name->type != Json::String || name->string.find(step.layer_name) == std::string::npos)) continue;
        if(!step.layer_path.empty() &&
           (!path || path->type != Json::String || path->string.find(step.layer_path) == std::string::npos)) continue;
        if(step.property.empty()) return true;
        const Json *property = layer.Get(step.property);
        if(!property) continue;
        if(property->type == Json::Number && property->number == step.number_value) return true;
        if(property->type == Json::String && property->string == step.string_value) return true;
        if(property->type == Json::Bool && property->boolean == (step.number_value != 0)) return true;
    }
    return false;
}

void ScenarioRunner::Fail(const ScenarioStep &step, const std::string &reason) {
    failed_ = true;
    failure_ = "step " + std::to_string(index_) + " (" + step.action + "): " + reason;
}

bool ScenarioRunner::Tick(engine_handle_t engine, uint64_t frame_serial,
                          uint64_t now_ms, const std::string &runtime_logs,
                          const std::string &visual_snapshot,
                          const CheckpointCallback &checkpoint) {
    if(finished_ || failed_) return false;
    if(index_ >= profile_.steps.size()) { finished_ = true; return true; }
    ScenarioStep &step = profile_.steps[index_];
    if(step_started_ms_ == 0) { step_started_ms_ = now_ms; step_started_frame_ = frame_serial; }
    for(const std::string &pattern : forbidden_log_patterns_) {
        if(runtime_logs.find(pattern) != std::string::npos) {
            Fail(step, "forbidden log matched: " + pattern);
            return false;
        }
    }

    if(held_key_ != 0 && key_release_ms_ != 0 && now_ms >= key_release_ms_) {
        if(!SendInput(engine, ENGINE_INPUT_EVENT_KEY_UP, 0, 0, held_key_)) {
            Fail(step, "key release failed"); return false;
        }
        held_key_ = 0;
        key_hold_completed_ = true;
    }
    if(now_ms - step_started_ms_ > step.timeout_ms) {
        if(held_key_ != 0)
            SendInput(engine, ENGINE_INPUT_EVENT_KEY_UP, 0, 0, held_key_);
        held_key_ = 0;
        key_release_ms_ = 0;
        Fail(step, "timeout after " + std::to_string(step.timeout_ms) + " ms"); return false;
    }

    bool complete = false;
    if(step.action == "wait_startup") complete = true;
    else if(step.action == "wait_frames") complete = frame_serial - step_started_frame_ >= step.frames;
    else if(step.action == "wait_ms" || step.action == "performance_sample") complete = now_ms - step_started_ms_ >= step.duration_ms;
    else if(step.action == "wait_log") complete = runtime_logs.find(step.pattern) != std::string::npos;
    else if(step.action == "forbid_log") {
        if(runtime_logs.find(step.pattern) != std::string::npos) { Fail(step, "forbidden log matched: " + step.pattern); return false; }
        forbidden_log_patterns_.push_back(step.pattern);
        complete = true;
    } else if(step.action == "wait_layer") complete = LayerMatches(step, visual_snapshot);
    else if(step.action == "wait_stable_frames") {
        const std::string normalized = NormalizeSnapshot(visual_snapshot);
        stable_count_ = normalized == last_stable_snapshot_ ? stable_count_ + 1 : 1;
        last_stable_snapshot_ = normalized;
        complete = stable_count_ >= step.frames;
    } else if(step.action == "move") {
        complete = SendInput(engine, ENGINE_INPUT_EVENT_POINTER_MOVE, step.x, step.y, 0);
    } else if(step.action == "click") {
        complete = SendInput(engine, ENGINE_INPUT_EVENT_POINTER_DOWN, step.x, step.y, 0) &&
                   SendInput(engine, ENGINE_INPUT_EVENT_POINTER_UP, step.x, step.y, 0);
    } else if(step.action == "key") {
        complete = SendInput(engine, ENGINE_INPUT_EVENT_KEY_DOWN, 0, 0, step.key) &&
                   SendInput(engine, ENGINE_INPUT_EVENT_KEY_UP, 0, 0, step.key);
    } else if(step.action == "key_hold") {
        if(key_hold_completed_) {
            complete = true;
        } else if(held_key_ == 0) {
            if(!SendInput(engine, ENGINE_INPUT_EVENT_KEY_DOWN, 0, 0, step.key)) {
                Fail(step, "key down failed"); return false;
            }
            held_key_ = step.key; key_release_ms_ = now_ms + step.duration_ms;
        }
    } else if(step.action == "key_until_layer") {
        if(LayerMatches(step, visual_snapshot)) {
            if(held_key_ != 0 &&
               !SendInput(engine, ENGINE_INPUT_EVENT_KEY_UP, 0, 0, held_key_)) {
                Fail(step, "key release failed"); return false;
            }
            held_key_ = 0;
            key_release_ms_ = 0;
            complete = true;
        } else if(held_key_ == 0) {
            if(!SendInput(engine, ENGINE_INPUT_EVENT_KEY_DOWN, 0, 0, step.key)) {
                Fail(step, "key down failed"); return false;
            }
            held_key_ = step.key;
            key_release_ms_ = 0;
        }
    } else if(step.action == "checkpoint" || step.action == "screenshot") {
        std::string error;
        const CheckpointStatus status = checkpoint(
            step.name.empty() ? "checkpoint" : step.name, error);
        complete = status == CheckpointStatus::Completed;
        if(status == CheckpointStatus::Failed) { Fail(step, error); return false; }
    } else {
        Fail(step, "unsupported action"); return false;
    }

    if(complete) {
        ++index_;
        step_started_ms_ = 0; step_started_frame_ = 0;
        stable_count_ = 0; last_stable_snapshot_.clear();
        key_hold_completed_ = false;
        if(index_ >= profile_.steps.size()) finished_ = true;
    }
    return true;
}
