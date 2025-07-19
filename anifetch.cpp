#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <set>
#include <map>
#include <mutex>
#include <print>
#include <queue>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

// Forward declaration for AnifetchArgs for get_file_stats_string_for_hashing
struct AnifetchArgs;
extern AnifetchArgs g_args; // Declare g_args as extern

// Struct for the frame index entry in our packed file
struct FrameIndexEntry {
    unsigned long long offset; // Byte offset in the data file
    unsigned long long length; // Length of the frame in bytes
};

// Helper to build a sorted, stable string from a map for hashing/caching.
std::string serialize_map_for_hashing(const std::map<std::string, std::string>& m) {
    std::string serialized_string;
    for (const auto& [key, value] : m) {
        std::format_to(std::back_inserter(serialized_string), "{}={};", key, value);
    }
    return serialized_string;
}

// Global Configuration & State
struct AnifetchArgs {
    std::string filename;
    int width = 80;
    int height_arg = 24;
    int actual_chafa_height = 0; // Determined height of ASCII art

    bool debug = false;
    int framerate = 30;
    std::string sound_arg;          // User-provided sound file or "" for extraction
    bool sound_flag_given = false;
    std::string sound_saved_path;   // Path to cached sound file
    std::map<std::string, std::string> chafa_options; // Direct mapping of Chafa flags
    std::string chroma_arg;         // Chroma key color
    bool chroma_flag_given = false;
    int num_frames = 0;             // Total ASCII frames generated/cached

    // Helper for to_cache_map, defined after AnifetchArgs
    std::string get_file_stats_string_for_hashing_member(const std::string& filepath) const;

    // Data for cache.txt
    std::map<std::string, std::string> to_cache_map(double current_video_duration) const {
        std::map<std::string, std::string> m;
        m["file_basename"] = std::filesystem::path(filename).filename().string();
        m["video_file_identity"] = get_file_stats_string_for_hashing_member(filename); // Store for inspection
        m["width"] = std::to_string(width);
        m["height_arg"] = std::to_string(height_arg);
        m["framerate"] = std::to_string(framerate);
        m["chroma"] = chroma_arg;
        m["sound"] = sound_arg;
        m["original_full_filename"] = filename;
        m["actual_chafa_height"] = std::to_string(actual_chafa_height);
        m["sound_saved_path"] = sound_saved_path;
        m["num_frames"] = std::to_string(num_frames);
        m["video_duration_cached"] = std::to_string(current_video_duration); // Store cached duration
        m["frames_dat_size"] = "0"; // Placeholder for the data file size
        m["chafa_options"] = serialize_map_for_hashing(chafa_options); // Store all other chafa args
        return m;
    }

    // Data for cache hash generation and input comparison
    std::map<std::string, std::string> to_input_map() const {
        std::map<std::string, std::string> m;
        m["file_basename"] = std::filesystem::path(filename).filename().string();
        m["video_file_identity"] = get_file_stats_string_for_hashing_member(filename);
        m["width"] = std::to_string(width);
        m["height_arg"] = std::to_string(height_arg);
        m["framerate"] = std::to_string(framerate);
        m["chroma"] = chroma_arg;
        m["sound"] = sound_arg;
        m["chafa_options"] = serialize_map_for_hashing(chafa_options); // All other chafa args
        return m;
    }
    
    // Generates the full argument string for the chafa command
    std::string build_chafa_command_string(int height_override) const {
        std::string cmd = "chafa";
        for (const auto& [key, value] : chafa_options) {
            cmd += " " + key;
            if (!value.empty()) {
                // Shell-escape the value
                cmd += " '" + value + "'";
            }
        }
        // Enforce anifetch's size parameter, overriding any --size in chafa_options (chafa also uses --size <WxH>)
        cmd += " --size=" + std::to_string(width) + "x" + std::to_string(height_override);
        cmd += " --format symbols"; // This is required by the program logic
        return cmd;
    }
};

AnifetchArgs g_args; // Global application arguments

// Cache and Asset Paths
std::filesystem::path g_video_specific_cache_root;    // e.g., [project_root]/.cache/myvideo.mp4/
std::filesystem::path g_current_args_cache_dir;       // e.g., [project_root]/.cache/myvideo.mp4/hash123/
std::filesystem::path g_processed_tiff_path;           // Final tiffs from FFmpeg (e.g., .../hash123/final_tiffs/)
std::filesystem::path g_temp_tiff_segments_path;       // Temp dir for FFmpeg segment outputs
std::filesystem::path g_processed_ascii_path;         // Final ASCII art files (e.g., .../hash123/ascii_art/)
std::filesystem::path g_current_cache_metadata_file;  // e.g., .../hash123/cache.txt

// Threading & Synchronization Primitives
std::mutex g_debug_mutex; // For thread-safe debug output
std::mutex g_cerr_mutex;    // For thread-safe std::cerr output

std::queue<std::pair<std::filesystem::path, int>> g_ascii_conversion_queue; // tiffs waiting for ASCII conversion
std::mutex g_conversion_queue_mutex;
std::condition_variable g_conversion_queue_cv;

std::atomic<bool> g_ffmpeg_extraction_done(false);  // True when all FFmpeg video segments are processed
std::atomic<int> g_ffmpeg_tasks_completed(0);        // Count of FFmpeg segments fully extracted
std::atomic<bool> g_tiff_processing_done(false);     // True when tiff preparation/dispatching is complete
std::atomic<int> g_tiffs_ready_for_ascii(0);         // Count of tiffs successfully prepared and queued
std::atomic<int> g_ascii_frames_completed(0);       // Count of ASCII files successfully converted and saved
std::atomic<bool> g_pipeline_error_occurred(false); // Global flag for critical pipeline errors
std::atomic<bool> g_exit_requested(false);          // Flag for graceful shutdown from signal handler

// Terminal State & Process Management
struct termios g_original_termios; // Stores original terminal settings
bool g_termios_saved = false;
pid_t g_ffplay_pid = -1;           // PID of the ffplay audio process

// Print message if debug mode is enabled
void debug_output(const std::string& msg) {
    if (g_args.debug) {
        std::lock_guard<std::mutex> lock(g_debug_mutex);
        std::println("[DEBUG] {}", msg);
    }
}

// Get file statistics as a string for hashing purposes
std::string AnifetchArgs::get_file_stats_string_for_hashing_member(const std::string& filepath) const {
    std::error_code ec;
    // Ensure the path is absolute to handle relative paths consistently
    std::filesystem::path p = std::filesystem::absolute(filepath, ec);
    if (ec) {
        return "file_stat_error_or_missing_" + filepath;
    }

    if (std::filesystem::exists(p, ec) && !ec && std::filesystem::is_regular_file(p, ec) && !ec) {
        auto fsize = std::filesystem::file_size(p, ec);
        if (ec) return "file_stat_error_or_missing_" + filepath;

        auto fmtime = std::filesystem::last_write_time(p, ec);
        if (ec) return "file_stat_error_or_missing_" + filepath;

        auto fmtime_sec = std::chrono::duration_cast<std::chrono::seconds>(fmtime.time_since_epoch());
        return std::format("s:{}_m:{}", fsize, fmtime_sec.count());
    }
    return "file_not_found_or_not_regular_" + filepath;
}


int run_command_silent_ex(const std::string& command_str, bool suppress_output_even_if_debug = false) {
    debug_output("Executing: " + command_str);
    std::string cmd_to_run = command_str;
    if (!g_args.debug || suppress_output_even_if_debug) {
        cmd_to_run += " > /dev/null 2>&1";
    }
    int sys_ret = system(cmd_to_run.c_str());
    int exit_code = 0;
    if (WIFEXITED(sys_ret)) {
        exit_code = WEXITSTATUS(sys_ret);
    } else {
        exit_code = -1; // Abnormal termination
    }

    if (exit_code != 0) {
        std::lock_guard<std::mutex> lock(g_cerr_mutex);
        std::println(stderr, "ERROR: Command failed with exit code {}: {}", exit_code, command_str);
    }
    return exit_code;
}

// Execute a command and capture its stdout
std::string run_command_with_output_ex(const std::string& command_str) {
    debug_output("Executing for output: " + command_str);
    std::string result = "";
    FILE* pipe = popen(command_str.c_str(), "r");
    if (!pipe) {
        std::lock_guard<std::mutex> lock(g_cerr_mutex);
        std::println(stderr, "ERROR: popen() failed for command: {} Error: {}", command_str, strerror(errno));
        return "";
    }
    char buffer[2048];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    int exit_status = pclose(pipe);
    if (WIFEXITED(exit_status)) {
        int actual_exit_code = WEXITSTATUS(exit_status);
        if (actual_exit_code != 0) {
            std::lock_guard<std::mutex> lock(g_cerr_mutex);
            // Chafa fails, had a bad argument.
            std::println(stderr, "ERROR: Command '{}' failed with exit code: {}. Output (if any): {}", command_str, actual_exit_code, result);
            // Invalid argument was likely passed to chafa.
            if(command_str.starts_with("chafa")) {
                std::println(stderr, "This may be due to an unrecognized or invalid argument passed to Chafa.");
            }
        }
    } else {
         std::lock_guard<std::mutex> lock(g_cerr_mutex);
         std::println(stderr, "WARNING: Command '{}' did not terminate normally.", command_str);
    }
    return result;
}

// Generate a hash string from a map of arguments
std::string hash_args_map(const std::map<std::string, std::string>& args_map) {
    std::string combined_string;

    size_t total_size = 0;
    for (const auto& [key, value] : args_map) {
        total_size += key.length() + 1 + value.length() + 1; // for key=value;
    }
    combined_string.reserve(total_size);
    
    for (const auto& [key, value] : args_map) {
        std::format_to(std::back_inserter(combined_string), "{}={};", key, value);
    }
    return std::to_string(std::hash<std::string>{}(combined_string));
}

// Parse simple key-value pairs from cache.txt
std::map<std::string, std::string> parse_cache_txt(const std::filesystem::path& path) {
    std::map<std::string, std::string> m;
    std::ifstream file(path);
    if (!file.is_open()) return m; // File not found or not readable
    std::string line;
    while (std::getline(file, line)) {
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        if (line.empty() || line.starts_with('#')) continue;

        size_t delimiter_pos = line.find('=');
        if (delimiter_pos != std::string::npos) {
            std::string key = line.substr(0, delimiter_pos);
            std::string value = line.substr(delimiter_pos + 1);

            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);

            if (!key.empty()) {
                 m[key] = value;
            } else {
                debug_output("Skipping cache line with empty key: " + line);
            }
        } else {
            debug_output("Skipping malformed cache line (no '='): " + line);
        }
    }
    return m;
}

// Extract audio stream from video file into a .mka container
std::string extract_audio_from_file(const std::string& input_file, const std::filesystem::path& dest_dir) {
    std::filesystem::path audio_file_path = dest_dir / "output_audio.mka";

    // The command uses stream copy (-c:a copy) to avoid re-encoding, preserving original quality.
    std::string cmd = "ffmpeg -i \"" + input_file + "\" -y -vn -c:a copy -loglevel error \"" + audio_file_path.string() + "\"";

    debug_output("Extracting audio: " + cmd);
    int sys_ret = ::system((cmd + (g_args.debug ? "" : " > /dev/null 2>&1")).c_str());

    if (WIFEXITED(sys_ret) && WEXITSTATUS(sys_ret) != 0) {
        // This can happen if the video has no audio stream. FFmpeg returns a non-zero exit code.
        // It's not a critical error for the whole pipeline, so we just return "" and let the caller handle it.
        debug_output("ffmpeg audio extraction failed (exit code " + std::to_string(WEXITSTATUS(sys_ret)) + "). This is normal if the video has no audio.");
        return "";
    } else if (!WIFEXITED(sys_ret)) {
        std::lock_guard<std::mutex> lock(g_cerr_mutex);
        std::println(stderr, "ffmpeg audio extraction did not exit normally.");
        return "";
    }
    return audio_file_path.string();
}

// Get video duration using ffprobe
double get_video_duration_ex(const std::string& filename) {
    debug_output("Probing video duration for: " + filename);
    std::string cmd = "ffprobe -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 \"" + filename + "\"";
    std::string output = run_command_with_output_ex(cmd);
    double duration = 0.0;
    if (!output.empty()) {
        std::from_chars(output.data(), output.data() + output.size(), duration);
    }
    return duration;
}

// Determine actual Chafa output height by processing one frame
bool predetermine_actual_chafa_height() {
    if (g_pipeline_error_occurred.load() || g_exit_requested.load()) return false;
    debug_output("Predetermining actual Chafa output height...");
    std::error_code ec;
    std::filesystem::path temp_first_frame_dir = g_current_args_cache_dir / "temp_first_frame_extract_for_height";
    std::filesystem::create_directories(temp_first_frame_dir, ec);
    if(ec) {
        std::lock_guard<std::mutex> lock(g_cerr_mutex);
        std::println(stderr, "ERROR: Could not create temp directory for height check: {}", ec.message());
        g_pipeline_error_occurred.store(true);
        return false;
    }


    std::string first_frame_filename = std::format("{:09d}.tiff", 1);
    std::filesystem::path first_tiff_path = temp_first_frame_dir / first_frame_filename;

    std::string ffmpeg_filter_complex = "fps=" + std::to_string(g_args.framerate);
    if (g_args.chroma_flag_given) {
        ffmpeg_filter_complex += ",format=rgba,colorkey=" + g_args.chroma_arg + ":similarity=0.01:blend=0";
    } else {
        ffmpeg_filter_complex += ",format=rgb24"; // Default format
    }

    // Extract just the first frame
    std::string ffmpeg_cmd = "ffmpeg -i \"" + g_args.filename + "\" -vf \"" + ffmpeg_filter_complex + "\" -vframes 1 -y \"" + first_tiff_path.string() + "\"";
    if (run_command_silent_ex(ffmpeg_cmd, !g_args.debug) != 0) {
        std::filesystem::remove_all(temp_first_frame_dir, ec);
        g_pipeline_error_occurred.store(true);
        return false;
    }

    if (!std::filesystem::exists(first_tiff_path)) {
        std::lock_guard<std::mutex> lock(g_cerr_mutex);
        std::println(stderr, "ERROR: First frame tiff ({}) not found after FFmpeg extraction.", first_tiff_path.string());
        std::filesystem::remove_all(temp_first_frame_dir, ec);
        g_pipeline_error_occurred.store(true);
        return false;
    }

    // Convert first frame with Chafa to get its height
    std::string chafa_cmd = g_args.build_chafa_command_string(g_args.height_arg) + " \"" + first_tiff_path.string() + "\"";
    std::string ascii_output = run_command_with_output_ex(chafa_cmd);
    std::filesystem::remove_all(temp_first_frame_dir, ec); // Clean up temp dir

    if (ascii_output.empty()) {
        std::lock_guard<std::mutex> lock(g_cerr_mutex);
        std::println(stderr, "ERROR: Chafa produced no output for the first frame during height check. The command used was:\n{}", chafa_cmd);
        std::println(stderr, "Please check that all Chafa arguments are valid.");
        g_pipeline_error_occurred.store(true);
        return false;
    }

    // Count lines in Chafa's output
    size_t line_count = 0;
    std::istringstream stream(ascii_output);
    std::string line_buffer;
    while(std::getline(stream, line_buffer)) line_count++;

    g_args.actual_chafa_height = static_cast<int>(line_count);
    if (g_args.actual_chafa_height <= 0) { // Sanity check
        debug_output("Warning: Predetermined actual_chafa_height is " + std::to_string(g_args.actual_chafa_height) + ". Using height_arg as fallback.");
        g_args.actual_chafa_height = g_args.height_arg;
        if(g_args.actual_chafa_height <= 0) g_args.actual_chafa_height = 24; // Absolute fallback
    }
    debug_output("Predetermined actual Chafa height: " + std::to_string(g_args.actual_chafa_height));
    return true;
}

// After finishing its FFmpeg task, it joins the pool of Chafa converters.
void ffmpeg_then_chafa_worker(int worker_id, double start_time, double segment_duration,
                              const std::filesystem::path& output_dir) {
    // FFmpeg Task
    if (g_pipeline_error_occurred.load() || g_exit_requested.load()) {
        debug_output("Hybrid Worker " + std::to_string(worker_id) + ": Skipping FFmpeg task (pipeline error or exit requested).");
        g_ffmpeg_tasks_completed++; // Still need to count it as "done" to unblock the main thread.
        return; // Exit early if there's no point in doing work.
    }
    debug_output("Hybrid Worker " + std::to_string(worker_id) + ": Processing FFmpeg segment (start: " +
                  std::to_string(start_time) + "s, duration: " + std::to_string(segment_duration) + "s)");
    
    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);
    if(ec) {
        std::lock_guard<std::mutex> lock(g_cerr_mutex);
        std::println(stderr, "ERROR: Hybrid worker {} failed to create output directory {}: {}", worker_id, output_dir.string(), ec.message());
        g_pipeline_error_occurred.store(true);
        g_ffmpeg_tasks_completed++;
        return;
    }


    std::string ffmpeg_filter_complex = "fps=" + std::to_string(g_args.framerate);
    if (g_args.chroma_flag_given) {
        ffmpeg_filter_complex += ",format=rgba,colorkey=" + g_args.chroma_arg + ":similarity=0.01:blend=0";
    } else {
        ffmpeg_filter_complex += ",format=rgb24";
    }

    std::string ffmpeg_cmd = "ffmpeg -ss " + std::to_string(start_time) +
                             " -i \"" + g_args.filename + "\"" +
                             " -t " + std::to_string(segment_duration) +
                             " -vf \"" + ffmpeg_filter_complex + "\"" +
                             " -an -y \"" + (output_dir / "%09d.tiff").string() + "\"";

    if (run_command_silent_ex(ffmpeg_cmd, !g_args.debug) != 0) {
        g_pipeline_error_occurred.store(true); // Signal error
    }
    
    g_ffmpeg_tasks_completed++; // Signal that this FFmpeg task is done.
    debug_output("Hybrid Worker " + std::to_string(worker_id) + ": Finished FFmpeg task. Switching to ASCII conversion.");

    // FFmpeg threads become Chafa workers
    while (!g_exit_requested.load()) {
        if (g_pipeline_error_occurred.load() && g_ascii_conversion_queue.empty()) {
             break;
        }

        std::pair<std::filesystem::path, int> task;
        bool task_ready = false;
        {
            std::unique_lock<std::mutex> lock(g_conversion_queue_mutex);
            g_conversion_queue_cv.wait(lock, [] {
                return !g_ascii_conversion_queue.empty() || g_tiff_processing_done.load() || g_pipeline_error_occurred.load() || g_exit_requested.load();
            });

            if (g_exit_requested.load()) break;

            if (!g_ascii_conversion_queue.empty()) {
                task = g_ascii_conversion_queue.front();
                g_ascii_conversion_queue.pop();
                task_ready = true;
            } else if (g_tiff_processing_done.load() || g_pipeline_error_occurred.load()) {
                break;
            }
        }

        if (task_ready) {
            if (g_pipeline_error_occurred.load()) continue;

            const auto& tiff_file_path = task.first;
            std::string ascii_filename = tiff_file_path.stem().string() + ".txt";
            std::filesystem::path ascii_output_path = g_processed_ascii_path / ascii_filename;

            std::string chafa_cmd = g_args.build_chafa_command_string(g_args.actual_chafa_height) + " \"" + tiff_file_path.string() + "\"";
            std::string chafa_output_text = run_command_with_output_ex(chafa_cmd);

            if (!chafa_output_text.empty()) {
                if (FILE* ascii_file = std::fopen(ascii_output_path.c_str(), "w")) {
                    std::print(ascii_file, "{}", chafa_output_text);
                    std::fclose(ascii_file);
                    g_ascii_frames_completed.fetch_add(1);
                } else {
                    std::lock_guard<std::mutex> lock(g_cerr_mutex);
                    std::println(stderr, "ERROR: Hybrid Worker {} (as Chafa) failed to open output file: {}", worker_id, ascii_output_path.string());
                    g_pipeline_error_occurred.store(true);
                }
            } else {
                debug_output("WARNING: Hybrid Worker " + std::to_string(worker_id) + " (as Chafa) got empty output from Chafa for " + tiff_file_path.string());
                 // An empty output from Chafa after a successful pre-check often means the command itself is the issue.
                g_pipeline_error_occurred.store(true);
            }
        }
    }
    debug_output("Hybrid Worker " + std::to_string(worker_id) + ": Finished all work.");
}


// Dispatcher worker: monitors FFmpeg segment outputs, renames tiffs, and queues them for ASCII conversion
void prepare_tiff_frames(const std::vector<std::filesystem::path>& segment_dirs,
                        const std::vector<int>& segment_base_frame_indices) {
    if (g_pipeline_error_occurred.load() || g_exit_requested.load()) {
        debug_output("tiff Preparer: Skipping (pipeline error or exit requested).");
        g_tiff_processing_done.store(true); // Signal completion to allow Chafa workers to exit
        g_conversion_queue_cv.notify_all();
        return;
    }
    debug_output("tiff Preparer: Monitoring " + std::to_string(segment_dirs.size()) + " segment directories.");
    std::vector<int> next_tiff_idx_in_segment(segment_dirs.size(), 1); // Next local tiff

    bool work_possible = true;
    while (work_possible && !g_pipeline_error_occurred.load() && !g_exit_requested.load()) {
        bool file_processed_this_cycle = false;
        for (size_t i = 0; i < segment_dirs.size(); ++i) {
            if (g_pipeline_error_occurred.load() || g_exit_requested.load()) break; // Exit early on global error or exit request
            
            std::error_code exists_ec;
            if (!std::filesystem::exists(segment_dirs[i], exists_ec) && g_ffmpeg_extraction_done.load()) {
                 if(exists_ec) { /* directory might not exist yet, that's okay */ }
                 continue;
            }

            std::string source_tiff_name = std::format("{:09d}.tiff", next_tiff_idx_in_segment[i]);
            std::filesystem::path source_tiff_path = segment_dirs[i] / source_tiff_name;

            if (std::filesystem::exists(source_tiff_path, exists_ec) && !exists_ec) {
                int global_frame_num_0based = segment_base_frame_indices[i] + next_tiff_idx_in_segment[i] - 1;

                std::string final_tiff_name = std::format("{:09d}.tiff", global_frame_num_0based + 1);
                std::filesystem::path final_tiff_path = g_processed_tiff_path / final_tiff_name;
                
                std::error_code rename_ec;
                std::filesystem::rename(source_tiff_path, final_tiff_path, rename_ec);

                if (rename_ec) {
                    std::lock_guard<std::mutex> lock(g_cerr_mutex);
                    std::println(stderr, "ERROR: tiff Preparer failed to move {} to {}. What: {}", source_tiff_path.string(), final_tiff_path.string(), rename_ec.message());
                    next_tiff_idx_in_segment[i]++;
                } else {
                    {
                        std::lock_guard<std::mutex> lock(g_conversion_queue_mutex);
                        g_ascii_conversion_queue.push({final_tiff_path, global_frame_num_0based + 1});
                    }
                    g_conversion_queue_cv.notify_one();
                    g_tiffs_ready_for_ascii++;
                    next_tiff_idx_in_segment[i]++;
                    file_processed_this_cycle = true;
                }
            }
        }

        if (g_ffmpeg_extraction_done.load()) {
            bool all_segments_processed = true;
            for (size_t i = 0; i < segment_dirs.size(); ++i) {
                std::string next_tiff_name = std::format("{:09d}.tiff", next_tiff_idx_in_segment[i]);
                std::error_code ec;
                if (std::filesystem::exists(segment_dirs[i] / next_tiff_name, ec) && !ec) {
                    all_segments_processed = false;
                    break;
                }
            }
            if (all_segments_processed) {
                work_possible = false;
            }
        }

        if (!file_processed_this_cycle && work_possible && !g_pipeline_error_occurred.load() && !g_exit_requested.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
    }
    g_tiff_processing_done.store(true);
    g_conversion_queue_cv.notify_all();
    debug_output("tiff Preparer: Finished. Total tiffs queued: " + std::to_string(g_tiffs_ready_for_ascii.load()));
}


void convert_tiff_to_ascii(int worker_id) {
    debug_output("Dedicated ASCII Converter " + std::to_string(worker_id) + ": Started.");
    while (!g_exit_requested.load()) {
        if (g_pipeline_error_occurred.load() && g_ascii_conversion_queue.empty()) {
             break;
        }

        std::pair<std::filesystem::path, int> task;
        bool task_ready = false;
        {
            std::unique_lock<std::mutex> lock(g_conversion_queue_mutex);
            g_conversion_queue_cv.wait(lock, [] {
                return !g_ascii_conversion_queue.empty() || g_tiff_processing_done.load() || g_pipeline_error_occurred.load() || g_exit_requested.load();
            });

            if (g_exit_requested.load()) break;

            if (!g_ascii_conversion_queue.empty()) {
                task = g_ascii_conversion_queue.front();
                g_ascii_conversion_queue.pop();
                task_ready = true;
            } else if (g_tiff_processing_done.load() || g_pipeline_error_occurred.load()) {
                break;
            }
        }

        if (task_ready) {
            if (g_pipeline_error_occurred.load()) continue;

            const auto& tiff_file_path = task.first;
            std::string ascii_filename = tiff_file_path.stem().string() + ".txt";
            std::filesystem::path ascii_output_path = g_processed_ascii_path / ascii_filename;

            std::string chafa_cmd = g_args.build_chafa_command_string(g_args.actual_chafa_height) + " \"" + tiff_file_path.string() + "\"";
            std::string chafa_output_text = run_command_with_output_ex(chafa_cmd);

            if (!chafa_output_text.empty()) {
                if (FILE* ascii_file = std::fopen(ascii_output_path.c_str(), "w")) {
                    std::print(ascii_file, "{}", chafa_output_text);
                    std::fclose(ascii_file);
                    g_ascii_frames_completed.fetch_add(1);
                } else {
                    std::lock_guard<std::mutex> lock(g_cerr_mutex);
                    std::println(stderr, "ERROR: ASCII Converter {} failed to open output file: {}", worker_id, ascii_output_path.string());
                    g_pipeline_error_occurred.store(true);
                }
            } else {
                debug_output("WARNING: ASCII Converter " + std::to_string(worker_id) + " got empty output from Chafa for " + tiff_file_path.string());
                g_pipeline_error_occurred.store(true);
            }
        }
    }
    debug_output("Dedicated ASCII Converter " + std::to_string(worker_id) + ": Finished.");
}

// Prepare all animation assets
void prepare_animation_assets() {
    std::filesystem::path input_file_path_obj(g_args.filename);
    std::error_code ec;
    
    // Resolve absolute path
    auto abs_path = std::filesystem::absolute(input_file_path_obj, ec);
    if (ec) {
        std::lock_guard<std::mutex> lock(g_cerr_mutex);
        std::println(stderr, "ERROR: Could not resolve absolute path for input file '{}': {}", g_args.filename, ec.message());
        exit(1);
    }
    g_args.filename = abs_path.string();
    input_file_path_obj = g_args.filename;


    std::filesystem::path project_root_dir = std::filesystem::current_path();
    std::filesystem::path base_cache_dir = project_root_dir / ".cache";

    // Create base cache directory
    if (!std::filesystem::exists(base_cache_dir, ec) && !ec) {
        std::filesystem::create_directories(base_cache_dir, ec);
        if (ec) {
            std::lock_guard<std::mutex> lock(g_cerr_mutex);
            std::println(stderr, "ERROR: Could not create base cache directory '{}': {}", base_cache_dir.string(), ec.message());
            exit(1);
        }
        debug_output("Created base cache directory: " + base_cache_dir.string());
    }
    
    g_video_specific_cache_root = base_cache_dir / input_file_path_obj.filename();
    std::string current_args_hash = hash_args_map(g_args.to_input_map());
    g_current_args_cache_dir = g_video_specific_cache_root / current_args_hash;
    g_current_cache_metadata_file = g_current_args_cache_dir / "cache.txt";
    g_processed_ascii_path = g_current_args_cache_dir / "ascii_art";
    double video_file_duration = 0.0;
    
    if (std::filesystem::exists(g_current_cache_metadata_file, ec) && !ec) {
        debug_output("Found existing cache metadata: " + g_current_cache_metadata_file.string());
        std::map<std::string, std::string> cached_args_map = parse_cache_txt(g_current_cache_metadata_file);
        std::map<std::string, std::string> current_input_args_map = g_args.to_input_map();
        bool cache_is_valid = true;

        for (const auto& current_pair : current_input_args_map) {
            if (cached_args_map.find(current_pair.first) == cached_args_map.end() || cached_args_map[current_pair.first] != current_pair.second) {
                debug_output("Cache invalid: Parameter mismatch for key '" + current_pair.first + "'. Current: '" + current_pair.second + "', Cached: '" + (cached_args_map.count(current_pair.first) ? cached_args_map[current_pair.first] : "N/A") + "'");
                cache_is_valid = false;
                break;
            }
        }

        if (cache_is_valid) {
            if (auto it = cached_args_map.find("actual_chafa_height"); it != cached_args_map.end()) { std::from_chars(it->second.data(), it->second.data() + it->second.size(), g_args.actual_chafa_height); } else { cache_is_valid = false; debug_output("Cache invalid: actual_chafa_height missing."); }
            if (auto it = cached_args_map.find("num_frames"); it != cached_args_map.end()) { std::from_chars(it->second.data(), it->second.data() + it->second.size(), g_args.num_frames); } else { cache_is_valid = false; debug_output("Cache invalid: num_frames missing."); }
            if (auto it = cached_args_map.find("sound_saved_path"); it != cached_args_map.end()) { g_args.sound_saved_path = it->second; }
            if (auto it = cached_args_map.find("video_duration_cached"); it != cached_args_map.end()) { std::from_chars(it->second.data(), it->second.data() + it->second.size(), video_file_duration); }

            // Integrity check
            std::filesystem::path data_file = g_current_args_cache_dir / "frames.dat";
            std::filesystem::path index_file = g_current_args_cache_dir / "frames.idx";
            
            unsigned long long expected_data_size = 0;
            if (auto it = cached_args_map.find("frames_dat_size"); it != cached_args_map.end()) {
                std::from_chars(it->second.data(), it->second.data() + it->second.size(), expected_data_size);
            } else {
                debug_output("Cache invalid: 'frames_dat_size' missing from cache.txt.");
                cache_is_valid = false;
            }
            
            ec.clear();
            if (cache_is_valid && (!std::filesystem::exists(data_file, ec) || ec || !std::filesystem::exists(index_file, ec) || ec)) {
                debug_output("Cache invalid: Packed data file 'frames.dat' or 'frames.idx' is missing.");
                cache_is_valid = false;
            }

            if (cache_is_valid) {
                ec.clear();
                unsigned long long actual_data_size = std::filesystem::file_size(data_file, ec);
                if (ec) {
                    debug_output("Cache invalid: Filesystem error during integrity check: " + ec.message());
                    cache_is_valid = false;
                } else {
                    if (actual_data_size != expected_data_size) {
                        debug_output("Cache invalid: 'frames.dat' size mismatch. Expected: " + std::to_string(expected_data_size) + ", Found: " + std::to_string(actual_data_size));
                        cache_is_valid = false;
                    }
                }
                
                ec.clear();
                size_t index_file_size = std::filesystem::file_size(index_file, ec);
                if (ec) {
                     debug_output("Cache invalid: Filesystem error during integrity check: " + ec.message());
                     cache_is_valid = false;
                }

                if (cache_is_valid) {
                    size_t frames_in_index = index_file_size / sizeof(FrameIndexEntry);
                    if (frames_in_index != static_cast<size_t>(g_args.num_frames)) {
                        debug_output("Cache invalid: Mismatch between frame count in cache.txt (" + std::to_string(g_args.num_frames) + ") and frames.idx (" + std::to_string(frames_in_index) + ").");
                        cache_is_valid = false;
                    } else if (frames_in_index > 0) {
                        std::ifstream index_stream(index_file, std::ios::binary);
                        index_stream.seekg(-static_cast<std::streamoff>(sizeof(FrameIndexEntry)), std::ios::end);
                        FrameIndexEntry last_entry;
                        index_stream.read(reinterpret_cast<char*>(&last_entry), sizeof(last_entry));
                        
                        unsigned long long size_from_index = last_entry.offset + last_entry.length;
                        if (size_from_index != actual_data_size) {
                            debug_output("Cache invalid: 'frames.dat' is corrupt or truncated. Index implies size " + std::to_string(size_from_index) + ", but actual size is " + std::to_string(actual_data_size) + ".");
                            cache_is_valid = false;
                        }
                    }
                }
            }
            ec.clear();
            if (cache_is_valid && !g_args.sound_saved_path.empty() && !std::filesystem::exists(g_args.sound_saved_path, ec) && !ec) {
                 debug_output("Cache invalid: Cached sound path missing: " + g_args.sound_saved_path);
                 cache_is_valid = false;
            }
        }

        if (cache_is_valid) {
            debug_output("Cache is valid and will be used.");
            return;
        } else {
            debug_output("Cache is invalid or incomplete. Re-rendering.");
        }
    }
    
    std::print("Caching...");
    std::fflush(stdout);

    g_pipeline_error_occurred.store(false);
    g_ffmpeg_extraction_done.store(false);
    g_tiff_processing_done.store(false);
    g_tiffs_ready_for_ascii.store(0);
    g_ascii_frames_completed.store(0);
    while(!g_ascii_conversion_queue.empty()) g_ascii_conversion_queue.pop();
    
    ec.clear();
    if (std::filesystem::exists(g_current_args_cache_dir, ec) && !ec) {
         std::filesystem::remove_all(g_current_args_cache_dir, ec);
    }
    ec.clear();
    std::filesystem::create_directories(g_current_args_cache_dir, ec);
    if(ec) { std::println(stderr, "Fatal: Could not create cache directory. {}", ec.message()); exit(1); }
    
    g_processed_tiff_path = g_current_args_cache_dir / "final_tiffs";
    g_temp_tiff_segments_path = g_current_args_cache_dir / "temp_tiff_segments";
    ec.clear();
    std::filesystem::create_directories(g_processed_tiff_path, ec);
    if(ec) { std::println(stderr, "Fatal: Could not create tiff directory. {}", ec.message()); exit(1); }
    ec.clear();
    std::filesystem::create_directories(g_temp_tiff_segments_path, ec);
    if(ec) { std::println(stderr, "Fatal: Could not create temp tiff directory. {}", ec.message()); exit(1); }
    ec.clear();
    std::filesystem::create_directories(g_processed_ascii_path, ec);
    if(ec) { std::println(stderr, "Fatal: Could not create ascii directory. {}", ec.message()); exit(1); }


    g_args.sound_saved_path.clear();
    if (g_args.sound_flag_given) {
        if (!g_args.sound_arg.empty()) {
            std::filesystem::path src_audio_path = g_args.sound_arg;
            std::filesystem::path dest_audio_path = g_current_args_cache_dir / src_audio_path.filename();
            ec.clear();
            bool src_exists = std::filesystem::exists(src_audio_path, ec) && !ec && std::filesystem::is_regular_file(src_audio_path, ec) && !ec;
            if (src_exists) {
                ec.clear();
                std::filesystem::copy(src_audio_path, dest_audio_path, std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) {
                     std::lock_guard<std::mutex> lock(g_cerr_mutex); std::println(stderr, "Error copying audio: {}", ec.message());
                } else {
                     g_args.sound_saved_path = dest_audio_path.string();
                }
            } else {
                std::lock_guard<std::mutex> lock(g_cerr_mutex); std::println(stderr, "Error: Provided sound file not found or not a file: {}", src_audio_path.string());
            }
        } else {
            g_args.sound_saved_path = extract_audio_from_file(g_args.filename, g_current_args_cache_dir);
            if (g_args.sound_saved_path.empty()) {
                debug_output("No audio stream found in " + g_args.filename + " or extraction failed.");
            }
        }
    }

    if (!predetermine_actual_chafa_height() && g_pipeline_error_occurred.load()) {
         std::println(stderr, "\nCRITICAL: Failed to predetermine Chafa output height. Aborting render pipeline.");
         ec.clear();
         if (std::filesystem::exists(g_current_args_cache_dir, ec)) std::filesystem::remove_all(g_current_args_cache_dir, ec);
         exit(1);
    }
    if (g_exit_requested.load()) return;

    if (video_file_duration <= 0.01) {
        video_file_duration = get_video_duration_ex(g_args.filename);
    }
    if (video_file_duration <= 0.01) {
        std::lock_guard<std::mutex> lock(g_cerr_mutex); std::println(stderr, "ERROR: Video duration too short or invalid ({}s). Aborting.", video_file_duration);
        ec.clear();
        if (std::filesystem::exists(g_current_args_cache_dir, ec)) std::filesystem::remove_all(g_current_args_cache_dir, ec);
        exit(1);
    }

    g_ffmpeg_tasks_completed.store(0); // Reset the counter for this run

    unsigned int num_hw_threads = std::thread::hardware_concurrency();
    if (num_hw_threads == 0) num_hw_threads = 4; // Sensible default

    // Decide how many threads get an initial FFmpeg task.
    unsigned int num_ffmpeg_workers = std::max(1u, num_hw_threads / 2); // Up to half the cores assigned to FFmpeg initially.
    // Limit the number of FFmpeg workers if the video is very short
    num_ffmpeg_workers = std::min(num_ffmpeg_workers, static_cast<unsigned int>(std::ceil(video_file_duration / 1.0)));
    num_ffmpeg_workers = std::max(1u, num_ffmpeg_workers);
    
    // The rest of the threads will start as dedicated Chafa converters.
    unsigned int num_dedicated_chafa_workers = (num_hw_threads > num_ffmpeg_workers) ? (num_hw_threads - num_ffmpeg_workers) : 0;
    
    debug_output(std::format("Thread strategy: {} hybrid FFmpeg->Chafa workers, {} dedicated Chafa workers.", num_ffmpeg_workers, num_dedicated_chafa_workers));

    double segment_len_nominal = video_file_duration / num_ffmpeg_workers;

    std::vector<std::filesystem::path> temp_segment_dirs;
    std::vector<int> segment_start_frame_indices;
    int current_ideal_frame_offset = 0;
    
    std::vector<std::thread> all_workers;
    all_workers.reserve(num_hw_threads);

    // Launch the hybrid workers (FFmpeg task first)
    for (unsigned int i = 0; i < num_ffmpeg_workers; ++i) {
        if (g_exit_requested.load()) break;
        double seg_start_time = i * segment_len_nominal;
        double seg_duration = (i == num_ffmpeg_workers - 1) ? (video_file_duration - seg_start_time) : segment_len_nominal;
        if (seg_duration <= 0) continue;

        std::filesystem::path segment_output_path = g_temp_tiff_segments_path / ("segment_" + std::to_string(i));
        temp_segment_dirs.push_back(segment_output_path);
        segment_start_frame_indices.push_back(current_ideal_frame_offset);
        
        // Use unique worker ID for logging
        int worker_id = i;
        all_workers.emplace_back(ffmpeg_then_chafa_worker, worker_id, seg_start_time, seg_duration, segment_output_path);
        
        current_ideal_frame_offset += static_cast<int>(std::round(seg_duration * g_args.framerate));
    }
    const unsigned int num_ffmpeg_workers_launched = all_workers.size();

    // Launch the dedicated Chafa workers
    for (unsigned int i = 0; i < num_dedicated_chafa_workers; ++i) {
        if (g_exit_requested.load()) break;
        // Use unique worker ID for logging
        int worker_id = num_ffmpeg_workers_launched + i;
        all_workers.emplace_back(convert_tiff_to_ascii, worker_id);
    }

    // The tiff preparer thread is still essential and runs separately
    std::thread tiff_preparer_thread(prepare_tiff_frames, temp_segment_dirs, segment_start_frame_indices);
    
    // Wait for all FFmpeg tasks to complete before signaling the preparer.
    debug_output("Main thread waiting for " + std::to_string(num_ffmpeg_workers_launched) + " FFmpeg tasks to complete...");
    while (g_ffmpeg_tasks_completed.load() < num_ffmpeg_workers_launched && !g_pipeline_error_occurred.load() && !g_exit_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    
    if (!g_pipeline_error_occurred.load() && !g_exit_requested.load()) {
        debug_output("All FFmpeg tasks are complete.");
    }
    g_ffmpeg_extraction_done.store(true);
    g_conversion_queue_cv.notify_all(); // Wake up any waiting threads just in case

    // Now, wait for the tiff preparer to finish its job.
    if (tiff_preparer_thread.joinable()) tiff_preparer_thread.join();
    g_conversion_queue_cv.notify_all(); // Final notification for Chafa workers

    // Finally, wait for all workers (hybrid and dedicated) to finish their Chafa tasks.
    debug_output("Main thread joining all " + std::to_string(all_workers.size()) + " worker threads...");
    for (auto& th : all_workers) {
        if (th.joinable()) th.join();
    }
    debug_output("All worker threads joined.");
    
    if (g_exit_requested.load()) return;

    if (g_pipeline_error_occurred.load()) {
        std::lock_guard<std::mutex> lock(g_cerr_mutex); std::println(stderr, "\nERROR: Asset pipeline failed. Cleaning up current hash directory.");
        ec.clear();
        if (std::filesystem::exists(g_current_args_cache_dir, ec)) std::filesystem::remove_all(g_current_args_cache_dir, ec);
        exit(1);
    }

    g_args.num_frames = g_ascii_frames_completed.load();

    if (g_args.num_frames == 0 && video_file_duration > 0.1) {
        std::lock_guard<std::mutex> lock(g_cerr_mutex); std::println(stderr, "\nERROR: Asset generation resulted in 0 frames. Check FFmpeg/Chafa output and arguments.");
        ec.clear();
        if (std::filesystem::exists(g_current_args_cache_dir, ec)) std::filesystem::remove_all(g_current_args_cache_dir, ec);
        exit(1);
    }

    debug_output("Consolidating " + std::to_string(g_args.num_frames) + " ASCII frames into packed files...");
    std::filesystem::path data_file_path = g_current_args_cache_dir / "frames.dat";
    std::filesystem::path index_file_path = g_current_args_cache_dir / "frames.idx";
    
    std::vector<std::filesystem::path> ascii_files;
    ec.clear();
    if (std::filesystem::exists(g_processed_ascii_path, ec)) {
        for (const auto& entry : std::filesystem::directory_iterator(g_processed_ascii_path, ec)) {
            if (entry.is_regular_file(ec) && !ec && entry.path().extension() == ".txt") {
                ascii_files.push_back(entry.path());
            }
        }
    }
    std::sort(ascii_files.begin(), ascii_files.end());

    if (ascii_files.size() != static_cast<size_t>(g_args.num_frames)) {
        debug_output("Warning: Mismatch between files on disk (" + std::to_string(ascii_files.size()) + ") and completed frame count (" + std::to_string(g_args.num_frames) + "). Using disk count.");
        g_args.num_frames = ascii_files.size();
    }
    
    std::ofstream data_file(data_file_path, std::ios::binary);
    std::ofstream index_file(index_file_path, std::ios::binary);
    if (!data_file.is_open() || !index_file.is_open()) {
        std::lock_guard<std::mutex> lock(g_cerr_mutex);
        std::println(stderr, "ERROR: Could not open packed cache files frames.dat/frames.idx for writing.");
        ec.clear();
        if (std::filesystem::exists(g_current_args_cache_dir, ec)) std::filesystem::remove_all(g_current_args_cache_dir, ec);
        exit(1);
    }

    unsigned long long current_offset = 0;
    for (const auto& txt_path : ascii_files) {
        std::ifstream src_file(txt_path, std::ios::binary | std::ios::ate);
        if (!src_file.is_open()) continue;
        
        std::streamsize length = src_file.tellg();
        src_file.seekg(0, std::ios::beg);

        FrameIndexEntry entry = {current_offset, static_cast<unsigned long long>(length)};
        index_file.write(reinterpret_cast<const char*>(&entry), sizeof(entry));
        
        data_file << src_file.rdbuf();
        current_offset += length;
    }
    data_file.close();
    index_file.close();
    debug_output("Consolidation complete.");
    
    ec.clear();
    if (std::filesystem::exists(g_temp_tiff_segments_path, ec)) std::filesystem::remove_all(g_temp_tiff_segments_path, ec);
    ec.clear();
    if (std::filesystem::exists(g_processed_tiff_path, ec)) std::filesystem::remove_all(g_processed_tiff_path, ec);
    ec.clear();
    if (std::filesystem::exists(g_processed_ascii_path, ec)) std::filesystem::remove_all(g_processed_ascii_path, ec);
    debug_output("Cleaned up temporary generation directories.");

    // Metadata write
    if (FILE* cache_file = std::fopen(g_current_cache_metadata_file.c_str(), "w")) {
        std::map<std::string, std::string> data_to_cache = g_args.to_cache_map(video_file_duration);
        // Update the map with the actual, final size of the data file.
        data_to_cache["frames_dat_size"] = std::to_string(current_offset);
        
        for (const auto& cache_pair : data_to_cache) {
            std::println(cache_file, "{}={}", cache_pair.first, cache_pair.second);
        }
        std::fclose(cache_file);
    } else {
        std::lock_guard<std::mutex> lock(g_cerr_mutex);
        std::println(stderr, "ERROR: Failed to write cache metadata: {}", g_current_cache_metadata_file.string());
    }
}

// Argument Parsing & UI Functions
const std::set<std::string_view> CHAFA_VALUELESS_FLAGS = {
    "--fg-only", "--bg-only", "--invert", "--clear", "--version", "--help",
    "--stretch", "--watch" // Note: --watch is nonsensical here but is a valid flag
};

void parse_arguments(int argc, char* argv[]) {
    std::vector<std::string_view> args(argv + 1, argv + argc);

    for (size_t i = 0; i < args.size(); ++i) {
        std::string_view arg = args[i];

        if (arg == "--file") {
            if (i + 1 < args.size()) { g_args.filename = args[++i]; } 
            else { std::println(stderr, "Error: --file requires an argument."); exit(1); }
        } else if (arg == "--size") {
            if (i + 1 < args.size()) {
                std::string_view val = args[++i];
                size_t x_pos = val.find('x');
                if (x_pos != std::string_view::npos) {
                    std::from_chars(val.data(), val.data() + x_pos, g_args.width);
                    std::from_chars(val.data() + x_pos + 1, val.data() + val.size(), g_args.height_arg);
                } else { std::println(stderr, "Error: --size requires widthxheight format (e.g., 80x24)."); exit(1); }
            } else { std::println(stderr, "Error: --size requires an argument."); exit(1); }
        } else if (arg == "--debug") {
            g_args.debug = true;
        } else if (arg == "--framerate") {
            if (i + 1 < args.size()) { std::string_view val = args[++i]; std::from_chars(val.data(), val.data() + val.size(), g_args.framerate); } 
            else { std::println(stderr, "Error: --framerate requires an argument."); exit(1); }
        } else if (arg == "--sound") {
            g_args.sound_flag_given = true;
            if (i + 1 < args.size() && !args[i+1].starts_with('-')) { g_args.sound_arg = args[++i]; } 
            else { g_args.sound_arg = ""; }
        } else if (arg == "--chroma") {
            g_args.chroma_flag_given = true;
            if (i + 1 < args.size() && !args[i+1].starts_with('-')) { g_args.chroma_arg = args[++i]; } 
            else { std::println(stderr, "Error: --chroma requires a hex color argument (e.g., 0x00FF00)."); exit(1); }
        } else if (arg.starts_with("--")) {
            // Assume it's a Chafa argument.
            if (arg == "--size" || arg == "--format") { 
                debug_output("Ignoring chafa argument '" + std::string(arg) + "' as it is managed by anifetch.");
                // Skip the value if there is one
                if (i + 1 < args.size() && !args[i+1].starts_with('-') && !CHAFA_VALUELESS_FLAGS.count(arg)) {
                    i++;
                }
                continue; 
            }

            // Check if it's a flag that doesn't take a value.
            if (CHAFA_VALUELESS_FLAGS.count(arg)) {
                g_args.chafa_options[std::string(arg)] = "";
            } else if (i + 1 < args.size() && !args[i+1].starts_with('-')) {
                // It's a flag that takes a value.
                g_args.chafa_options[std::string(arg)] = args[++i];
            } else {
                // It's a flag that might not have a value, or the user made a mistake. Passed to Chafa.
                g_args.chafa_options[std::string(arg)] = "";
            }
        }
        else {
             std::println(stderr, "Unknown argument or invalid format: {}", arg); exit(1);
        }
    }

    if (g_args.filename.empty()) { std::println(stderr, "Filename required (--file <path>)."); exit(1); }
    if (g_args.chroma_flag_given && !std::string_view(g_args.chroma_arg).starts_with("0x")) { std::println(stderr, "Chroma hex needs '0x' prefix (e.g., 0x00FF00)."); exit(1); }
    if (g_args.width <= 0) { std::println(stderr, "Error: --size width dimension must be positive."); exit(1); }
    if (g_args.height_arg <= 0) { std::println(stderr, "Error: --size height dimension must be positive."); exit(1); }
    if (g_args.framerate <= 0) { std::println(stderr, "Error: --framerate must be positive."); exit(1); }
}

void clear_screen() { std::print("\033[H\033[2J"); std::fflush(stdout); }
void move_cursor(int row, int col) { std::print("\033[{};{}H", row, col); std::fflush(stdout); }
void hide_cursor() { std::print("\033[?25l"); std::fflush(stdout); }
void show_cursor() { std::print("\033[?25h"); std::fflush(stdout); }

void cleanup_on_exit() {
    show_cursor();
    if (g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_original_termios);
        debug_output("Restored terminal settings on exit.");
    }
    if (g_ffplay_pid > 0) {
        debug_output("Cleanup on exit: Terminating ffplay PID: " + std::to_string(g_ffplay_pid));
        kill(g_ffplay_pid, SIGTERM);
        int ffplay_status;
        for(int i=0; i < 10; ++i) {
             if (waitpid(g_ffplay_pid, &ffplay_status, WNOHANG) != 0) {
                g_ffplay_pid = -1; break;
             }
             std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        if (g_ffplay_pid > 0) {
            debug_output("ffplay did not exit gracefully, sending SIGKILL.");
            kill(g_ffplay_pid, SIGKILL);
            waitpid(g_ffplay_pid, nullptr, 0);
        }
        g_ffplay_pid = -1;
    }
    struct winsize term_size_cleanup;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &term_size_cleanup) == 0 && term_size_cleanup.ws_row > 0) {
        std::print("\033[{};{}H", term_size_cleanup.ws_row, 1);
    }
    std::fflush(stdout);
}

// Signal Handler
void signal_handler_safe(int signal_num) {
    (void)signal_num; 
    g_exit_requested.store(true);
    g_conversion_queue_cv.notify_all();
}

void generate_static_template() {
    std::vector<std::string> info_lines;
    std::string fetch_command = "fastfetch --logo none --pipe false";
    std::string fetch_output_str = run_command_with_output_ex(fetch_command);

    bool fastfetch_likely_missing = fetch_output_str.empty() &&
                                    !std::filesystem::exists("/usr/bin/fastfetch") &&
                                    !std::filesystem::exists("/bin/fastfetch") &&
                                    !std::filesystem::exists("/usr/local/bin/fastfetch");

    if (fastfetch_likely_missing) {
        std::println(stderr, "Warning: Could not run fastfetch or it produced no output. Static info will be minimal.");
    }

    std::istringstream output_stream(fetch_output_str);
    std::string line_buffer;
    while(std::getline(output_stream, line_buffer)) {
        info_lines.push_back(line_buffer);
    }

    if (info_lines.empty()){
        int fallback_lines = (g_args.actual_chafa_height > 0) ? g_args.actual_chafa_height : g_args.height_arg;
        for(int i=0; i < fallback_lines; ++i) info_lines.push_back(" ");
    }

    struct winsize term_size_display;
    int current_terminal_width = 80;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &term_size_display) == 0) {
        current_terminal_width = term_size_display.ws_col;
    }

    const int TEMPLATE_GAP = 2;
    const int TEMPLATE_PAD_LEFT = 4;
    const int ANIM_AREA_WIDTH = g_args.width;
    int effective_animation_height = (g_args.actual_chafa_height > 0) ? g_args.actual_chafa_height : g_args.height_arg;
    if (effective_animation_height <=0) effective_animation_height = 24;

    std::vector<std::string> final_template_lines;
    for (int y = 0; y < std::max(effective_animation_height, static_cast<int>(info_lines.size())); ++y) {
        std::string current_info_line = (static_cast<size_t>(y) < info_lines.size()) ? info_lines[y] : "";
        std::string anim_placeholder = std::string(ANIM_AREA_WIDTH, ' ');

        int info_area_start_col = TEMPLATE_PAD_LEFT + ANIM_AREA_WIDTH + TEMPLATE_GAP;
        int available_width_for_info = current_terminal_width - info_area_start_col;
        std::string info_segment_for_line;

        if (available_width_for_info > 0) {
            info_segment_for_line = current_info_line;
            info_segment_for_line.resize(available_width_for_info, ' ');
        } else {
            info_segment_for_line = "";
        }

        std::string full_line = std::string(TEMPLATE_PAD_LEFT, ' ') + anim_placeholder + std::string(TEMPLATE_GAP, ' ') + info_segment_for_line;
        full_line.resize(current_terminal_width, ' ');
        final_template_lines.push_back(full_line + "\n");
    }

    std::error_code ec;
    if (!std::filesystem::exists(g_video_specific_cache_root, ec) && !ec) {
        std::filesystem::create_directories(g_video_specific_cache_root, ec);
        if (ec) {
             std::println(stderr, "Error: Could not create directory for template.txt: {}: {}", g_video_specific_cache_root.string(), ec.message());
        } else {
             debug_output("Created video-specific cache root for template: " + g_video_specific_cache_root.string());
        }
    }


    std::filesystem::path template_file_path = g_video_specific_cache_root / "template.txt";
    if (FILE* template_output_file = std::fopen(template_file_path.c_str(), "w")) {
        for (const auto& template_line : final_template_lines) {
            std::print(template_output_file, "{}", template_line);
        }
        std::fclose(template_output_file);
        debug_output("Static template written to: " + template_file_path.string());
    } else {
        std::println(stderr, "Error: Could not write template.txt to {}", template_file_path.string());
    }
}

void run_animation_loop() {
    hide_cursor();

    const int ANIM_PAD_LEFT = 4;
    const int ANIM_FRAME_WIDTH = g_args.width;
    const int SCREEN_TOP_PADDING = 2;
    const int ANIM_START_COL = ANIM_PAD_LEFT + 1;
    int anim_display_height = (g_args.actual_chafa_height > 0) ? g_args.actual_chafa_height : g_args.height_arg;
    if (anim_display_height <= 0) anim_display_height = 24;

    clear_screen();
    for (int i = 0; i < SCREEN_TOP_PADDING; ++i) std::print("\n");

    std::filesystem::path static_template_path = g_video_specific_cache_root / "template.txt";
    std::error_code ec;
    if (std::filesystem::exists(static_template_path, ec) && !ec) {
        std::ifstream template_input_stream(static_template_path);
        if (template_input_stream.is_open()) {
            std::string template_line_content;
            int template_display_row = SCREEN_TOP_PADDING + 1;
            while (std::getline(template_input_stream, template_line_content)) {
                 move_cursor(template_display_row++, 1);
                 if (!template_line_content.empty() && template_line_content.back() == '\n') {
                    template_line_content.pop_back();
                 }
                 std::print("{}", template_line_content);
            }
        } else { std::println(stderr, "Warning: template.txt found but could not be opened."); }
    } else { std::println(stderr, "Warning: template.txt not found. Static info will be missing."); }
    std::fflush(stdout);
    
    std::filesystem::path data_file_path = g_current_args_cache_dir / "frames.dat";
    std::filesystem::path index_file_path = g_current_args_cache_dir / "frames.idx";
    
    ec.clear();
    if (!std::filesystem::exists(data_file_path, ec) || ec || !std::filesystem::exists(index_file_path, ec) || ec) {
        std::println(stderr, "\nError: Packed animation files (frames.dat/idx) not found. Re-run to generate them.");
        show_cursor();
        std::exit(1);
    }
    
    std::vector<FrameIndexEntry> frame_index;
    std::ifstream index_stream(index_file_path, std::ios::binary | std::ios::ate);
    if (!index_stream) { std::println(stderr, "\nError: Could not open index file."); show_cursor(); std::exit(1); }
    size_t file_size = index_stream.tellg();
    index_stream.seekg(0, std::ios::beg);
    size_t num_frames_in_index = file_size / sizeof(FrameIndexEntry);
    frame_index.resize(num_frames_in_index);
    index_stream.read(reinterpret_cast<char*>(frame_index.data()), file_size);
    index_stream.close();

    if (frame_index.empty()) {
        std::println("\nNo animation frames found in index. Check input video or cache.");
        show_cursor();
        std::exit(1);
    }
    
    std::ifstream data_stream(data_file_path, std::ios::binary);
    if (!data_stream.is_open()) {
        std::println(stderr, "\nError: Could not open frame data file: {}", data_file_path.string());
        show_cursor();
        std::exit(1);
    }
    
    std::string frame_buffer;
    
    while (!g_exit_requested.load()) {
        ec.clear();
        if (g_args.sound_flag_given && !g_args.sound_saved_path.empty() && std::filesystem::exists(g_args.sound_saved_path, ec) && !ec) {
            g_ffplay_pid = fork();
            if (g_ffplay_pid == 0) {
                if (freopen("/dev/null", "r", stdin) == nullptr || freopen("/dev/null", "w", stdout) == nullptr || freopen("/dev/null", "w", stderr) == nullptr) _exit(127);
                execlp("ffplay", "ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet", g_args.sound_saved_path.c_str(), static_cast<char*>(nullptr));
                _exit(127);
            } else if (g_ffplay_pid < 0) {
                std::println(stderr, "\nFailed to fork for ffplay. Audio will not play.");
                g_ffplay_pid = -1;
            }
        }

        double effective_framerate = g_args.framerate;

        auto animation_start_time = std::chrono::high_resolution_clock::now();
        for (size_t frame_idx = 0; frame_idx < frame_index.size() && !g_exit_requested.load(); ++frame_idx) {
            
            const auto& entry = frame_index[frame_idx];
            frame_buffer.resize(entry.length);
            data_stream.seekg(entry.offset);
            data_stream.read(&frame_buffer[0], entry.length);

            std::stringstream frame_data_stream(frame_buffer);
            int screen_row = SCREEN_TOP_PADDING + 1;
            std::string line;
            int lines_drawn = 0;

            while (std::getline(frame_data_stream, line) && lines_drawn < anim_display_height) {
                move_cursor(screen_row++, ANIM_START_COL);
                std::print("{}", line);
                lines_drawn++;
            }
            while (lines_drawn++ < anim_display_height) {
                move_cursor(screen_row++, ANIM_START_COL);
                std::print("{}", std::string(ANIM_FRAME_WIDTH, ' '));
            }
            hide_cursor();

            auto target_time = animation_start_time + std::chrono::duration<double>((frame_idx + 1) / effective_framerate);
            auto current_time = std::chrono::high_resolution_clock::now();
            if (target_time > current_time) {
                std::this_thread::sleep_until(target_time);
            }
        }

        if (g_ffplay_pid > 0) {
            int ffplay_status;
            while (waitpid(g_ffplay_pid, &ffplay_status, WNOHANG) == 0) {
                if (g_exit_requested.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            g_ffplay_pid = -1; 
        }

        if (g_exit_requested.load()) break;
    }
    std::print("\n");
}

int main(int argc, char* argv[]) {
    if (isatty(STDIN_FILENO)) {
        if (tcgetattr(STDIN_FILENO, &g_original_termios) == 0) {
            g_termios_saved = true;
        } else {
            perror("Warning: tcgetattr failed");
        }
    }

    if (std::atexit(cleanup_on_exit) != 0) {
        std::println(stderr, "Warning: Failed to register atexit cleanup function.");
    }

    signal(SIGINT, signal_handler_safe);
    signal(SIGTERM, signal_handler_safe);

    parse_arguments(argc, argv);
    if (g_termios_saved) debug_output("Original terminal settings saved.");

    std::error_code ec;
    if (!std::filesystem::exists(g_args.filename, ec) || ec) {
        std::println(stderr, "Error: Input file '{}' not found.", g_args.filename);
        return 1;
    }
    ec.clear();
    if (!std::filesystem::is_regular_file(g_args.filename, ec) || ec) {
        std::println(stderr, "Error: Input '{}' is not a regular file.", g_args.filename);
        return 1;
    }

    g_args.actual_chafa_height = g_args.height_arg; 

    prepare_animation_assets();
    
    if (g_exit_requested.load()) {
        std::print("\r{}\r", std::string(40, ' '));
        return 0;
    }

    if (g_args.actual_chafa_height <= 0) {
        debug_output("Warning: actual_chafa_height is still invalid (" + std::to_string(g_args.actual_chafa_height) + ") after asset preparation. Using height_arg as fallback.");
        g_args.actual_chafa_height = g_args.height_arg;
        if (g_args.actual_chafa_height <= 0) {
             g_args.actual_chafa_height = 24; 
             debug_output("Warning: height_arg also invalid. Using default height of 24.");
        }
    }

    generate_static_template();

    if (g_exit_requested.load()) {
        return 0;
    }

    run_animation_loop();

    return 0;
}
