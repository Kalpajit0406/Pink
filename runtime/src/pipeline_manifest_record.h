#pragma once
// Offline Pipeline Manifest Recorder (Milestone O4)
// Records distinct graphics and compute pipeline states during gameplay under LEONIDA_PIPE_RECORD=1.
// Emits pipelines.manifest in line-delimited JSON (JSONL) for offline pre-warming.
// Section 7.2 of OFFLINE_PIPELINE_SPEC.md.

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace leonida::offline {

struct RecordedPipeline {
    std::string type;               // "graphics" or "compute"
    std::string key;                // unique identification string
    std::vector<std::string> stages;// stage hashes (e.g. VS, PS, CS)
    std::vector<uint32_t> rtv_formats;
    uint32_t dsv_format = 0;
    uint32_t sample_count = 1;
    uint32_t topology = 0;
    uint32_t blend_mask = 0;
    bool depth_enable = false;
    bool depth_write = false;
    uint32_t depth_func = 0;
    uint32_t cull_mode = 0;
    uint64_t seen_count = 1;
    uint64_t compile_us = 0;

    std::string to_json() const {
        std::ostringstream ss;
        ss << "{\"type\":\"" << type << "\",\"key\":\"" << key << "\",\"stages\":[";
        for (std::size_t i = 0; i < stages.size(); ++i) {
            ss << "\"" << stages[i] << "\"" << (i + 1 < stages.size() ? "," : "");
        }
        ss << "],\"rtv\":[";
        for (std::size_t i = 0; i < rtv_formats.size(); ++i) {
            ss << rtv_formats[i] << (i + 1 < rtv_formats.size() ? "," : "");
        }
        ss << "],\"dsv\":" << dsv_format
           << ",\"samples\":" << sample_count
           << ",\"topology\":" << topology
           << ",\"blend\":" << blend_mask
           << ",\"depth_enable\":" << (depth_enable ? 1 : 0)
           << ",\"depth_write\":" << (depth_write ? 1 : 0)
           << ",\"depth_func\":" << depth_func
           << ",\"cull\":" << cull_mode
           << ",\"seen\":" << seen_count
           << ",\"compile_us\":" << compile_us
           << "}";
        return ss.str();
    }
};

class PipelineManifestRecorder {
public:
    static PipelineManifestRecorder& instance() {
        static PipelineManifestRecorder s_instance;
        return s_instance;
    }

    PipelineManifestRecorder() {
        const char* env = std::getenv("LEONIDA_PIPE_RECORD");
        const char* env_alt = std::getenv("LEONIDA_RECORD_PIPELINES");
        enabled_ = (env && std::string_view(env) == "1") || (env_alt && std::string_view(env_alt) == "1");
        if (enabled_) {
            std::atexit([]() {
                PipelineManifestRecorder::instance().flush();
            });
        }
    }

    ~PipelineManifestRecorder() {
        if (enabled_) flush();
    }

    bool is_enabled() const { return enabled_; }

    void record_graphics(
        std::string_view key,
        const std::vector<std::string>& stages,
        const std::vector<uint32_t>& rtv_formats,
        uint32_t dsv_format,
        uint32_t sample_count,
        uint32_t topology,
        uint32_t blend_mask,
        bool depth_enable,
        bool depth_write,
        uint32_t depth_func,
        uint32_t cull_mode,
        uint64_t compile_us
    ) {
        if (!enabled_) return;
        std::lock_guard lock(mutex_);
        auto it = pipelines_.find(std::string(key));
        if (it != pipelines_.end()) {
            it->second.seen_count++;
            it->second.compile_us += compile_us;
            return;
        }

        RecordedPipeline p;
        p.type = "graphics";
        p.key = std::string(key);
        p.stages = stages;
        p.rtv_formats = rtv_formats;
        p.dsv_format = dsv_format;
        p.sample_count = sample_count;
        p.topology = topology;
        p.blend_mask = blend_mask;
        p.depth_enable = depth_enable;
        p.depth_write = depth_write;
        p.depth_func = depth_func;
        p.cull_mode = cull_mode;
        p.seen_count = 1;
        p.compile_us = compile_us;

        pipelines_.emplace(p.key, std::move(p));
        dirty_ = true;

        if (pipelines_.size() % 100 == 0) {
            flush_locked();
        }
    }

    void record_compute(std::string_view key, std::string_view cs_hash, uint64_t compile_us) {
        if (!enabled_) return;
        std::lock_guard lock(mutex_);
        auto it = pipelines_.find(std::string(key));
        if (it != pipelines_.end()) {
            it->second.seen_count++;
            it->second.compile_us += compile_us;
            return;
        }

        RecordedPipeline p;
        p.type = "compute";
        p.key = std::string(key);
        p.stages.push_back(std::string(cs_hash));
        p.seen_count = 1;
        p.compile_us = compile_us;

        pipelines_.emplace(p.key, std::move(p));
        dirty_ = true;

        if (pipelines_.size() % 100 == 0) {
            flush_locked();
        }
    }

    void flush() {
        std::lock_guard lock(mutex_);
        flush_locked();
    }

private:
    void flush_locked() {
        if (!dirty_ || pipelines_.empty()) return;

        std::filesystem::path manifest_path = "pipelines.manifest";
        const char* custom_path = std::getenv("LEONIDA_MANIFEST_PATH");
        if (custom_path && custom_path[0]) {
            manifest_path = custom_path;
        }

        std::ofstream file(manifest_path, std::ios::out | std::ios::trunc);
        if (!file) return;

        for (const auto& [_, p] : pipelines_) {
            file << p.to_json() << "\n";
        }
        file.flush();
        dirty_ = false;
    }

    bool enabled_ = false;
    bool dirty_ = false;
    std::mutex mutex_;
    std::unordered_map<std::string, RecordedPipeline> pipelines_;
};

}  // namespace leonida::offline
