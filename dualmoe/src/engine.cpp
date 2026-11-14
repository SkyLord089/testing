#include "engine.h"

#include "autobudget.h"

#include "chat.h"
#include "common.h"
#include "ggml-backend.h"
#include "ggml-cpu.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace dualmoe {

namespace {

// Lock-free crash-handler state (safe inside Windows SEH & ggml_set_abort_callback)
std::atomic<DualMoeEngine *> g_active_engine{nullptr};
#if defined(_WIN32)
wchar_t g_crash_wpath_buf[1024]  = L"dualmoe-crash.txt";
wchar_t g_status_wpath_buf[1024] = L"dualmoe-load.status";
#endif
char g_crash_file_buf[1024]  = "dualmoe-crash.txt";
char g_status_file_buf[1024] = "dualmoe-load.status";
char g_active_stage_buf[512] = "Инициализация движка";

std::string fs_path_to_utf8(const std::filesystem::path & p) {
    const auto u8 = p.u8string();
    return std::string(reinterpret_cast<const char *>(u8.data()), u8.size());
}

FILE * open_utf8_file(const std::string & utf8_path, const char * mode) {
#if defined(_WIN32)
    const std::filesystem::path p = std::filesystem::u8path(utf8_path);
    std::wstring wmode;
    for (const char * c = mode; c && *c; ++c) wmode.push_back((wchar_t) *c);
    return _wfopen(p.wstring().c_str(), wmode.c_str());
#else
    return std::fopen(utf8_path.c_str(), mode);
#endif
}

std::string now_time_hhmmss() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_val{};
#if defined(_WIN32)
    localtime_s(&tm_val, &tt);
#else
    localtime_r(&tt, &tm_val);
#endif
    char buf[32]{};
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec);
    return std::string(buf);
}

std::string now_iso_local() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t tt = std::chrono::system_clock::to_time_t(now);
    std::tm tm_val{};
#if defined(_WIN32)
    localtime_s(&tm_val, &tt);
#else
    localtime_r(&tt, &tm_val);
#endif
    char buf[64]{};
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tm_val.tm_year + 1900, tm_val.tm_mon + 1, tm_val.tm_mday,
                  tm_val.tm_hour, tm_val.tm_min, tm_val.tm_sec);
    return std::string(buf);
}

std::filesystem::path resolve_runtime_dir() {
#if defined(_WIN32)
    wchar_t module_path[2048]{};
    const DWORD len = GetModuleFileNameW(nullptr, module_path, 2047);
    if (len > 0) {
        const std::filesystem::path p(module_path);
        if (p.has_parent_path()) return p.parent_path();
    }
#endif
    std::error_code ec;
    const auto cwd = std::filesystem::current_path(ec);
    return ec ? std::filesystem::path(".") : cwd;
}

void set_no_mmap_prefetch_env(bool disable_prefetch) {
#if defined(_WIN32)
    _putenv_s("DUALMOE_NO_MMAP_PREFETCH", disable_prefetch ? "1" : "");
#else
    if (disable_prefetch) {
        setenv("DUALMOE_NO_MMAP_PREFETCH", "1", 1);
    } else {
        unsetenv("DUALMOE_NO_MMAP_PREFETCH");
    }
#endif
}

// Set right before a probe graph so a stale cancel flag can never abort the load itself.
bool g_probe_cancel_clean = false;

void ggml_abort_forensics_cb(const char * error_message) {
    // ggml_abort() is also how the scheduler reports an aborted graph: when the abort came from our own
    // cancel callback (GGML_STATUS_ABORTED, "Стоп"/cancelled benchmark), this is not a crash and must
    // not be written to the crash report — otherwise the next start offers a bogus "crash" banner.
    if (g_probe_cancel_clean) {
        return;
    }
#if defined(_WIN32)
    FILE * fp = _wfopen(g_crash_wpath_buf, L"wb");
#else
    FILE * fp = std::fopen(g_crash_file_buf, "wb");
#endif
    if (fp) {
        std::fprintf(fp, "TIME=%s\n", now_iso_local().c_str());
        std::fprintf(fp, "STAGE=%s\n", g_active_stage_buf);
        std::fprintf(fp, "REASON=GGML_ABORT: %s\n", error_message ? error_message : "неизвестная внутренняя ошибка ggml");
        std::fprintf(fp, "ADVICE=Сработал защитный assert внутри ggml/Vulkan. Попробуйте нажать «Безопасная загрузка» (уменьшает GPU слои, кэш VRAM и размер контекста).\n");
        std::fflush(fp);
        std::fclose(fp);
    }
}

#if defined(_WIN32)
const char * describe_win32_exception(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "0xC0000005 EXCEPTION_ACCESS_VIOLATION (обращение к невыделенной/освобождённой странице памяти или сбой драйвера Vulkan)";
        case STATUS_NO_MEMORY:                return "0xC0000017 STATUS_NO_MEMORY (исчерпан лимит виртуальной памяти Windows / pagefile.sys)";
        case EXCEPTION_STACK_OVERFLOW:        return "0xC00000FD EXCEPTION_STACK_OVERFLOW (переполнение стека потока)";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "0xC000001D EXCEPTION_ILLEGAL_INSTRUCTION (неподдерживаемая инструкция CPU)";
        case 0xE06D7363u:                     return "0xE06D7363 Неперехваченное исключение C++ / vk::OutOfDeviceMemoryError (нехватка памяти GPU RX 580 или оперативной памяти)";
        default:                              return "Системное исключение Windows SEH";
    }
}

LONG WINAPI win32_seh_forensics_filter(EXCEPTION_POINTERS * ep) {
    const DWORD code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatusEx(&ms);

    FILE * fp = _wfopen(g_crash_wpath_buf, L"wb");
    if (fp) {
        std::fprintf(fp, "TIME=%s\n", now_iso_local().c_str());
        std::fprintf(fp, "STAGE=%s\n", g_active_stage_buf);
        if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
            const ULONG_PTR rw   = ep->ExceptionRecord->ExceptionInformation[0];
            const ULONG_PTR addr = ep->ExceptionRecord->ExceptionInformation[1];
            std::fprintf(fp, "REASON=%s [%s по адресу 0x%llX] | Свободно RAM: %llu МБ, Commit (pagefile): %llu МБ\n",
                         describe_win32_exception(code),
                         rw == 1 ? "запись" : "чтение",
                         (unsigned long long) addr,
                         (unsigned long long) (ms.ullAvailPhys >> 20),
                         (unsigned long long) (ms.ullAvailPageFile >> 20));
        } else {
            std::fprintf(fp, "REASON=%s (код 0x%08lX) | Свободно RAM: %llu МБ, Commit (pagefile): %llu МБ\n",
                         describe_win32_exception(code),
                         (unsigned long) code,
                         (unsigned long long) (ms.ullAvailPhys >> 20),
                         (unsigned long long) (ms.ullAvailPageFile >> 20));
        }
        std::fprintf(fp, "ADVICE=Увеличьте файл подкачки Windows (Система -> Дополнительно -> Виртуальная память -> Автоматически / от 16384 МБ) или используйте кнопку «Безопасный профиль RX 580».\n");
        std::fflush(fp);
        std::fclose(fp);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

ggml_type parse_kv_type(const std::string & s) {
    if (s == "q8_0" || s == "Q8_0") return GGML_TYPE_Q8_0;
    if (s == "q4_0" || s == "Q4_0") return GGML_TYPE_Q4_0;
    return GGML_TYPE_F16;
}

uint64_t bytes_per_kv_elem_num(ggml_type t) {
    if (t == GGML_TYPE_Q4_0) return 1; // ~0.56 bytes -> conservative 1 byte
    if (t == GGML_TYPE_Q8_0) return 1; // ~1.06 bytes
    return 2;                          // F16 = 2 bytes
}

bool parse_topk_node_layer(const char * name, int32_t & il_out) {
    if (!name) return false;
    static constexpr const char * kPrefix = "ffn_moe_topk-";
    static constexpr size_t kLen = 13;
    if (std::strncmp(name, kPrefix, kLen) != 0) return false;
    char * endp = nullptr;
    const long v = std::strtol(name + kLen, &endp, 10);
    if (!endp || *endp != '\0' || v < 0) return false;
    il_out = (int32_t) v;
    return true;
}

void install_cpu_expert_hook(ggml_cpu_expert_ready_hook_t fn, void * ud) {
    ggml_backend_reg_t reg = ggml_backend_reg_by_name("CPU");
    if (reg) {
        using setter_t = void (*)(ggml_cpu_expert_ready_hook_t, void *);
        auto * dyn = reinterpret_cast<setter_t>(
            ggml_backend_reg_get_proc_address(reg, "ggml_cpu_set_expert_ready_hook"));
        if (dyn) {
            dyn(fn, ud);
            return;
        }
    }
#if !defined(GGML_BACKEND_DL)
    ggml_cpu_set_expert_ready_hook(fn, ud);
#endif
}

size_t incomplete_utf8_tail(const std::string & s) {
    if (s.empty()) return 0;
    const size_t n = s.size();
    size_t back = 0;
    while (back < n && back < 4 && ((uint8_t) s[n - 1 - back] & 0xC0u) == 0x80u) {
        ++back;
    }
    if (back >= n || back >= 4) return 0;
    const uint8_t lead = (uint8_t) s[n - 1 - back];
    size_t expected = 1;
    if ((lead & 0x80u) == 0x00u) expected = 1;
    else if ((lead & 0xE0u) == 0xC0u) expected = 2;
    else if ((lead & 0xF0u) == 0xE0u) expected = 3;
    else if ((lead & 0xF8u) == 0xF0u) expected = 4;
    else return 0;

    const size_t have = back + 1;
    return have < expected ? have : 0;
}

void global_llama_log_cb(enum ggml_log_level level, const char * text, void *) {
    if (!text) return;
    DualMoeEngine * eng = g_active_engine.load(std::memory_order_relaxed);
    if (eng) {
        eng->append_backend_log((int) level, text);
    }
    if (std::getenv("DUALMOE_VERBOSE") || level == GGML_LOG_LEVEL_ERROR) {
        std::fputs(text, stderr);
    }
}

uint64_t now_ms() {
    return (uint64_t) std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string trim_line(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ')) {
        s.pop_back();
    }
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return i > 0 ? s.substr(i) : s;
}

} // namespace

DualMoeEngine::DualMoeEngine() {
    const std::filesystem::path rdir = resolve_runtime_dir();
    const auto log_p    = rdir / "dualmoe-load.log";
    const auto crash_p  = rdir / "dualmoe-crash.txt";
    const auto status_p = rdir / "dualmoe-load.status";
    log_file_path_   = fs_path_to_utf8(log_p);
    crash_file_path_ = fs_path_to_utf8(crash_p);
    const std::string status_path = fs_path_to_utf8(status_p);

    std::snprintf(g_crash_file_buf, sizeof(g_crash_file_buf), "%s", crash_file_path_.c_str());
    std::snprintf(g_status_file_buf, sizeof(g_status_file_buf), "%s", status_path.c_str());
#if defined(_WIN32)
    wcsncpy_s(g_crash_wpath_buf, crash_p.wstring().c_str(), _TRUNCATE);
    wcsncpy_s(g_status_wpath_buf, status_p.wstring().c_str(), _TRUNCATE);
#endif
    diag_.log_file_path = log_file_path_;

    inspect_previous_session_logs();

    g_active_engine.store(this, std::memory_order_release);
    ggml_set_abort_callback(ggml_abort_forensics_cb);
#if defined(_WIN32)
    SetUnhandledExceptionFilter(win32_seh_forensics_filter);
#endif
    llama_log_set(global_llama_log_cb, nullptr);

    ggml_backend_load_all();
    llama_backend_init();

    diag_.sys_mem = query_system_memory();
    init_load_steps(cfg_);
    diag_.loading = false;
    diag_.current_stage = "Готов к загрузке модели";
}

DualMoeEngine::~DualMoeEngine() {
    shutting_down_.store(true, std::memory_order_release);
    if (warm_save_thread_.joinable()) {
        warm_save_thread_.join();
    }
    if (async_load_thread_.joinable()) {
        async_load_thread_.join();
    }
    unload_model();
    if (g_active_engine.load(std::memory_order_relaxed) == this) {
        g_active_engine.store(nullptr, std::memory_order_release);
    }
    llama_backend_free();
}

void DualMoeEngine::append_backend_log(int level, const char * text) {
    if (!text || text[0] == '\0') return;
    const std::string cleaned = trim_line(text);
    if (cleaned.empty() || cleaned == ".") return;

    std::string lvl = "INFO";
    if (level == GGML_LOG_LEVEL_ERROR) lvl = "ERROR";
    else if (level == GGML_LOG_LEVEL_WARN) lvl = "WARN";
    else if (cleaned.find("vulkan") != std::string::npos || cleaned.find("Vulkan") != std::string::npos ||
             cleaned.find("vk") != std::string::npos || cleaned.find("Radeon") != std::string::npos ||
             cleaned.find("RX ") != std::string::npos) {
        lvl = "VULKAN";
    }

    // Ignore noisy debug spam unless warning/error/vulkan/moe_cache/buffer related
    if (level == GGML_LOG_LEVEL_DEBUG && lvl == "INFO") {
        return;
    }

    std::lock_guard<std::mutex> lk(log_mu_);
    LoadLogEntry e;
    e.seq       = next_log_seq_++;
    e.timestamp = now_time_hhmmss();
    e.level     = lvl;
    e.text      = cleaned;
    log_ring_.push_back(e);
    while (log_ring_.size() > 450) {
        log_ring_.pop_front();
    }

    if (loading_.load(std::memory_order_relaxed) || level == GGML_LOG_LEVEL_ERROR || level == GGML_LOG_LEVEL_WARN) {
        FILE * fp = open_utf8_file(log_file_path_, "ab");
        if (fp) {
            std::fprintf(fp, "[%s] [%s] %s\n", e.timestamp.c_str(), e.level.c_str(), e.text.c_str());
            std::fflush(fp);
            std::fclose(fp);
        }
    }
}

void DualMoeEngine::write_flight_line(const std::string & line, bool flush_now) {
    std::lock_guard<std::mutex> lk(log_mu_);
    LoadLogEntry e;
    e.seq       = next_log_seq_++;
    e.timestamp = now_time_hhmmss();
    e.level     = "STEP";
    e.text      = line;
    log_ring_.push_back(e);
    while (log_ring_.size() > 450) {
        log_ring_.pop_front();
    }

    FILE * fp = open_utf8_file(log_file_path_, "ab");
    if (fp) {
        std::fprintf(fp, "[%s] [STEP] %s\n", e.timestamp.c_str(), line.c_str());
        if (flush_now) std::fflush(fp);
        std::fclose(fp);
    }
}

void DualMoeEngine::mark_flight_header(const std::string & status_tag, const std::string & stage_desc) {
    std::snprintf(g_active_stage_buf, sizeof(g_active_stage_buf), "%s", stage_desc.c_str());
    FILE * fp = open_utf8_file(std::string(g_status_file_buf), "wb");
    if (fp) {
        std::fprintf(fp, "STATUS=%s\nTIME=%s\nSTAGE=%s\n",
                     status_tag.c_str(), now_iso_local().c_str(), stage_desc.c_str());
        std::fflush(fp);
        std::fclose(fp);
    }
}

void DualMoeEngine::inspect_previous_session_logs() {
    CrashForensicsReport rep;
    rep.log_file_path = log_file_path_;

    // Read tail of dualmoe-load.log if it exists
    auto load_tail = [&]() {
        std::ifstream lf(std::filesystem::u8path(log_file_path_));
        if (!lf.is_open()) return;
        std::deque<std::string> tail;
        std::string line;
        while (std::getline(lf, line)) {
            const std::string t = trim_line(line);
            if (!t.empty()) {
                tail.push_back(t);
                if (tail.size() > 20) tail.pop_front();
            }
        }
        rep.last_log_tail.assign(tail.begin(), tail.end());
    };

    // Case 1: Explicit dualmoe-crash.txt written by SEH or ggml_set_abort_callback
    {
        std::ifstream cf(std::filesystem::u8path(crash_file_path_));
        if (cf.is_open()) {
            rep.has_previous_crash = true;
            std::string line;
            while (std::getline(cf, line)) {
                line = trim_line(line);
                if (line.rfind("TIME=", 0) == 0) rep.crash_time = line.substr(5);
                else if (line.rfind("STAGE=", 0) == 0) rep.crash_stage = line.substr(6);
                else if (line.rfind("REASON=", 0) == 0) rep.crash_reason = line.substr(7);
                else if (line.rfind("ADVICE=", 0) == 0) rep.crash_advice = line.substr(7);
            }
            cf.close();
            load_tail();
            std::error_code ec;
            std::filesystem::rename(std::filesystem::u8path(crash_file_path_),
                                    std::filesystem::u8path(crash_file_path_ + ".prev"), ec);
            diag_.prev_crash = std::move(rep);
            return;
        }
    }

    // Case 2: Previous session died abruptly while STATUS=IN_PROGRESS (e.g., Windows GPU TDR reset / OOM kill)
    {
        std::ifstream sf(std::filesystem::u8path(std::string(g_status_file_buf)));
        if (sf.is_open()) {
            std::string st, tm, stg;
            std::string line;
            while (std::getline(sf, line)) {
                line = trim_line(line);
                if (line.rfind("STATUS=", 0) == 0) st = line.substr(7);
                else if (line.rfind("TIME=", 0) == 0) tm = line.substr(5);
                else if (line.rfind("STAGE=", 0) == 0) stg = line.substr(6);
            }
            sf.close();
            if (st == "IN_PROGRESS") {
                rep.has_previous_crash = true;
                rep.crash_time   = tm;
                rep.crash_stage  = stg.empty() ? "Во время загрузки модели" : stg;
                rep.crash_reason = "Процесс был принудительно завершён ОС или видеодрайвером на этапе «" + rep.crash_stage + "» (нехватка VRAM RX 580 / лимита файла подкачки Windows / сброс драйвера TDR).";
                rep.crash_advice = "В DualMoE v0.2 уже включена защита uBatch<=32, безопасный MMap и авто-снижение слоёв. Если модель очень крупная (>25 ГБ), нажмите «Безопасный профиль RX 580» или проверьте размер файла подкачки Windows (>= 16 ГБ).";
                load_tail();
                mark_flight_header("IDLE", "Перезапуск после прерванной загрузки");
                diag_.prev_crash = std::move(rep);
            }
        }
    }
}

void DualMoeEngine::clear_crash_banner() {
    std::lock_guard<std::mutex> slk(state_mu_);
    diag_.prev_crash = CrashForensicsReport{};
}

std::vector<LoadLogEntry> DualMoeEngine::recent_logs(uint64_t since_seq) const {
    std::lock_guard<std::mutex> lk(log_mu_);
    std::vector<LoadLogEntry> out;
    for (const auto & e : log_ring_) {
        if (e.seq > since_seq) out.push_back(e);
    }
    return out;
}

std::vector<DeviceInfo> DualMoeEngine::list_devices() const {
    std::vector<DeviceInfo> out;
    const size_t n = ggml_backend_dev_count();
    for (size_t i = 0; i < n; ++i) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        if (!dev) continue;
        DeviceInfo info;
        info.name        = ggml_backend_dev_name(dev) ? ggml_backend_dev_name(dev) : "Device";
        info.description = ggml_backend_dev_description(dev) ? ggml_backend_dev_description(dev) : "";
        ggml_backend_reg_t reg = ggml_backend_dev_backend_reg(dev);
        info.backend     = (reg && ggml_backend_reg_name(reg)) ? ggml_backend_reg_name(reg) : "";
        const auto dt    = ggml_backend_dev_type(dev);
        info.is_gpu      = (dt == GGML_BACKEND_DEVICE_TYPE_GPU || dt == GGML_BACKEND_DEVICE_TYPE_IGPU);
        size_t free_b = 0, total_b = 0;
        ggml_backend_dev_memory(dev, &free_b, &total_b);
        info.free_mb  = (uint64_t) (free_b >> 20);
        info.total_mb = (uint64_t) (total_b >> 20);
        ggml_backend_buffer_type_t buft = ggml_backend_dev_buffer_type(dev);
        if (buft) {
            const size_t max_b = ggml_backend_buft_get_max_size(buft);
            if (max_b > 0 && max_b < (1ull << 50)) {
                info.max_buffer_mb = (uint64_t) (max_b >> 20);
            }
        }
        out.push_back(std::move(info));
    }
    return out;
}

SystemMemoryInfo DualMoeEngine::query_system_memory() const {
    SystemMemoryInfo m;
#if defined(_WIN32)
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        m.ram_total_mb    = (uint64_t) (ms.ullTotalPhys >> 20);
        m.ram_avail_mb    = (uint64_t) (ms.ullAvailPhys >> 20);
        m.commit_total_mb = (uint64_t) (ms.ullTotalPageFile >> 20);
        m.commit_avail_mb = (uint64_t) (ms.ullAvailPageFile >> 20);
    }
#elif defined(__linux__)
    std::ifstream f("/proc/meminfo");
    std::string key, unit;
    uint64_t val_kb = 0;
    uint64_t commit_lim_kb = 0, committed_as_kb = 0;
    while (f >> key >> val_kb) {
        std::getline(f, unit);
        if (key == "MemTotal:") m.ram_total_mb = val_kb >> 10;
        else if (key == "MemAvailable:") m.ram_avail_mb = val_kb >> 10;
        else if (key == "CommitLimit:") commit_lim_kb = val_kb;
        else if (key == "Committed_AS:") committed_as_kb = val_kb;
    }
    m.commit_total_mb = commit_lim_kb > 0 ? (commit_lim_kb >> 10) : m.ram_total_mb;
    m.commit_avail_mb = (commit_lim_kb > committed_as_kb)
        ? ((commit_lim_kb - committed_as_kb) >> 10)
        : m.ram_avail_mb;
#endif

    for (const auto & d : list_devices()) {
        if (d.is_gpu) {
            m.gpu_name           = d.description.empty() ? d.name : d.description;
            m.gpu_backend        = d.backend;
            m.vram_free_mb       = d.free_mb;
            m.vram_total_mb      = d.total_mb;
            m.vram_max_buffer_mb = d.max_buffer_mb;
            break;
        }
    }
    return m;
}

void DualMoeEngine::init_load_steps(const EngineConfig & cfg) {
    diag_.phase_index             = 0;
    diag_.phase_total             = 8;
    diag_.progress_pct            = 0;
    diag_.last_error.clear();
    diag_.warnings.clear();
    diag_.requested_ngl           = cfg.n_gpu_layers;
    diag_.effective_ngl           = cfg.n_gpu_layers;
    diag_.requested_vram_cache_mb = cfg.vram_cache_mb;
    diag_.effective_vram_cache_mb = cfg.vram_cache_mb;
    diag_.requested_ram_cache_mb  = cfg.ram_cache_mb;
    diag_.effective_ram_cache_mb  = cfg.ram_cache_mb;
    diag_.requested_ctx           = cfg.n_ctx;
    diag_.effective_ctx           = cfg.n_ctx;
    diag_.effective_ubatch        = cfg.n_ubatch;

    diag_.steps = {
        {1, "gguf_header", "1. Проверка GGUF и структуры MoE-экспертов",
         "Чтение заголовка, шардов, размера плотных (Attention/Router) и экспертных 3D-тензоров",
         "Сбой здесь бывает, если файл .gguf повреждён, недокачан или указан неверный путь.", "pending", 0.0},
        {2, "mem_audit", "2. Аудит памяти ОС и GPU (RAM / Commit / VRAM)",
         "Измерение свободной DDR4, лимита файла подкачки Windows и свободной VRAM видеокарты",
         "На ПК с 16 ГБ ОЗУ без файла подкачки Windows отклоняет выделение памяти (0xC0000017).", "pending", 0.0},
        {3, "dense_load", "3. Размещение весов Attention и Router (GPU ∥ Host)",
         "Ленивое отображение GGUF без 20-ГБ префетча и загрузка плотных слоёв в VRAM с авто-подбором NGL",
         "Если плотные веса превышают свободную VRAM RX 580, DualMoE автоматически снижает число GPU-слоёв.", "pending", 0.0},
        {4, "stage1_capture", "4. Этап 1: Захват графа экспертов (1-токен прогон)",
         "Создание компактного 64-токен контекста и фиксация точных адресов экспертных тензоров",
         "Проверяется совместимость квантованного KV-кэша (q8_0 -> f16 при необходимости).", "pending", 0.0},
        {5, "virtual_arena", "5. Привязка VirtualExpertArena (VRAM ⇄ DDR4 пул)",
         "Резервирование виртуальных диапазонов MEM_RESERVE и подготовка последовательного чтения без рывков HDD",
         "Исключает хаотичные обращения головок HDD и мгновенное переполнение 16 ГБ RAM.", "pending", 0.0},
        {6, "mmap_guard", "6. Безопасная политика файловых проекций (Vulkan Guard)",
         "Сохранение целостности внутренних проекций драйвера AMD Adrenalin Vulkan",
         "Защищает от сбоя 0xC0000005 из-за выгрузки шейдерного кэша и BAR-областей драйвера.", "pending", 0.0},
        {7, "stage2_context", "7. Этап 2: Рабочий контекст + максимум VRAM под горячие слоты",
         "Выделение KV-кэша, буферов расчёта (uBatch<=32) и максимального безопасного банка экспертов в VRAM",
         "Защищает от переполнения 8 ГБ VRAM и гарантирует, что все микробатчи идут через кэш слотов.", "pending", 0.0},
        {8, "parallel_hooks", "8. Линейная предзагрузка с HDD в VRAM + RAM и запуск GPU ∥ CPU",
         "Однопроходное чтение модели с HDD в VRAM и ОЗУ (0 чтений диска в чате) + живой обмен горячих/холодных слотов",
         "Финальная точка запуска бездисковой параллельной генерации.", "pending", 0.0},
    };
}

void DualMoeEngine::begin_step(int32_t idx, const std::string & detail) {
    step_t0_ = std::chrono::steady_clock::now();
    const int32_t pct = std::clamp<int32_t>((idx - 1) * 12 + 4, 2, 96);
    load_phase_index_.store(idx, std::memory_order_relaxed);
    load_progress_pct_.store(pct, std::memory_order_relaxed);

    std::string stage_line;
    {
        std::lock_guard<std::mutex> slk(state_mu_);
        diag_.phase_index = idx;
        diag_.progress_pct = pct;
        diag_.sys_mem = query_system_memory();
        if (idx >= 1 && (size_t) idx <= diag_.steps.size()) {
            auto & s = diag_.steps[(size_t) (idx - 1)];
            s.state  = "running";
            if (!detail.empty()) s.detail = detail;
            diag_.current_stage = s.title;
            status_text_        = s.title;
            stage_line          = s.title + " — " + s.detail;
        }
    }
    mark_flight_header("IN_PROGRESS", stage_line);
    write_flight_line(">> " + stage_line +
                      " [RAM своб: " + std::to_string(diag_.sys_mem.ram_avail_mb) + " МБ, Commit своб: " +
                      std::to_string(diag_.sys_mem.commit_avail_mb) + " МБ, VRAM своб: " +
                      std::to_string(diag_.sys_mem.vram_free_mb) + " МБ]");
}

void DualMoeEngine::finish_step(int32_t idx, const std::string & state, const std::string & detail) {
    const auto now = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - step_t0_).count();
    const int32_t pct = (state == "error")
        ? load_progress_pct_.load(std::memory_order_relaxed)
        : std::clamp<int32_t>(idx * 12 + (idx == 8 ? 4 : 0), 10, 100);
    load_progress_pct_.store(pct, std::memory_order_relaxed);

    std::string log_msg;
    {
        std::lock_guard<std::mutex> slk(state_mu_);
        diag_.progress_pct = pct;
        diag_.sys_mem = query_system_memory();
        if (idx >= 1 && (size_t) idx <= diag_.steps.size()) {
            auto & s = diag_.steps[(size_t) (idx - 1)];
            s.state      = state;
            s.elapsed_ms = ms;
            if (!detail.empty()) s.detail = detail;
            log_msg = s.title + " [" + state + ", " + std::to_string((int) ms) + " мс]: " + s.detail;
        }
    }
    if (!log_msg.empty()) {
        write_flight_line("<< " + log_msg);
    }
}

void DualMoeEngine::add_warning(const std::string & msg) {
    {
        std::lock_guard<std::mutex> slk(state_mu_);
        diag_.warnings.push_back(msg);
    }
    append_backend_log(GGML_LOG_LEVEL_WARN, ("[Авто-защита DualMoE] " + msg).c_str());
}

bool DualMoeEngine::is_loaded() const {
    return loaded_.load(std::memory_order_acquire);
}

EngineConfig DualMoeEngine::current_config() const {
    std::lock_guard<std::mutex> lk(state_mu_);
    return cfg_;
}

ModelSummary DualMoeEngine::current_model() const {
    std::lock_guard<std::mutex> lk(state_mu_);
    return layout_.summary;
}

LoadDiagnosticsSnapshot DualMoeEngine::diagnostics() const {
    std::lock_guard<std::mutex> lk(state_mu_);
    LoadDiagnosticsSnapshot snap = diag_;
    snap.loading = loading_.load(std::memory_order_acquire);
    if (!snap.loading) {
        snap.sys_mem = query_system_memory();
    }
    return snap;
}

void DualMoeEngine::set_thinking_enabled(bool enabled) {
    std::lock_guard<std::mutex> lk(state_mu_);
    cfg_.enable_thinking = enabled;
}

void DualMoeEngine::request_cancel() {
    cancel_flag_.store(true, std::memory_order_release);
    // The flag must not outlive the turn: ggml only checks the abort callback between graph nodes,
    // so a flag raised by "Стоп", by a cancelled benchmark or by an unload of the previous model can
    // still be set when the NEXT graph runs — that graph then dies with GGML_STATUS_ABORTED
    // (`llama_decode: failed to decode, ret = 2`) even though nobody cancelled it.
    abort_disarm_at_.store((uint64_t) now_ms(), std::memory_order_release);
}

bool DualMoeEngine::apply_live_config(const EngineConfig & cfg, std::string & err) {
    std::lock_guard<std::mutex> lk(api_mu_);
    if (!ctx_ || !loaded_.load(std::memory_order_acquire)) {
        err = "Модель не загружена";
        return false;
    }
    if (generating_.load(std::memory_order_acquire)) {
        err = "Идёт генерация — дождитесь ответа";
        return false;
    }
    // A different bank size (or KV precision / context) needs a reload: refuse instead of silently
    // ignoring the request, the caller then falls back to a full reload.
    size_t bank_mb = 0;
    size_t bank_req_mb = 0;
    {
        std::lock_guard<std::mutex> slk(state_mu_);
        bank_mb     = active_vram_cache_mb_;
        bank_req_mb = cfg_.vram_cache_mb;
    }
    // The bank the load actually got may be smaller than the one the user asked for (VRAM headroom
    // clamp). The request stays the same in that case, so it must not look like a reload trigger —
    // otherwise every live change (threads, CPU share, seam) would be refused on such a machine.
    const bool bank_changed = cfg.vram_cache_mb != bank_mb && cfg.vram_cache_mb != bank_req_mb;
    if (bank_changed || cfg.kv_type_k != cfg_.kv_type_k || cfg.kv_type_v != cfg_.kv_type_v ||
        cfg.n_ctx != cfg_.n_ctx) {
        err = "Эти параметры требуют перезагрузки модели";
        return false;
    }

    const int32_t threads = std::clamp<int32_t>(cfg.n_threads, 1, 512);
    llama_set_n_threads(ctx_, threads, std::max<int32_t>(threads, cfg.n_threads_batch));
    if (cfg.cpu_share >= 0.0f) {
        llama_set_moe_cache_cpu_share(ctx_, std::clamp(cfg.cpu_share, 0.0f, 1.0f));
        active_cpu_share_.store(std::clamp(cfg.cpu_share, 0.0f, 1.0f), std::memory_order_relaxed);
    }
    if (ctx_) {
        const float seam = std::clamp(cfg.parallel_seam_share, 0.0f, 0.9f);
        llama_set_moe_cache_forced_seam(ctx_, seam > 0.02f ? seam : -1.0f);
        active_seam_share_.store(seam > 0.02f ? seam : 0.0f, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> slk(state_mu_);
        cfg_.n_threads       = threads;
        cfg_.n_threads_batch = std::max<int32_t>(threads, cfg.n_threads_batch);
        cfg_.cpu_share       = cfg.cpu_share;
        cfg_.auto_balance_compute = cfg.auto_balance_compute;
        cfg_.static_bank_share    = cfg.static_bank_share;
        cfg_.static_bank_mb       = cfg.static_bank_mb;
    }
    if (cfg.static_bank_mb != 0 || cfg.static_bank_share != 0.0f) {
        apply_static_class();
    }
    return true;
}

void DualMoeEngine::unload_model_locked() {
    if (loaded_.load(std::memory_order_acquire) && cfg_.warm_state) {
        save_warm_state(); // the hot set survives the unload (and the process)
    }
    if (loaded_.load(std::memory_order_acquire)) {
        spec_save();       // the lookahead table makes the next start fast from token #1
    }
    spec_.clear();
    install_cpu_expert_hook(nullptr, nullptr);
    if (chat_templates_) {
        common_chat_templates_free(chat_templates_);
        chat_templates_ = nullptr;
    }
    if (ctx_) {
        llama_set_moe_cache_host_cb(ctx_, nullptr, nullptr);
        llama_set_moe_cache_swap_cbs(ctx_, nullptr, nullptr, 0.0f, nullptr);
        llama_free(ctx_);
        ctx_ = nullptr;
    }
    arena_.shutdown();
    if (model_) {
        llama_model_free(model_);
        model_ = nullptr;
    }
    vocab_ = nullptr;
    kv_tokens_.clear();
    captured_weights_.clear();
    captured_layer_experts_.clear();
    layer_on_gpu_cache_.clear();
    prev_route_.clear();
    {
        std::lock_guard<std::mutex> wl(warm_mu_);
        static_experts_.clear();
        prompt_route_.clear();
    }
    static_class_active_ = false;
    warm_state_loaded_   = false;

    loaded_.store(false, std::memory_order_release);
    generating_.store(false, std::memory_order_release);
    kv_used_.store(0, std::memory_order_relaxed);
    gpu_expert_hits_.store(0, std::memory_order_relaxed);
    gpu_expert_fills_.store(0, std::memory_order_relaxed);
    cpu_parallel_experts_.store(0, std::memory_order_relaxed);
    vram_upload_bytes_.store(0, std::memory_order_relaxed);
    active_cpu_share_.store(0.0f, std::memory_order_relaxed);
    active_seam_share_.store(0.0f, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> slk(state_mu_);
        layout_               = GgufLayout{};
        active_vram_cache_mb_ = 0;
        vram_prewarmed_bytes_ = 0;
        ram_preloaded_bytes_  = 0;
        auto_split_active_    = false;
        parallel_gpu_cpu_     = false;
        zero_hdd_active_      = false;
        disjoint_swap_active_ = false;
        auto_balance_active_  = false;
        active_gpu_layers_    = 0;
        ema_gpu_hit_ratio_    = 0.75f;
        balance_events_       = 0;
        active_ubatch_        = 32;
        status_text_          = "Готов к загрузке модели";
    }
}

void DualMoeEngine::unload_model() {
    if (async_load_thread_.joinable() && async_load_thread_.get_id() != std::this_thread::get_id()) {
        async_load_thread_.join();
    }
    if (warm_save_thread_.joinable() && warm_save_thread_.get_id() != std::this_thread::get_id()) {
        warm_save_thread_.join(); // it reads ctx_ while saving: never free the context under it
    }
    std::lock_guard<std::mutex> lk(api_mu_);
    unload_model_locked();
}

bool DualMoeEngine::start_load_async(const EngineConfig & cfg, std::string & err) {
    if (loading_.load(std::memory_order_acquire)) {
        err = "Загрузка модели уже выполняется — следите за индикатором этапов";
        return false;
    }
    if (generating_.load(std::memory_order_acquire)) {
        cancel_flag_.store(true, std::memory_order_release);
    }
    if (async_load_thread_.joinable()) {
        async_load_thread_.join();
    }
    loading_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> slk(state_mu_);
        init_load_steps(cfg);
        diag_.loading       = true;
        diag_.current_stage = "Подготовка конвейера загрузки...";
        status_text_        = "Подготовка конвейера загрузки...";
    }
    async_load_thread_ = std::thread([this, cfg]() {
        std::string load_err;
        load_model_impl(cfg, load_err);
    });
    return true;
}

bool DualMoeEngine::load_model(const EngineConfig & input_cfg, std::string & err) {
    if (async_load_thread_.joinable() && async_load_thread_.get_id() != std::this_thread::get_id()) {
        async_load_thread_.join();
    }
    loading_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> slk(state_mu_);
        init_load_steps(input_cfg);
        diag_.loading       = true;
        diag_.current_stage = "Подготовка конвейера загрузки...";
        status_text_        = "Подготовка конвейера загрузки...";
    }
    return load_model_impl(input_cfg, err);
}

bool DualMoeEngine::load_model_impl(const EngineConfig & input_cfg, std::string & err) {
    std::lock_guard<std::mutex> lk(api_mu_);

    auto fail_at = [&](int32_t step_idx, const std::string & msg) -> bool {
        err = msg;
        finish_step(step_idx, "error", msg);
        {
            std::lock_guard<std::mutex> slk(state_mu_);
            diag_.loading       = false;
            diag_.last_error    = msg;
            diag_.current_stage = "Ошибка загрузки на шаге " + std::to_string(step_idx);
            status_text_        = "Ошибка: " + msg;
        }
        mark_flight_header("ERROR", "Шаг " + std::to_string(step_idx) + ": " + msg);
        loading_.store(false, std::memory_order_release);
        return false;
    };

    // Reset previous loaded state cleanly
    unload_model_locked();

    // Truncate per-load flight log header so dualmoe-load.log always shows clean latest run
    {
        std::lock_guard<std::mutex> llk(log_mu_);
        FILE * fp = open_utf8_file(log_file_path_, "wb");
        if (fp) {
            std::fprintf(fp, "=== DualMoE v0.3.0 Zero-HDD VRAM<->RAM Flight Recorder ===\n");
            std::fprintf(fp, "Start: %s\nModel: %s\n\n", now_iso_local().c_str(), input_cfg.model_path.c_str());
            std::fflush(fp);
            std::fclose(fp);
        }
    }

    // -------------------------------------------------------------------------
    // STEP 1: GGUF Header & MoE Topology Inspection
    // -------------------------------------------------------------------------
    begin_step(1, "Чтение метаданных GGUF и расчёт объёма плотных/экспертных весов: " + input_cfg.model_path);
    if (input_cfg.model_path.empty()) {
        return fail_at(1, "Путь к файлу модели (.gguf) пуст. Выберите модель в списке или через обзор папок.");
    }

    GgufLayout lay = inspect_gguf_layout(input_cfg.model_path);
    if (!lay.summary.ok) {
        return fail_at(1, lay.summary.error);
    }

    {
        std::ostringstream d1;
        d1 << lay.summary.model_name << " (" << lay.summary.arch << ") • Слоёв: " << lay.summary.n_layer
           << " • Файл: " << (lay.summary.file_size_bytes >> 20) << " МБ";
        if (lay.summary.is_moe) {
            d1 << " • MoE " << lay.summary.n_expert << "xTop" << lay.summary.n_expert_used
               << " (плотные: " << (lay.summary.dense_bytes >> 20) << " МБ, эксперты: "
               << (lay.summary.expert_bytes >> 20) << " МБ, мин. цикл: " << lay.summary.cycle_floor_mb << " МБ)";
        } else {
            d1 << " • Плотная модель (Dense)";
        }
        finish_step(1, "done", d1.str());
    }

    // -------------------------------------------------------------------------
    // STEP 2: OS RAM / Windows Commit / RX 580 VRAM Pre-Flight Audit
    // -------------------------------------------------------------------------
    begin_step(2, "Проверка свободной DDR4, лимита Commit (pagefile.sys) и VRAM видеокарты...");
    SystemMemoryInfo pre_mem = query_system_memory();
    const bool has_gpu_device = !pre_mem.gpu_name.empty();

    EngineConfig cfg = input_cfg;
    const bool stream_moe = cfg.moe_stream && lay.summary.is_moe;

    // Fix #2: Always clamp n_ubatch <= 32 when MoE streaming or GPU MoE cache is enabled!
    // llama_moe_cache::max_batch == 32; larger n_ubatch bypasses slot map during sched_reserve and crashes!
    if ((stream_moe || (lay.summary.is_moe && has_gpu_device && cfg.vram_cache_mb > 0)) &&
        (cfg.n_ubatch == 0 || cfg.n_ubatch > 32)) {
        const uint32_t old_ub = cfg.n_ubatch;
        cfg.n_ubatch = 32;
        add_warning("Микробатч uBatch автоматически зафиксирован на 32 (было " + std::to_string(old_ub) +
                    "), чтобы 100% батчей проходили через кэш слотов llama_moe_cache и не вызывали сбой 0xC0000005 / переполнение VRAM.");
    } else if (cfg.n_ubatch == 0) {
        cfg.n_ubatch = 32;
    }

    // Fix #4 + Zero-HDD Physical RAM Guard:
    // Clamp DDR4 arena against BOTH Windows Commit Limit AND Physical Free RAM (when Zero-HDD mode is on)
    // so Windows 11 NEVER spills warm experts into pagefile.sys on a mechanical HDD!
    const uint64_t safe_phys_ram_mb = pre_mem.ram_avail_mb > 1600
        ? (pre_mem.ram_avail_mb - 1600)
        : (pre_mem.ram_avail_mb * 2 / 3);

    // -------------------------------------------------------------------------
    // AUTO BUDGET: fill VRAM first, then DDR4, and let the HDD keep only the leftovers.
    //
    // The user picks how much VRAM and how much DDR4 the system may keep for itself
    // (vram_reserve_mb / ram_reserve_mb, -1 = automatic). Everything else goes to experts:
    //   VRAM bank -> CPU arena -> disk.
    // This replaces the old "trust the slider" behaviour, which left hundreds of MiB
    // unclaimed on one machine and overflowed on the next.
    // -------------------------------------------------------------------------
    AutoBudgetPlan budget;
    bool auto_budget = cfg.auto_budget && lay.summary.is_moe;
    if (stream_moe && !auto_budget) {
        // Manual mode: keep the historical clamps so a user who typed a number still gets it,
        // but never above what the OS would have to page out.
        const uint64_t expert_total_mb = (lay.summary.expert_bytes >> 20) + 64;
        if (cfg.zero_hdd_mode && safe_phys_ram_mb > 0) {
            const uint64_t safety_commit_reserve_mb = 2048;
            if (pre_mem.commit_avail_mb <= safety_commit_reserve_mb + 384) {
                if (cfg.ram_cache_mb > 384) {
                    add_warning("Внимание: в системе осталось мало виртуальной памяти Commit (" +
                                std::to_string(pre_mem.commit_avail_mb) +
                                " МБ). Рекомендуется включить автоматический файл подкачки Windows (>= 16 ГБ). Бюджет RAM-арены снижен до 384 МБ.");
                    cfg.ram_cache_mb = 384;
                }
            } else {
                const uint64_t max_safe_arena = pre_mem.commit_avail_mb - safety_commit_reserve_mb;
                if (cfg.ram_cache_mb > max_safe_arena) {
                    add_warning("Бюджет DDR4-арены автоматически адаптирован с " + std::to_string(cfg.ram_cache_mb) +
                                " МБ до " + std::to_string(max_safe_arena) +
                                " МБ с учётом доступного лимита виртуальной памяти ОС (" +
                                std::to_string(pre_mem.commit_avail_mb) + " МБ).");
                    cfg.ram_cache_mb = (size_t) max_safe_arena;
                }
            }
        }
    }

    int32_t effective_ngl = has_gpu_device ? std::max<int32_t>(0, cfg.n_gpu_layers) : 0;
    if (!has_gpu_device && cfg.n_gpu_layers > 0) {
        add_warning("Дискретный GPU Vulkan не обнаружен — активирован безопасный режим CPU + SSD (NGL = 0).");
    }

    // The auto budget needs the KV / dense footprints of the layers that go to the GPU. Those are
    // computed exactly a few lines below (SWA + shared-KV aware); this quick pass is only used to size
    // the expert bank, so the same estimator as the benchmark plan is used here for consistency.
    uint64_t planned_bank_mb = 0;
    uint64_t planned_ram_mb  = 0;
    int32_t  last_planned_ngl = -1;
    if (auto_budget) {
        const uint64_t kv_est_mb = lay.summary.is_moe ? estimate_kv_mb(lay.summary, cfg, effective_ngl) : 0;
        const uint64_t dense_gpu_mb = (effective_ngl > 0 && lay.summary.n_layer > 0)
            ? (lay.summary.dense_bytes >> 20) * (uint64_t) std::min<int32_t>(effective_ngl, lay.summary.n_layer) / (uint64_t) lay.summary.n_layer
            : 0;
        budget = plan_auto_budget(lay.summary, pre_mem, cfg, effective_ngl, kv_est_mb, dense_gpu_mb);
        if (!budget.ok) {
            add_warning("Авто-бюджет не рассчитан (" + budget.error + ") — используются значения из настроек.");
        } else {
            last_planned_ngl  = effective_ngl;
            planned_bank_mb   = budget.vram_bank_mb;
            planned_ram_mb    = budget.ram_arena_mb;
            cfg.vram_cache_mb = (size_t) std::max<uint64_t>(1, planned_bank_mb ? planned_bank_mb : 1);
            // A 0 MiB arena would fall back to the 2 GiB default inside VirtualExpertArena and silently
            // ignore the reserve the user picked, so keep at least one expert slot.
            cfg.ram_cache_mb  = (size_t) std::max<uint64_t>(planned_ram_mb, std::max<uint64_t>(1, budget.per_expert_mb));
            if (planned_ram_mb == 0) {
                add_warning("Авто-бюджет: в ОЗУ после резерва места нет — эксперты будут читаться с диска "
                            "(уменьшите «Оставить системе ОЗУ», чтобы забрать их в память).");
            }
            for (const std::string & r : budget.reasons) add_warning("Авто-бюджет: " + r);
        }
    }

    // Pre-estimate dense VRAM footprint if GPU is present so we don't ask RX 580 for impossible dense allocations
    if (has_gpu_device && effective_ngl > 0 && pre_mem.vram_free_mb > 0 && lay.summary.dense_bytes > 0 && lay.summary.n_layer > 0) {
        const uint64_t dense_mb = lay.summary.dense_bytes >> 20;
        // Leave at least 900 MiB VRAM after dense weights for KV cache + compute buffers
        const uint64_t max_dense_vram = pre_mem.vram_free_mb > 900 ? (pre_mem.vram_free_mb - 900) : (pre_mem.vram_free_mb / 2);
        if (!lay.summary.is_moe) {
            const uint64_t total_model_mb = lay.summary.file_size_bytes >> 20;
            if (total_model_mb > max_dense_vram && total_model_mb > 0) {
                const int32_t fit_layers = std::clamp<int32_t>(
                    (int32_t) ((uint64_t) lay.summary.n_layer * max_dense_vram / total_model_mb), 0, lay.summary.n_layer);
                if (effective_ngl > fit_layers) {
                    add_warning("Модель плотная (" + std::to_string(total_model_mb) + " МБ), свободно VRAM: " +
                                std::to_string(pre_mem.vram_free_mb) + " МБ — число GPU-слоёв адаптировано до " +
                                std::to_string(fit_layers) + ".");
                    effective_ngl = fit_layers;
                }
            }
        } else if (dense_mb > max_dense_vram && dense_mb > 0) {
            const int32_t fit_layers = std::clamp<int32_t>(
                (int32_t) ((uint64_t) lay.summary.n_layer * max_dense_vram / dense_mb), 0, lay.summary.n_layer);
            if (effective_ngl > fit_layers) {
                add_warning("Плотные веса Attention/Router (" + std::to_string(dense_mb) +
                            " МБ) превышают безопасный запас VRAM (" + std::to_string(pre_mem.vram_free_mb) +
                            " МБ) — число GPU-слоёв адаптировано до " + std::to_string(fit_layers) + ".");
                effective_ngl = fit_layers;
            }
        }
    }

    {
        std::lock_guard<std::mutex> slk(state_mu_);
        diag_.effective_ngl          = effective_ngl;
        diag_.effective_ram_cache_mb = cfg.ram_cache_mb;
        diag_.effective_ubatch       = cfg.n_ubatch;
    }

    {
        std::ostringstream d2;
        d2 << "RAM своб: " << pre_mem.ram_avail_mb << "/" << pre_mem.ram_total_mb << " МБ • "
           << "Commit своб: " << pre_mem.commit_avail_mb << "/" << pre_mem.commit_total_mb << " МБ";
        if (has_gpu_device) {
            d2 << " • GPU: " << pre_mem.gpu_name << " (своб. VRAM " << pre_mem.vram_free_mb << "/" << pre_mem.vram_total_mb << " МБ";
            if (pre_mem.vram_max_buffer_mb > 0) d2 << ", макс. буфер " << pre_mem.vram_max_buffer_mb << " МБ";
            d2 << ")";
        } else {
            d2 << " • Режим CPU";
        }
        finish_step(2, "done", d2.str());
    }

    // -------------------------------------------------------------------------
    // STEP 3: Load Dense Weights & Router (with No-Prefetch Guard + Auto NGL Step-Down)
    // -------------------------------------------------------------------------
    begin_step(3, "Ленивое отображение GGUF и размещение весов Attention/Router (NGL=" + std::to_string(effective_ngl) + ")...");

    // Fix #3: Prevent Windows PrefetchVirtualMemory from faulting all 20+ GB of GGUF into 16 GB RAM!
    set_no_mmap_prefetch_env(stream_moe);

    std::vector<int32_t> ngl_candidates;
    ngl_candidates.push_back(effective_ngl);
    for (int32_t fb : {40, 32, 24, 16, 8, 0}) {
        if (fb < effective_ngl && std::find(ngl_candidates.begin(), ngl_candidates.end(), fb) == ngl_candidates.end()) {
            ngl_candidates.push_back(fb);
        }
    }

    bool split_experts = false;
    static const char * kRoutedExpsRegex = "\\.ffn_(gate|up|down|gate_up)_exps\\.weight";

    for (size_t attempt = 0; attempt < ngl_candidates.size(); ++attempt) {
        const int32_t try_ngl = ngl_candidates[attempt];
        split_experts = lay.summary.is_moe && try_ngl > 0 &&
                        (cfg.auto_split_experts || stream_moe || cfg.vram_cache_mb > 0);

        llama_model_params mparams = llama_model_default_params();
        ggml_backend_dev_t cpu_only_devs[1] = {nullptr};
        if (try_ngl == 0) {
            mparams.devices = cpu_only_devs;
        }
        // Zero-HDD Optimization for Dense models: if host portion fits safe DDR4 RAM, read sequentially
        // into anonymous RAM once (LLAMA_LOAD_MODE_NONE) -> 0 HDD page faults forever during chat!
        const uint64_t total_model_mb = lay.summary.file_size_bytes >> 20;
        const uint64_t est_gpu_mb     = (!lay.summary.is_moe && lay.summary.n_layer > 0)
            ? (total_model_mb * (uint64_t) std::min<int32_t>(try_ngl, lay.summary.n_layer) / (uint64_t) lay.summary.n_layer)
            : 0;
        const uint64_t est_host_mb    = total_model_mb > est_gpu_mb ? (total_model_mb - est_gpu_mb) : 0;
        const bool dense_zero_hdd_ram = (!lay.summary.is_moe) && cfg.zero_hdd_mode &&
                                        (safe_phys_ram_mb == 0 || est_host_mb + 256 <= safe_phys_ram_mb);

        mparams.load_mode       = dense_zero_hdd_ram ? LLAMA_LOAD_MODE_NONE : LLAMA_LOAD_MODE_MMAP;
        mparams.use_extra_bufts = false;
        mparams.lazy_mode       = (stream_moe || dense_zero_hdd_ram) ? LLAMA_LAZY_MODE_OFF : LLAMA_LAZY_MODE_AUTO;
        mparams.n_gpu_layers    = try_ngl;

        std::vector<llama_model_tensor_buft_override> overrides;
        if (split_experts) {
            overrides.push_back({kRoutedExpsRegex, ggml_backend_cpu_buffer_type()});
            overrides.push_back({nullptr, nullptr});
            mparams.tensor_buft_overrides = overrides.data();
        }

        llama_model_kv_override kv_ov[2]{};
        std::string used_key;
        if (cfg.n_expert_used_override > 0 && lay.summary.is_moe && !lay.summary.arch.empty()) {
            used_key = lay.summary.arch + ".expert_used_count";
            kv_ov[0].tag = LLAMA_KV_OVERRIDE_TYPE_INT;
            std::snprintf(kv_ov[0].key, sizeof(kv_ov[0].key), "%s", used_key.c_str());
            kv_ov[0].val_i64 = std::min<int32_t>(cfg.n_expert_used_override, lay.summary.n_expert);
            kv_ov[1].key[0] = '\0';
            mparams.kv_overrides = kv_ov;
        }

        model_ = llama_model_load_from_file(cfg.model_path.c_str(), mparams);
        if (model_) {
            if (attempt > 0) {
                add_warning("Размещение весов успешно выполнено после авто-снижения GPU-слоёв до NGL = " +
                            std::to_string(try_ngl) + ".");
            }
            effective_ngl = try_ngl;
            break;
        }
        if (attempt + 1 < ngl_candidates.size()) {
            write_flight_line("Попытка загрузки с NGL=" + std::to_string(try_ngl) +
                              " не поместилась в память, авто-повтор с NGL=" + std::to_string(ngl_candidates[attempt + 1]));
        }
    }

    set_no_mmap_prefetch_env(false);

    if (!model_) {
        return fail_at(3, "llama.cpp не смог отобразить или разместить веса модели. Проверьте целостность GGUF и наличие свободного места в файле подкачки Windows.");
    }

    vocab_ = llama_model_get_vocab(model_);
    const int32_t n_layer = llama_model_n_layer(model_);
    // An OOM retry may have moved the model to FEWER GPU layers than the budget was computed for.
    // Re-plan against the layers that really got offloaded, so the bank neither asks for VRAM the retry
    // just freed nor leaves the newly free VRAM unused.
    if (auto_budget && last_planned_ngl >= 0 && last_planned_ngl != effective_ngl) {
        const uint64_t kv_est_mb = estimate_kv_mb(lay.summary, cfg, effective_ngl);
        const uint64_t dense_gpu_mb = (effective_ngl > 0 && lay.summary.n_layer > 0)
            ? (lay.summary.dense_bytes >> 20) * (uint64_t) std::min<int32_t>(effective_ngl, lay.summary.n_layer) / (uint64_t) lay.summary.n_layer
            : 0;
        const SystemMemoryInfo now_mem = query_system_memory();
        budget = plan_auto_budget(lay.summary, now_mem, cfg, effective_ngl, kv_est_mb, dense_gpu_mb);
        if (budget.ok) {
            cfg.vram_cache_mb = (size_t) std::max<uint64_t>(1, budget.vram_bank_mb ? budget.vram_bank_mb : 1);
            cfg.ram_cache_mb  = (size_t) std::max<uint64_t>(budget.ram_arena_mb, std::max<uint64_t>(1, budget.per_expert_mb));
            add_warning("Авто-бюджет пересчитан под реально размещённые GPU-слои (" +
                        std::to_string(effective_ngl) + "): банк VRAM " +
                        std::to_string(budget.vram_bank_mb) + " МБ.");
        }
    }
    SystemMemoryInfo post_weight_mem = query_system_memory();
    {
        std::lock_guard<std::mutex> slk(state_mu_);
        diag_.effective_ngl = effective_ngl;
    }

    {
        std::ostringstream d3;
        d3 << "Веса размещены (активно GPU-слоёв: " << effective_ngl
           << (split_experts ? ", 3D-эксперты отделены от VRAM" : "")
           << ") • Свободно VRAM после плотных весов: " << post_weight_mem.vram_free_mb << " МБ";
        finish_step(3, "done", d3.str());
    }

    auto make_base_cparams = [&](uint32_t ctx_len) {
        llama_context_params cp = llama_context_default_params();
        cp.n_ctx           = ctx_len > 0 ? ctx_len : 8192;
        cp.n_batch         = std::max<uint32_t>(32, cfg.n_batch);
        cp.n_ubatch        = std::clamp<uint32_t>(cfg.n_ubatch > 0 ? cfg.n_ubatch : 32, 8, cp.n_batch);
        if (stream_moe) {
            cp.n_ubatch = std::min<uint32_t>(cp.n_ubatch, 32);
        }
        const int32_t hw_threads = std::max<int32_t>(2, (int32_t) std::thread::hardware_concurrency());
        cp.n_threads       = std::clamp<int32_t>(cfg.n_threads, 1, hw_threads);
        cp.n_threads_batch = std::clamp<int32_t>(cfg.n_threads_batch > 0 ? cfg.n_threads_batch : cfg.n_threads, 1, hw_threads);
        cp.n_outputs_max   = 32;
        cp.type_k          = parse_kv_type(cfg.kv_type_k);
        cp.type_v          = parse_kv_type(cfg.kv_type_v);
        cp.offload_kqv     = (effective_ngl > 0);
        cp.op_offload      = !stream_moe && (effective_ngl > 0);
        cp.cb_eval           = &DualMoeEngine::c_eval_callback;
        cp.cb_eval_user_data = this;
        cp.abort_callback      = &DualMoeEngine::c_abort_callback;
        cp.abort_callback_data = this;
        return cp;
    };

    {
        std::lock_guard<std::mutex> slk(state_mu_);
        cfg_    = cfg;
        layout_ = lay;
    }
    captured_weights_.clear();
    captured_layer_experts_.assign((size_t) std::max<int32_t>(1, n_layer), {});
    prev_route_.assign((size_t) std::max<int32_t>(1, n_layer), {});
    layer_on_gpu_cache_.assign((size_t) std::max<int32_t>(1, n_layer), false);
    {
        std::lock_guard<std::mutex> wl(warm_mu_);
        static_experts_.assign((size_t) std::max<int32_t>(1, n_layer), {});
        prompt_route_.assign((size_t) std::max<int32_t>(1, n_layer), {});
    }

    // -------------------------------------------------------------------------
    // STEP 4, 5, 6: Stage-1 Graph Capture, VirtualExpertArena, Safe MMap Guard
    // -------------------------------------------------------------------------
    bool capture_failed = false;
    if (stream_moe) {
        begin_step(4, "Пробный 1-токен прогон (ctx=64, batch=8) для точного обнаружения тензоров экспертов...");
        llama_context_params cap_cp = make_base_cparams(64);
        cap_cp.n_ctx          = 64;
        cap_cp.n_batch        = 8;
        cap_cp.n_ubatch       = 8;
        cap_cp.moe_cache_size = 0;

        llama_context * cap_ctx = llama_init_from_model(model_, cap_cp);
        if (!cap_ctx && ggml_is_quantized(cap_cp.type_v)) {
            cap_cp.type_v = GGML_TYPE_F16;
            cfg.kv_type_v = "f16";
            add_warning("Видеодрайвер не поддерживает квантованный V-кэш без FlashAttn — тип V автоматически переключён на f16.");
            cap_ctx = llama_init_from_model(model_, cap_cp);
        }
        if (!cap_ctx && ggml_is_quantized(cap_cp.type_k)) {
            cap_cp.type_k = GGML_TYPE_F16;
            cfg.kv_type_k = "f16";
            cap_ctx = llama_init_from_model(model_, cap_cp);
        }
        if (!cap_ctx) {
            llama_model_free(model_);
            model_ = nullptr;
            return fail_at(4, "Не удалось создать компактный контекст захвата графа (проверьте свободную память VRAM/RAM).");
        }

        capture_mode_ = true;
        llama_token warm_tok = llama_vocab_bos(vocab_);
        if (warm_tok < 0) warm_tok = 0;
        llama_batch warm = llama_batch_get_one(&warm_tok, 1);
        // The probe runs with a fresh, private abort callback: a stale cancel from a previous turn or
        // from a cancelled benchmark must never abort the graph capture of a new model.
        cancel_flag_.store(false, std::memory_order_release);
        abort_disarm_at_.store(0, std::memory_order_release);
        g_probe_cancel_clean = true;
        const int rc = llama_decode(cap_ctx, warm);
        g_probe_cancel_clean = false;
        capture_mode_ = false;
        llama_free(cap_ctx);

        if (rc != 0) {
            // 2 / 1 = GGML_STATUS_ABORTED (a cancel raced the probe), -2 / -3 = allocation failure.
            // Either way this only disables the *streaming* of MoE experts; the model itself is fine,
            // so continue in a degraded but working mode instead of refusing to load.
            std::string why = (rc == 2 || rc == 1)
                ? "прогон был прерван (отмена пришла во время загрузки)"
                : "не хватило памяти под пробный граф";
            add_warning("Этап 1 (захват графа экспертов) пропущен: " + why + " (код " +
                        std::to_string(rc) + "). Модель загружается без стриминга экспертов — "
                        "попробуйте закрыть фон и повторить загрузку для полного режима.");
            begin_step(4, "Захват графа экспертов пропущен — модель загрузится без стриминга");
            finish_step(4, "warn", "Пробный прогон не удался (код " + std::to_string(rc) +
                                   "), стриминг экспертов отключён");
            // Without a captured graph the arena has no tensors to bind: fall through to the
            // standard (non-streaming) path instead of aborting the load.
            capture_failed = true;
            cfg.moe_stream = false;
        }

        size_t total_captured_exps = 0;
        for (const auto & v : captured_layer_experts_) total_captured_exps += v.size();
        finish_step(4, "done", "Обнаружено " + std::to_string(total_captured_exps) +
                               " 3D-проекций экспертов и " + std::to_string(captured_weights_.size()) + " весовых листьев");

        // STEP 5: VirtualExpertArena (skipped when the capture probe could not identify the experts)
        if (!capture_failed) {
        const bool seq_hdd_mode = cfg.zero_hdd_mode || cfg.io_lanes <= 1;
        begin_step(5, "Инициализация арены VRAM ⇄ DDR4 (бюджет RAM " + std::to_string(cfg.ram_cache_mb) +
                      " МБ, режим " + std::string(seq_hdd_mode ? "линейный HDD без рывков" : "многопоточный SSD") + ")...");
        const size_t ram_budget = cfg.ram_cache_mb > 0 ? (cfg.ram_cache_mb << 20) : (2048ull << 20);
        std::string arena_err;
        if (!arena_.init(lay.summary.shard_paths, n_layer, lay.summary.n_expert, ram_budget, cfg.io_lanes, seq_hdd_mode, arena_err)) {
            llama_model_free(model_);
            model_ = nullptr;
            return fail_at(5, arena_err);
        }

        std::unordered_set<std::string> bound_expert_names;
        for (int32_t il = 0; il < n_layer; ++il) {
            for (ggml_tensor * et : captured_layer_experts_[(size_t) il]) {
                const auto it = lay.tensors.find(et->name);
                if (it == lay.tensors.end()) continue;
                if (!arena_.bind_layer_projection(il, et, it->second.shard_idx,
                                                  it->second.file_offset, it->second.nbytes, arena_err)) {
                    arena_.shutdown();
                    llama_model_free(model_);
                    model_ = nullptr;
                    return fail_at(5, arena_err);
                }
                bound_expert_names.insert(et->name);
            }
        }
        finish_step(5, "done", "Привязано " + std::to_string(bound_expert_names.size()) +
                               " экспертных тензоров в MEM_RESERVE (0 МБ занято до обращения)");

        // STEP 6: Safe MMap Guard & Optional Dense Rehome
        begin_step(6, "Проверка политики файловых отображений (защита Vulkan MEM_MAPPED)...");
        if (cfg.dense_anon) {
            std::vector<DenseRehomeEntry> dense_list;
            std::unordered_map<uint64_t, size_t> seen_key;
            for (ggml_tensor * wt : captured_weights_) {
                if (!wt) continue;
                if (wt->buffer && !ggml_backend_buffer_is_host(wt->buffer)) continue;
                const std::string tname = wt->name;
                if (bound_expert_names.count(tname)) continue;
                const auto it = lay.tensors.find(tname);
                if (it == lay.tensors.end()) continue;

                const uint64_t key = ((uint64_t) (uint32_t) it->second.shard_idx << 48) ^ it->second.file_offset;
                const auto sit = seen_key.find(key);
                if (sit != seen_key.end()) {
                    dense_list[sit->second].aliases.push_back(wt);
                    continue;
                }
                DenseRehomeEntry de;
                de.primary     = wt;
                de.shard_idx   = it->second.shard_idx;
                de.file_offset = it->second.file_offset;
                de.nbytes      = it->second.nbytes;
                seen_key.emplace(key, dense_list.size());
                dense_list.push_back(std::move(de));
            }
            std::string rehome_err;
            if (!arena_.rehome_dense_weights(dense_list, rehome_err)) {
                add_warning("Перенос плотных CPU-весов в анонимную память пропущен (" + rehome_err + ") — сохранено безопасное чтение через OS MMap.");
                cfg.dense_anon   = false;
                cfg.release_mmap = false;
            }
        }

        if (cfg.release_mmap && cfg.dense_anon) {
            std::vector<const void *> host_ptrs;
            for (ggml_tensor * wt : captured_weights_) {
                if (!wt) continue;
                if (wt->buffer && !ggml_backend_buffer_is_host(wt->buffer)) continue;
                if (wt->data) host_ptrs.push_back(wt->data);
            }
            const bool dropped = arena_.release_file_mappings(host_ptrs);
            finish_step(6, "done", dropped
                ? "Файловое отображение GGUF безопасно сброшено (проверено имя файла .gguf)"
                : "Проекции драйвера сохранены без изменений (безопасный режим)");
        } else {
            finish_step(6, "done", "Активен безопасный режим Vulkan Guard (проекции драйвера защищены, 20-ГБ префетч отключён)");
        }

        install_cpu_expert_hook(&VirtualExpertArena::c_expert_ready_hook, &arena_);
        } else {
            finish_step(5, "warn", "Виртуальная арена MoE пропущена: экспертные тензоры не обнаружены");
            finish_step(6, "warn", "Плотные веса остаются на стандартном отображении памяти");
        }
    } else {
        finish_step(4, "done", "Модель плотная — захват 3D-экспертов не требуется");
        finish_step(5, "done", "Виртуальная арена MoE пропущена (режим стандартных весов)");
        finish_step(6, "done", "Стандартное отображение памяти активно");
    }

    // -------------------------------------------------------------------------
    // STEP 7: Stage-2 Production Context + Dynamic RX 580 VRAM ClassLRU Clamp
    // -------------------------------------------------------------------------
    begin_step(7, "Расчёт остатка VRAM под KV-кэш и горячий банк экспертов ClassLRU...");

    post_weight_mem = query_system_memory();
    uint32_t target_ctx = cfg.n_ctx > 0 ? cfg.n_ctx : 8192;
    size_t target_moe_cache_mb = (lay.summary.is_moe && effective_ngl > 0) ? cfg.vram_cache_mb : 0;

    if (has_gpu_device && effective_ngl > 0 && post_weight_mem.vram_free_mb > 0) {
        // Exact SWA + Shared-KV + GQA aware KV footprint estimate (unlocks ~900+ MiB extra VRAM on Gemma 4 26B-A4B!)
        const uint64_t kv_elem_b = bytes_per_kv_elem_num(parse_kv_type(cfg.kv_type_k)) +
                                   bytes_per_kv_elem_num(parse_kv_type(cfg.kv_type_v));
        const int32_t total_L    = std::max<int32_t>(1, n_layer);
        const int32_t own_kv_L   = std::max<int32_t>(1, total_L - std::max<int32_t>(0, lay.summary.n_kv_shared_layers));
        const int32_t first_gpu  = effective_ngl >= total_L ? 0 : std::max<int32_t>(0, total_L - effective_ngl);
        const int32_t gpu_own_kv = std::max<int32_t>(0, own_kv_L - first_gpu);

        uint64_t est_kv_mb = 0;
        if (lay.summary.kv_dim_full > 0 && gpu_own_kv > 0) {
            const int32_t swa_L  = (lay.summary.n_swa > 0)
                ? std::min<int32_t>(gpu_own_kv, (int32_t) ((int64_t) lay.summary.n_swa_layers * gpu_own_kv / own_kv_L))
                : 0;
            const int32_t full_L = std::max<int32_t>(0, gpu_own_kv - swa_L);
            const uint64_t swa_ctx = std::min<uint64_t>(target_ctx, (uint64_t) lay.summary.n_swa + 128ull);
            const uint64_t full_b  = (uint64_t) full_L * (uint64_t) target_ctx * (uint64_t) lay.summary.kv_dim_full * kv_elem_b;
            const uint64_t swa_b   = (uint64_t) swa_L  * swa_ctx * (uint64_t) std::max<int32_t>(1, lay.summary.kv_dim_swa) * kv_elem_b;
            est_kv_mb = (full_b + swa_b + (1ull << 20) - 1) >> 20;
        } else {
            const uint64_t kv_dim_est = std::max<uint64_t>(256, (uint64_t) std::max<int32_t>(512, lay.summary.n_embd) / 3);
            est_kv_mb = ((uint64_t) gpu_own_kv * (uint64_t) target_ctx * kv_dim_est * kv_elem_b) >> 20;
        }
        const uint64_t compute_headroom_mb = 360; // Compute graph + Vulkan staging safety margin

        if (post_weight_mem.vram_free_mb <= est_kv_mb + compute_headroom_mb) {
            if (target_moe_cache_mb > 0) {
                add_warning("После размещения весов и KV-кэша (" + std::to_string(est_kv_mb) +
                            " МБ) в GPU свободно " + std::to_string(post_weight_mem.vram_free_mb) +
                            " МБ VRAM — горячий банк VRAM отключён (0 МБ), эксперты пойдут через DDR4+SSD при сохранении Attention на GPU.");
                target_moe_cache_mb = 0;
            }
            // If even KV exceeds free VRAM, auto-trim context window so it doesn't crash!
            if (post_weight_mem.vram_free_mb < est_kv_mb + 220 && target_ctx > 8192) {
                target_ctx = 8192;
                add_warning("Окно контекста автоматически снижено до 8192 токенов для предотвращения нехватки VRAM (" +
                            std::to_string(post_weight_mem.vram_free_mb) + " МБ свободно).");
            }
        } else if (target_moe_cache_mb > 0) {
            const uint64_t avail_for_moe_mb = post_weight_mem.vram_free_mb - est_kv_mb - compute_headroom_mb;
            if (target_moe_cache_mb > avail_for_moe_mb) {
                add_warning("Кэш горячих экспертов в VRAM автоматически адаптирован с " +
                            std::to_string(target_moe_cache_mb) + " МБ до " +
                            std::to_string(avail_for_moe_mb) + " МБ (остаток свободной VRAM после весов и KV-кэша).");
                target_moe_cache_mb = (size_t) avail_for_moe_mb;
            }
        }
    }

    auto apply_gpu_layer_mask = [&](bool enabled) {
        layer_on_gpu_cache_.assign((size_t) std::max<int32_t>(1, n_layer), false);
        if (enabled && effective_ngl > 0) {
            const int32_t gpu_block_count = effective_ngl > n_layer ? n_layer : std::max<int32_t>(0, effective_ngl - 1);
            const int32_t first_gpu_il    = std::max<int32_t>(0, n_layer - gpu_block_count);
            for (int32_t il = first_gpu_il; il < n_layer; ++il) {
                layer_on_gpu_cache_[(size_t) il] = true;
            }
        }
    };

    llama_context_params cparams = make_base_cparams(target_ctx);
    if (target_moe_cache_mb > 0) {
        cparams.moe_cache_size      = target_moe_cache_mb << 20;
        // The MoE cache is the only path that can run the GPU and the CPU on the same token, so the CPU
        // share must be a real value here: 0 disables the CPU part for good (it stays disabled even if
        // the user raises cpu_share later with apply_live_config).
        const float share_init = cfg.cpu_share > 0.0f ? std::clamp(cfg.cpu_share, 0.0f, 1.0f)
                                                     : (cfg.moe_stream ? 0.85f : 0.0f);
        cparams.moe_cache_cpu_share = share_init;
        apply_gpu_layer_mask(true);
    } else {
        cparams.moe_cache_size = 0;
        apply_gpu_layer_mask(false);
    }

    ctx_ = llama_init_from_model(model_, cparams);
    if (!ctx_ && ggml_is_quantized(cparams.type_v)) {
        cparams.type_v = GGML_TYPE_F16;
        cfg.kv_type_v  = "f16";
        ctx_ = llama_init_from_model(model_, cparams);
    }
    if (!ctx_ && ggml_is_quantized(cparams.type_k)) {
        cparams.type_k = GGML_TYPE_F16;
        cfg.kv_type_k  = "f16";
        ctx_ = llama_init_from_model(model_, cparams);
    }
    // Step down moe_cache_size if Vulkan heap fragmentation still rejected bank allocation
    if (!ctx_ && cparams.moe_cache_size > 0) {
        const size_t floor_b = std::max<size_t>((size_t) lay.summary.cycle_floor_mb << 20, 64ull << 20);
        size_t cur_sz = cparams.moe_cache_size;
        while (!ctx_ && (cur_sz * 2) / 3 >= floor_b) {
            cur_sz = ((cur_sz * 2) / 3) & ~((1ull << 20) - 1);
            cparams.moe_cache_size = cur_sz;
            ctx_ = llama_init_from_model(model_, cparams);
        }
        if (!ctx_) {
            add_warning("Выделение банка экспертов в VRAM отклонено драйвером из-за фрагментации памяти — переключено на 0 МБ VRAM-банка (Attention остаётся на GPU, эксперты на CPU+SSD).");
            cparams.moe_cache_size = 0;
            apply_gpu_layer_mask(false);
            ctx_ = llama_init_from_model(model_, cparams);
        }
    }
    // Last-resort context trim if KV cache itself exceeded VRAM/RAM
    if (!ctx_ && cparams.n_ctx > 4096) {
        cparams.n_ctx = 4096;
        target_ctx    = 4096;
        add_warning("Контекст автоматически уменьшен до 4096 токенов для успешной инициализации памяти.");
        ctx_ = llama_init_from_model(model_, cparams);
    }

    if (!ctx_) {
        install_cpu_expert_hook(nullptr, nullptr);
        arena_.shutdown();
        llama_model_free(model_);
        model_ = nullptr;
        return fail_at(7, "Не удалось выделить рабочий контекст и KV-кэш даже после авто-снижения параметров. Закройте фоновые программы/браузерные вкладки или увеличьте файл подкачки Windows.");
    }

    cfg.n_ctx = cparams.n_ctx;
    {
        std::lock_guard<std::mutex> slk(state_mu_);
        diag_.effective_vram_cache_mb = (uint64_t) (cparams.moe_cache_size >> 20);
        diag_.effective_ctx           = cparams.n_ctx;
        diag_.effective_ubatch        = cparams.n_ubatch;
    }

    {
        std::ostringstream d7;
        d7 << "Контекст " << cparams.n_ctx << " ток. (uBatch=" << cparams.n_ubatch
           << ", KV=" << cfg.kv_type_k << "/" << cfg.kv_type_v << ") • "
           << "VRAM-банк экспертов: " << (cparams.moe_cache_size >> 20) << " МБ";
        finish_step(7, diag_.warnings.empty() ? "done" : "warn", d7.str());
    }

    // -------------------------------------------------------------------------
    // STEP 8: Zero-HDD Sequential Prewarm (VRAM + DDR4 RAM) & Live VRAM <-> RAM Swapper
    // -------------------------------------------------------------------------
    begin_step(8, cfg.zero_hdd_mode
        ? "Линейное чтение весов с HDD в VRAM и ОЗУ без рывков головок (подготовка режима 0 чтений диска)..."
        : "Подключение асинхронного маршрутизатора GPU ∥ CPU ∥ Диск...");

    try {
        auto tmpl_ptr = common_chat_templates_init(model_, "");
        chat_templates_ = tmpl_ptr.release();
    } catch (...) {
        chat_templates_ = nullptr;
    }

    uint64_t prewarmed_vram_b = 0;
    uint64_t preloaded_ram_b  = 0;
    bool     disjoint_active  = false;

    if (stream_moe) {
        // Activate Disjoint VRAM (+) RAM Bidirectional Swap automatically whenever experts exceed DDR4 RAM budget alone,
        // saving up to ~5 GB of physical DDR4 RAM so 14-18 GB models fit 100% in VRAM + RAM without pagefile.sys!
        disjoint_active = cfg.zero_hdd_mode && (cparams.moe_cache_size > 0) && (effective_ngl > 0) &&
                          (lay.summary.expert_bytes > ((uint64_t) cfg.ram_cache_mb << 20));
        arena_.set_disjoint_vram_mode(disjoint_active);
    }

    if (cparams.moe_cache_size > 0 && effective_ngl > 0) {
        llama_set_moe_cache_host_cb(ctx_, &DualMoeEngine::c_moe_cache_callback, this);

        // Prewarm the RX 580 VRAM MoE slots so token #1 hits hot GPU + parallel CPU immediately.
        // The warm-start file tells which experts this model actually routes to; prewarming the bank in
        // that order means the first answer is already warm — the rest is uploaded lazily on demand.
        if (cfg.zero_hdd_mode && stream_moe) {
            std::vector<int32_t> warm_layers, warm_experts;
            {
                std::lock_guard<std::mutex> wl(warm_mu_);
                for (size_t il = 0; il < static_experts_.size(); ++il) {
                    for (int32_t e : static_experts_[il]) {
                        warm_layers.push_back((int32_t) il);
                        warm_experts.push_back(e);
                    }
                }
            }
            const auto t_pw0 = std::chrono::steady_clock::now();
            // Token #1 must not pay for the RAM side: fetch the warm experts from disk once, sequentially,
            // and let the bank upload them while the rest of the load finishes.
            if (!warm_layers.empty()) {
                for (size_t i = 0; i < warm_layers.size(); ++i) {
                    const int32_t il = warm_layers[i];
                    if (!arena_.has_layer(il)) continue;
                    arena_.ensure_experts_sync(il, &warm_experts[i], 1);
                }
            }
            prewarmed_vram_b = (uint64_t) llama_moe_cache_prewarm(ctx_, &DualMoeEngine::c_moe_prewarm_fetch, this);
            const double pw_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t_pw0).count();
            append_backend_log(GGML_LOG_LEVEL_INFO,
                ("[load] прогрев VRAM-банка: " + std::to_string(prewarmed_vram_b >> 20) + " МБ за " +
                 std::to_string((int) pw_ms) + " мс\n").c_str());
        }

        const float hyst = cfg.auto_balance_compute ? std::max(1.05f, cfg.swap_hysteresis) : cfg.swap_hysteresis;
        llama_set_moe_cache_swap_cbs(
            ctx_,
            disjoint_active ? &DualMoeEngine::c_moe_evict_cb : nullptr,
            &DualMoeEngine::c_moe_promoted_cb,
            hyst,
            this);

        if (stream_moe) {
            arena_.set_vram_rescue_callback([this](int32_t il, int32_t exp) -> bool {
                return ctx_ ? llama_moe_cache_rescue_expert(ctx_, il, exp, &DualMoeEngine::c_moe_rescue_cb, this) : false;
            });
        }
    }

    if (cfg.zero_hdd_mode && stream_moe) {
        const uint64_t max_ram_bytes = (uint64_t) cfg.ram_cache_mb << 20;
        preloaded_ram_b = arena_.preload_remaining_to_ram_sequential(
            max_ram_bytes,
            [this, prewarmed_vram_b](uint64_t done_b, uint64_t target_b, double mbs) {
                const int32_t sub_pct = 90 + (int32_t) (9ull * done_b / std::max<uint64_t>(1, target_b));
                load_progress_pct_.store(std::clamp<int32_t>(sub_pct, 90, 99), std::memory_order_relaxed);
                std::ostringstream prog;
                prog << "Линейная загрузка в ОЗУ: " << (done_b >> 20) << "/" << (target_b >> 20)
                     << " МБ (" << (int) mbs << " МБ/с"
                     << (prewarmed_vram_b > 0 ? (", в VRAM уже прогрето " + std::to_string(prewarmed_vram_b >> 20) + " МБ") : "")
                     << ")";
                std::lock_guard<std::mutex> slk(state_mu_);
                diag_.progress_pct = std::clamp<int32_t>(sub_pct, 90, 99);
                if (diag_.steps.size() >= 8) {
                    diag_.steps[7].detail = prog.str();
                }
                status_text_ = prog.str();
            });
        arena_.mark_preload_complete();
    }

    // Verify whether 100% of the model now lives in Silicon (VRAM + DDR4 RAM) with 0 runtime HDD reads needed
    bool full_silicon_resident = false;
    if (stream_moe) {
        const uint64_t in_silicon = arena_.silicon_resident_bytes(prewarmed_vram_b);
        full_silicon_resident = cfg.zero_hdd_mode && (in_silicon + (4ull << 20) >= arena_.total_expert_bytes());
    } else {
        full_silicon_resident = cfg.zero_hdd_mode;
    }

    const float init_share = cparams.moe_cache_size > 0 ? cparams.moe_cache_cpu_share : 1.0f;
    active_cpu_share_.store(init_share, std::memory_order_relaxed);

    // Forced GPU || CPU seam: without it the CPU only works once the VRAM bank is full, so on a model
    // that fits the bank the GPU does 100% and the seam never forms. Negative disables, as the llama API
    // expects; the share itself is clamped to a sane range so the CPU cannot become the bottleneck.
    if (ctx_) {
        const float seam = std::clamp(cfg.parallel_seam_share, 0.0f, 0.9f);
        llama_set_moe_cache_forced_seam(ctx_, seam > 0.02f ? seam : -1.0f);
        active_seam_share_.store(seam > 0.02f ? seam : 0.0f, std::memory_order_relaxed);
    }

    std::string step8_summary;
    {
        std::lock_guard<std::mutex> slk(state_mu_);
        cfg_                  = cfg;
        layout_               = std::move(lay);
        active_vram_cache_mb_ = (uint64_t) (cparams.moe_cache_size >> 20);
        vram_prewarmed_bytes_ = prewarmed_vram_b;
        ram_preloaded_bytes_  = preloaded_ram_b;
        auto_split_active_    = split_experts;
        parallel_gpu_cpu_     = (cparams.moe_cache_size > 0 && cparams.moe_cache_cpu_share > 0.0f) ||
                                (!layout_.summary.is_moe && effective_ngl > 0 && effective_ngl < n_layer);
        zero_hdd_active_      = full_silicon_resident;
        disjoint_swap_active_ = disjoint_active;
        auto_balance_active_  = cfg.auto_balance_compute && (cparams.moe_cache_size > 0);
        active_gpu_layers_    = effective_ngl;
        ema_gpu_hit_ratio_    = 0.75f;
        balance_events_       = 0;
        active_ubatch_        = cparams.n_ubatch;
        vram_reserve_mb_      = budget.ok ? budget.vram_reserve_mb
                                          : resolve_vram_reserve_mb(pre_mem.vram_total_mb,
                                                                    (int64_t) cfg.vram_reserve_mb);
        ram_reserve_mb_       = budget.ok ? budget.ram_reserve_mb
                                          : resolve_ram_reserve_mb(pre_mem.ram_total_mb, (int64_t) cfg.ram_reserve_mb);
        auto_budget_active_   = auto_budget;

        if (full_silicon_resident && parallel_gpu_cpu_) {
            status_text_ = std::string("Готов • Без HDD (100% в VRAM") +
                           (disjoint_active ? " ⊕ " : " + ") +
                           "RAM) • Макс. параллелизм GPU ∥ CPU";
        } else if (parallel_gpu_cpu_) {
            status_text_ = "Готов • Параллельный режим GPU (VRAM) ∥ CPU (DDR4)";
        } else {
            status_text_ = "Готов • Модель загружена в память";
        }
        diag_.loading       = false;
        diag_.current_stage = status_text_;

        std::ostringstream s8;
        if (stream_moe && cfg.zero_hdd_mode) {
            s8 << "В VRAM прогрето " << (prewarmed_vram_b >> 20) << " МБ, в DDR4 загружено "
               << (preloaded_ram_b >> 20) << " МБ (" << (int) arena_.preload_bandwidth_mbs() << " МБ/с)"
               << (disjoint_active ? " • Раздельный обмен VRAM ⊕ RAM активен" : " • Быстрый обмен VRAM ⇄ RAM активен")
               << (full_silicon_resident ? " • 0 обращений к HDD в чате!" : "");
        } else {
            s8 << "Модель полностью готова к работе и принимает запросы";
        }
        step8_summary = s8.str();
    }

    // Warm start: the hot experts of the previous run of this model become the pinned static class of
    // the VRAM bank, and their DDR4 copies are pre-touched, so the first answer after a start already
    // runs at the steady-state speed instead of learning the routing from scratch.
    if (stream_moe && cfg.warm_state && cparams.moe_cache_size > 0) {
        load_warm_state(cfg);
        if (warm_state_loaded_) {
            apply_static_class();
            std::lock_guard<std::mutex> wl(warm_mu_);
            for (size_t il = 0; il < static_experts_.size(); ++il) {
                const auto & exps = static_experts_[il];
                if (!exps.empty() && arena_.has_layer((int32_t) il)) {
                    arena_.prefetch_experts_async((int32_t) il, exps.data(), exps.size());
                }
            }
        }
    } else if (stream_moe && cfg.warm_state) {
        // no VRAM bank (CPU + DDR4): still restore the hot set so the arena starts warm
        load_warm_state(cfg);
    }

    // Speculative lookahead table: load the one from the previous run (instant speed-up) or start
    // growing a fresh one from this session's own token history.
    spec_load_or_build(cfg);

    finish_step(8, "done", step8_summary);
    mark_flight_header("READY", "Модель успешно загружена");
    loaded_.store(true, std::memory_order_release);
    loading_.store(false, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------------------
// Warm expert state + static expert class
//
// The VRAM expert bank is a ClassLRU over (layer, expert). Two problems are fixed here:
//   * "cold start": every run re-learns the routing, so the first answers are slow — the hot set
//     of the previous run is stored next to the model and pre-pinned on load;
//   * "topic shift": the experts of the structure / system prompt are evicted together with the
//     rest, so the *cheap* part of the model thrashes — a share of the bank is reserved for them.
// ---------------------------------------------------------------------------

std::string DualMoeEngine::warm_state_path(const std::string & model_path) const {
    if (model_path.empty()) return {};
    std::filesystem::path p = std::filesystem::u8path(model_path);
    std::filesystem::path dir = p.parent_path();
    const std::string stem = fs_path_to_utf8(p.filename()) + ".warm";
    // Prefer a sidecar next to the model; fall back to the runtime dir when the folder is read-only.
    std::error_code ec;
    if (!dir.empty() && std::filesystem::is_directory(dir, ec) && !ec) {
        // Write-probe with a throwaway name: never touch an existing warm-state file of the model.
        std::filesystem::path target = dir / stem;
        std::filesystem::path probe  = dir / (stem + ".probe");
        FILE * f = open_utf8_file(fs_path_to_utf8(probe), "ab");
        if (f) {
            std::fclose(f);
            std::filesystem::remove(probe, ec);
            return fs_path_to_utf8(target);
        }
    }
    return fs_path_to_utf8(resolve_runtime_dir() / stem);
}

void DualMoeEngine::load_warm_state(const EngineConfig & cfg) {
    warm_state_loaded_ = false;
    if (!cfg.warm_state || cfg.model_path.empty()) return;

    std::string path = warm_state_path(cfg.model_path);
    if (path.empty()) return;
    FILE * f = open_utf8_file(path, "rb");
    append_backend_log(GGML_LOG_LEVEL_INFO, (std::string("[warm] читаю ") + path + (f ? " (ok)\n" : " (нет файла)\n")).c_str());
    if (!f) return;

    const size_t max_bytes = (size_t) std::max<int32_t>(1, cfg.warm_state_max_kb) * 1024ull;
    std::string data;
    char buf[4096];
    while (data.size() < max_bytes) {
        const size_t got = std::fread(buf, 1, sizeof(buf), f);
        if (got == 0) break;
        data.append(buf, got);
    }
    std::fclose(f);

    // Format (line based, tolerant to a truncated file):
    //   dualmoe-warm 1
    //   model <sha-ish fingerprint: file size + layer/expert count>
    //   route <layer> <e0> <e1> ...
    //   hot <layer> <expert> <crf>
    const size_t nl = data.find('\n');
    if (nl == std::string::npos || data.compare(0, 12, "dualmoe-warm") != 0) return;

    const std::string expect_model =
        std::to_string(layout_.summary.file_size_bytes) + "/" +
        std::to_string(layout_.summary.n_layer) + "/" +
        std::to_string(layout_.summary.n_expert);
    const int32_t n_layer = std::max<int32_t>(1, layout_.summary.n_layer);
    const int32_t n_expert = std::max<int32_t>(1, layout_.summary.n_expert);

    std::vector<std::vector<int32_t>> route((size_t) n_layer);
    std::vector<std::pair<double, std::pair<int32_t, int32_t>>> hot; // (crf, (il, expert))
    bool model_ok = false;

    size_t pos = nl + 1;
    while (pos < data.size()) {
        size_t end = data.find('\n', pos);
        if (end == std::string::npos) end = data.size();
        const std::string line = data.substr(pos, end - pos);
        pos = end + 1;
        if (line.compare(0, 6, "model ") == 0) {
            model_ok = line.compare(6, std::string::npos, expect_model) == 0;
            if (!model_ok) return; // different model file: the state is meaningless
            continue;
        }
        if (line.compare(0, 6, "route ") == 0) {
            const char * c = line.c_str() + 6;
            char * e = nullptr;
            const long il = std::strtol(c, &e, 10);
            if (e == c || il < 0 || il >= n_layer) continue;
            auto & vec = route[(size_t) il];
            while (*e) {
                const long v = std::strtol(e, &e, 10);
                if (v >= 0 && v < n_expert) vec.push_back((int32_t) v);
            }
            continue;
        }
        if (line.compare(0, 4, "hot ") == 0) {
            const char * c = line.c_str() + 4;
            char * e = nullptr;
            const long il = std::strtol(c, &e, 10);
            const long ex = std::strtol(e, &e, 10);
            const double crf = std::strtod(e, &e);
            if (il >= 0 && il < n_layer && ex >= 0 && ex < n_expert && crf > 0.0) {
                hot.emplace_back(crf, std::make_pair((int32_t) il, (int32_t) ex));
            }
        }
    }
    if (!model_ok) return;

    // Hottest first: the caller pins them in this order and stops at the bank capacity.
    std::sort(hot.begin(), hot.end(), [](const auto & a, const auto & b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second < b.second;
    });
    static_experts_.assign((size_t) n_layer, {});
    for (const auto & h : hot) {
        auto & vec = static_experts_[(size_t) h.second.first];
        if (vec.size() < 64) vec.push_back(h.second.second);
    }
    prompt_route_ = std::move(route);
    warm_state_loaded_ = true;

    size_t n_hot = 0;
    for (const auto & v : static_experts_) n_hot += v.size();
    append_backend_log(GGML_LOG_LEVEL_INFO,
        ("Warm state: восстановлено " + std::to_string(n_hot) + " горячих экспертов из " + path + "\n").c_str());
}

std::string DualMoeEngine::spec_cache_path(const std::string & model_path) const {
    const std::string warm = warm_state_path(model_path);
    if (warm.empty()) return {};
    std::string p = warm;
    const std::string suffix = ".warm";
    if (p.size() > suffix.size() && p.compare(p.size() - suffix.size(), suffix.size(), suffix) == 0) {
        p.resize(p.size() - suffix.size());
    }
    return p + ".spec";
}

void DualMoeEngine::spec_load_or_build(const EngineConfig & cfg) {
    spec_.set_n_max(cfg.spec_tokens);
    spec_.clear();
    spec_drafted_  = 0;
    spec_accepted_ = 0;
    spec_graphs_   = 0;
    spec_emitted_  = 0;
    spec_accepted_ = 0;
    if (cfg.spec_tokens <= 0 || cfg.model_path.empty()) return;

    // 1. The lookahead table of this model from the previous run: instant speed-up on the first answer.
    if (cfg.spec_disk_cache && spec_.load(spec_cache_path(cfg.model_path))) {
        append_backend_log(GGML_LOG_LEVEL_INFO,
            "[spec] таблица разгона загружена с диска\n");
    }
    // 2. Seed the table from the warm expert state: its `route`/`hot` lines describe the same context
    //    the model just saw, so the very first answer already has something to draft from.
    spec_.observe({});
}

void DualMoeEngine::spec_save() {
    EngineConfig cfg;
    { std::lock_guard<std::mutex> slk(state_mu_); cfg = cfg_; }
    if (cfg.spec_tokens <= 0 || !cfg.spec_disk_cache || cfg.model_path.empty()) return;
    spec_.save(spec_cache_path(cfg.model_path));
}

void DualMoeEngine::save_warm_state() {
    EngineConfig cfg;
    std::string path;
    {
        std::lock_guard<std::mutex> slk(state_mu_);
        cfg = cfg_;
        path = warm_state_path(cfg_.model_path);
    }
    if (!cfg.warm_state || path.empty() || !ctx_) return;

    const int32_t n_layer = std::max<int32_t>(1, (int32_t) prev_route_.size());

    // The hot set is the union of the static class and the observed route, ordered by how often
    // the expert was selected. Every entry is one line so a truncated file stays readable.
    struct HotEntry {
        int32_t count;
        double  weight;   // 2.0 = came from the static class or a prompt route, 1.0 = decode route
        int32_t il;
        int32_t expert;
    };
    std::vector<HotEntry> hot;
    {
        std::lock_guard<std::mutex> wl(warm_mu_);
        std::vector<std::vector<int32_t>> merged((size_t) n_layer);
        for (int32_t il = 0; il < n_layer; ++il) {
            if ((size_t) il < static_experts_.size()) {
                for (int32_t e : static_experts_[(size_t) il]) merged[(size_t) il].push_back(e);
            }
            if ((size_t) il < prompt_route_.size()) {
                for (int32_t e : prompt_route_[(size_t) il]) merged[(size_t) il].push_back(e);
            }
            if ((size_t) il < prev_route_.size()) {
                for (int32_t e : prev_route_[(size_t) il]) merged[(size_t) il].push_back(e);
            }
        }
        for (int32_t il = 0; il < n_layer; ++il) {
            auto & v = merged[(size_t) il];
            std::sort(v.begin(), v.end());
            for (size_t i = 0; i < v.size(); ) {
                size_t j = i;
                while (j < v.size() && v[j] == v[i]) ++j;
                hot.push_back(HotEntry{(int32_t) (j - i), (double) (j - i), il, v[i]});
                i = j;
            }
        }
    }
    if (hot.empty()) return;

    std::sort(hot.begin(), hot.end(), [](const HotEntry & a, const HotEntry & b) {
        const double wa = (double) a.count * (0.5 + 0.5 * std::min(a.weight, 4.0));
        const double wb = (double) b.count * (0.5 + 0.5 * std::min(b.weight, 4.0));
        if (wa != wb) return wa > wb;
        if (a.il != b.il) return a.il < b.il;
        return a.expert < b.expert;
    });

    const size_t cap_kb = (size_t) std::max<int32_t>(8, cfg.warm_state_max_kb);
    std::string out;
    out.reserve(cap_kb * 1024);
    out += "dualmoe-warm 1\n";
    out += "model " + std::to_string(layout_.summary.file_size_bytes) + "/" +
           std::to_string(layout_.summary.n_layer) + "/" +
           std::to_string(layout_.summary.n_expert) + "\n";
    for (const auto & h : hot) {
        if (out.size() > cap_kb * 1024) break;
        out += "hot " + std::to_string(h.il) + " " + std::to_string(h.expert) +
               " " + std::to_string(h.count) + "\n";
    }

    FILE * f = open_utf8_file(path, "wb");
    if (!f) return;
    std::fwrite(out.data(), 1, out.size(), f);
    std::fclose(f);

    // Short warm-up needs the lookahead table on disk as well: it turns the very first answer of the
    // next session into a speculative one instead of a learning run.
    spec_save();
}

void DualMoeEngine::schedule_warm_state_save() {
    if (shutting_down_.load(std::memory_order_acquire)) return;
    if (warm_save_requested_.exchange(true, std::memory_order_acq_rel)) return;
    if (warm_save_thread_.joinable()) {
        warm_save_thread_.join(); // previous save already finished (operator== on id is enough)
    }
    warm_save_thread_ = std::thread([this]() {
        for (int i = 0; i < 30; ++i) { // let the answer finish streaming, but never wait forever
            if (shutting_down_.load(std::memory_order_acquire)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!generating_.load(std::memory_order_acquire)) break;
        }
        if (!shutting_down_.load(std::memory_order_acquire) && loaded_.load(std::memory_order_acquire)) {
            save_warm_state();
        }
        warm_save_requested_.store(false, std::memory_order_release);
    });
}

void DualMoeEngine::apply_static_class() {
    static_class_active_ = false;
    if (!ctx_) return;

    EngineConfig cfg;
    { std::lock_guard<std::mutex> slk(state_mu_); cfg = cfg_; }

    // The static class only exists when a VRAM bank exists and the user asked for a share of it.
    if (layout_.summary.n_expert <= 0 || active_vram_cache_mb_ == 0) return;

    size_t pin_mb = cfg.static_bank_mb;
    if (pin_mb == 0 && cfg.static_bank_share > 0.0f) {
        pin_mb = (size_t) ((double) active_vram_cache_mb_ * std::clamp((double) cfg.static_bank_share, 0.0, 0.8));
    }
    if (pin_mb < 64) return;

    // Experts hold their VRAM slots at ~1/4 of their size on the host, but a bank is filled with the
    // heaviest 3D expert tensor: derive how many slots the reservation covers.
    const int32_t n_layer = std::max<int32_t>(1, layout_.summary.n_layer);
    const int32_t n_expert = std::max<int32_t>(1, layout_.summary.n_expert);
    const uint64_t bytes_per_slot = std::max<uint64_t>(1, (uint64_t) std::max<uint64_t>(1, layout_.summary.expert_bytes) /
                                                          ((uint64_t) n_layer * (uint64_t) n_expert));
    const size_t want = (size_t) std::min<uint64_t>((uint64_t) pin_mb << 20, (uint64_t) -1) / bytes_per_slot;

    std::vector<int32_t> layer_ids, expert_ids;
    {
        std::lock_guard<std::mutex> wl(warm_mu_);
        for (int32_t il = 0; il < n_layer && layer_ids.size() < want; ++il) {
            if ((size_t) il >= static_experts_.size()) break;
            for (int32_t e : static_experts_[(size_t) il]) {
                if (layer_ids.size() >= want) break;
                layer_ids.push_back(il);
                expert_ids.push_back(e);
            }
        }
    }
    if (layer_ids.empty()) return;

    llama_set_moe_cache_pinned(ctx_, layer_ids.data(), expert_ids.data(), layer_ids.size());
    static_class_active_ = true;
    append_backend_log(GGML_LOG_LEVEL_INFO,
        ("Static-class: " + std::to_string(layer_ids.size()) + " экспертов закреплено в банке VRAM (" +
         std::to_string(pin_mb) + " МБ).\n").c_str());
}

bool DualMoeEngine::c_eval_callback(struct ggml_tensor * t, bool ask, void * user_data) {
    if (!user_data) return true;
    return static_cast<DualMoeEngine *>(user_data)->on_eval_node(t, ask);
}

void DualMoeEngine::c_moe_cache_callback(const struct llama_moe_cache_event * ev, void * user_data) {
    if (!user_data || !ev) return;
    static_cast<DualMoeEngine *>(user_data)->on_moe_cache_event(ev);
}

bool DualMoeEngine::c_moe_prewarm_fetch(int32_t il, int32_t ip, int32_t expert_id, const char * tensor_name, void * dst_buf, size_t nbytes, void * user_data) {
    (void) ip;
    if (!user_data) return false;
    return static_cast<DualMoeEngine *>(user_data)->arena_.fetch_expert_for_vram_prewarm(il, tensor_name, expert_id, dst_buf, nbytes);
}

void * DualMoeEngine::c_moe_evict_cb(int32_t evicted_il, int32_t ip, int32_t evicted_expert, const char * tensor_name, size_t nbytes, void * user_data) {
    (void) ip;
    if (!user_data) return nullptr;
    return static_cast<DualMoeEngine *>(user_data)->arena_.prepare_vram_eviction_dst(evicted_il, tensor_name, evicted_expert, nbytes);
}

void * DualMoeEngine::c_moe_rescue_cb(int32_t il, int32_t ip, int32_t expert_id, const char * tensor_name, size_t nbytes, void * user_data) {
    (void) ip;
    if (!user_data) return nullptr;
    return static_cast<DualMoeEngine *>(user_data)->arena_.prepare_vram_rescue_dst(il, tensor_name, expert_id, nbytes);
}

void DualMoeEngine::c_moe_promoted_cb(int32_t il, const int32_t * promoted_experts, size_t n_promoted, size_t n_evicted_to_ram, void * user_data) {
    if (!user_data) return;
    static_cast<DualMoeEngine *>(user_data)->arena_.on_experts_promoted_to_vram(il, promoted_experts, n_promoted, n_evicted_to_ram);
}

bool DualMoeEngine::c_abort_callback(void * user_data) {
    if (!user_data) return false;
    auto * eng = static_cast<DualMoeEngine *>(user_data);

    // A cancel that was raised while no graph was running is stale: disarm it on the first check of
    // the next graph instead of aborting it.
    const uint64_t disarm_at = eng->abort_disarm_at_.load(std::memory_order_acquire);
    if (disarm_at != 0 && !eng->generating_.load(std::memory_order_acquire)) {
        const bool fresh = (uint64_t) now_ms() - disarm_at < 400;
        if (!fresh) {
            eng->cancel_flag_.store(false, std::memory_order_release);
            eng->abort_disarm_at_.store(0, std::memory_order_release);
            return false;
        }
    }
    return eng->cancel_flag_.load(std::memory_order_acquire);
}

void DualMoeEngine::on_moe_cache_event(const struct llama_moe_cache_event * ev) {
    gpu_expert_hits_.fetch_add(ev->n_hits, std::memory_order_relaxed);
    gpu_expert_fills_.fetch_add(ev->n_fills, std::memory_order_relaxed);
    cpu_parallel_experts_.fetch_add(ev->n_cpu, std::memory_order_relaxed);
    vram_upload_bytes_.fetch_add(ev->upload_bytes, std::memory_order_relaxed);

    if (cfg_.moe_stream && arena_.has_layer(ev->il)) {
        if (ev->n_fills > 0 && ev->fill_experts) {
            arena_.ensure_experts_sync(ev->il, ev->fill_experts, ev->n_fills);
        }
        if (ev->n_cpu > 0 && ev->cpu_experts) {
            arena_.prefetch_experts_async(ev->il, ev->cpu_experts, ev->n_cpu);
        }
    }

    // Closed-Loop GPU || CPU Parallel Load Auto-Balancer:
    // - When GPU VRAM hit ratio is high (>= 70%), RX 580 is heavily loaded with cached experts -> route 88-93% of misses
    //   to Ryzen AVX2 in DDR4 RAM so both processors finish together with minimal PCIe traffic.
    // - When GPU VRAM hit ratio dips (< 48%, topic shift!), lower cpu_share to ~0.62 so hotter RAM experts rapidly
    //   promote into RX 580 VRAM and restore full GPU saturation!
    if (auto_balance_active_ && ctx_) {
        const size_t total_sel = ev->n_hits + ev->n_fills + ev->n_cpu;
        if (total_sel > 0) {
            const float inst_hit = (float) ev->n_hits / (float) total_sel;
            ema_gpu_hit_ratio_   = 0.90f * ema_gpu_hit_ratio_ + 0.10f * inst_hit;
            if ((++balance_events_ & 15ull) == 0ull) {
                float target_share = cfg_.cpu_share;
                if (ema_gpu_hit_ratio_ >= 0.72f) {
                    target_share = std::clamp(std::max(cfg_.cpu_share, 0.90f), 0.55f, 0.94f);
                } else if (ema_gpu_hit_ratio_ < 0.45f) {
                    target_share = std::clamp(std::min(cfg_.cpu_share, 0.64f), 0.50f, 0.88f);
                } else {
                    const float t = (ema_gpu_hit_ratio_ - 0.45f) / (0.72f - 0.45f);
                    target_share  = 0.64f + t * (0.90f - 0.64f);
                }
                const float prev = active_cpu_share_.load(std::memory_order_relaxed);
                if (std::abs(target_share - prev) >= 0.02f) {
                    active_cpu_share_.store(target_share, std::memory_order_relaxed);
                    llama_set_moe_cache_cpu_share(ctx_, target_share);
                }
            }
        }
    }
}

bool DualMoeEngine::on_eval_node(struct ggml_tensor * t, bool ask) {
    if (!t) return !ask;

    if (capture_mode_) {
        if (ask) {
            for (int s = 0; s < GGML_MAX_SRC; ++s) {
                ggml_tensor * src = t->src[s];
                if (!src) continue;
                ggml_tensor * leaf = src;
                while (leaf->view_src) leaf = leaf->view_src;
                if (leaf->op == GGML_OP_NONE && layout_.tensors.find(leaf->name) != layout_.tensors.end()) {
                    captured_weights_.insert(leaf);
                }
                const auto it = layout_.tensors.find(src->name);
                if (it != layout_.tensors.end() && it->second.is_expert && it->second.layer_idx >= 0) {
                    const bool host_ok = !src->buffer || ggml_backend_buffer_is_host(src->buffer);
                    if (host_ok && (size_t) it->second.layer_idx < captured_layer_experts_.size()) {
                        auto & vec = captured_layer_experts_[(size_t) it->second.layer_idx];
                        if (std::find(vec.begin(), vec.end(), src) == vec.end()) {
                            vec.push_back(src);
                        }
                    }
                }
            }
            return false;
        }
        return true;
    }

    int32_t il = -1;
    if (!parse_topk_node_layer(t->name, il)) {
        return ask ? false : true;
    }
    if (il >= 0 && (size_t) il < layer_on_gpu_cache_.size() && layer_on_gpu_cache_[(size_t) il]) {
        return ask ? false : true;
    }

    if (ask) {
        return true;
    }

    const int64_t n_used   = t->ne[0];
    const int64_t n_tokens = t->ne[1];
    if (n_used <= 0 || n_tokens <= 0) return true;

    std::vector<int32_t> selected;
    selected.reserve((size_t) (n_used * n_tokens));

    const bool on_dev = t->buffer && !ggml_backend_buffer_is_host(t->buffer);
    if (!on_dev && t->data) {
        const uint8_t * base = static_cast<const uint8_t *>(t->data);
        for (int64_t it = 0; it < n_tokens; ++it) {
            for (int64_t k = 0; k < n_used; ++k) {
                int32_t id = 0;
                std::memcpy(&id, base + it * t->nb[1] + k * t->nb[0], sizeof(int32_t));
                if (id >= 0) selected.push_back(id);
            }
        }
    } else if (ggml_is_contiguous(t)) {
        selected.resize((size_t) (n_used * n_tokens));
        ggml_backend_tensor_get(t, selected.data(), 0, selected.size() * sizeof(int32_t));
    }

    // NOTE: this is the number of *selected* experts (a routing statistic), not the number of experts
    // the CPU actually computed. The real GPU || CPU accounting comes from the MoE cache event and from
    // the split timeline (see record_parallel_seam), so no counter is bumped here.

    if (cfg_.moe_stream && arena_.has_layer(il) && !selected.empty()) {
        arena_.prefetch_experts_async(il, selected.data(), selected.size());
        if ((size_t) il < prev_route_.size()) {
            prev_route_[(size_t) il] = selected;
        }
        // Prefill routes (n_tokens > 1) are what a repeated prefix will hit again on the next turn,
        // so they are kept apart from the single-token decode route used for layer-ahead prefetch.
        if (n_tokens > 1) {
            std::lock_guard<std::mutex> wl(warm_mu_);
            if ((size_t) il < prompt_route_.size()) {
                auto & dst = prompt_route_[(size_t) il];
                dst.insert(dst.end(), selected.begin(), selected.end());
            }
        }
        // Prefetch the next layer twice when the prompt is processed: one token of look-ahead is
        // enough for decode, but a prefill ubatch benefits from the whole next layer being ready.
        const int32_t lookahead = n_tokens > 1 ? cfg_.prefetch_ahead + 2 : cfg_.prefetch_ahead;
        for (int32_t step = 1; step <= lookahead; ++step) {
            const int32_t next_il = il + step;
            if ((size_t) next_il >= prev_route_.size() || !arena_.has_layer(next_il)) break;
            if (prev_route_[(size_t) next_il].empty()) continue;
            arena_.prefetch_experts_async(next_il, prev_route_[(size_t) next_il].data(),
                                          prev_route_[(size_t) next_il].size());
        }
    }
    return true;
}

namespace {

void split_past_assistant_reasoning(const std::string & raw,
                                    std::string & reasoning_out,
                                    std::string & clean_out) {
    reasoning_out.clear();
    clean_out = raw;

    // 1. Gemma 4 format: <|channel>thought\n...<channel|>Answer
    const std::string g4_open  = "<|channel>thought";
    const std::string g4_close = "<channel|>";
    const size_t p_g4 = clean_out.find(g4_open);
    if (p_g4 != std::string::npos && p_g4 <= 4) {
        const size_t p_end = clean_out.find(g4_close, p_g4 + g4_open.size());
        if (p_end != std::string::npos) {
            size_t r_start = p_g4 + g4_open.size();
            if (r_start < p_end && clean_out[r_start] == '\n') ++r_start;
            reasoning_out = clean_out.substr(r_start, p_end - r_start);
            clean_out     = clean_out.substr(p_end + g4_close.size());
            return;
        }
    }

    // 2. Qwen3 / DeepSeek-R1 format: <think>...</think>Answer
    const std::string q_open  = "<think>";
    const std::string q_close = "</think>";
    const size_t p_q = clean_out.find(q_open);
    if (p_q != std::string::npos && p_q <= 4) {
        const size_t p_end = clean_out.find(q_close, p_q + q_open.size());
        if (p_end != std::string::npos) {
            size_t r_start = p_q + q_open.size();
            if (r_start < p_end && clean_out[r_start] == '\n') ++r_start;
            reasoning_out = clean_out.substr(r_start, p_end - r_start);
            size_t c_start = p_end + q_close.size();
            while (c_start < clean_out.size() && (clean_out[c_start] == '\n' || clean_out[c_start] == '\r')) {
                ++c_start;
            }
            clean_out = clean_out.substr(c_start);
        }
    }
}

size_t longest_stop_prefix_tail(const std::string & s, const std::vector<std::string> & stops) {
    size_t hold = 0;
    for (const auto & st : stops) {
        if (st.size() <= 1) continue;
        const size_t max_k = std::min(s.size(), st.size() - 1);
        for (size_t k = max_k; k >= 1; --k) {
            if (s.compare(s.size() - k, k, st, 0, k) == 0) {
                if (k > hold) hold = k;
                break;
            }
        }
    }
    return hold;
}

} // namespace

std::string DualMoeEngine::format_chat_prompt(const std::vector<ChatMessage> & messages) const {
    if (messages.empty()) return "";

    std::string arch;
    bool enable_thinking = true;
    {
        std::lock_guard<std::mutex> slk(state_mu_);
        arch            = layout_.summary.arch;
        enable_thinking = cfg_.enable_thinking;
    }

    // Architecture-native fallback for Gemma 4 if GGUF omitted explicit Jinja metadata
    if (arch.rfind("gemma4", 0) == 0) {
        std::ostringstream oss;
        bool system_emitted = false;
        for (const auto & m : messages) {
            std::string reasoning, clean;
            if (m.role == "assistant") {
                split_past_assistant_reasoning(m.content, reasoning, clean);
            } else {
                clean = m.content;
            }
            if (m.role == "system" && !system_emitted) {
                oss << "<|turn>system\n";
                if (enable_thinking) {
                    oss << "<|think|>";
                    if (!clean.empty()) oss << "\n" << clean;
                } else {
                    oss << clean;
                }
                oss << "<turn|>\n";
                system_emitted = true;
            } else {
                if (!system_emitted && enable_thinking) {
                    oss << "<|turn>system\n<|think|><turn|>\n";
                    system_emitted = true;
                }
                const std::string g_role = (m.role == "assistant") ? "model" : m.role;
                oss << "<|turn>" << g_role << "\n";
                if (m.role == "assistant") {
                    oss << "<|channel>thought\n<channel|>" << clean;
                } else {
                    oss << clean;
                }
                oss << "<turn|>\n";
            }
        }
        if (!system_emitted && enable_thinking) {
            oss << "<|turn>system\n<|think|><turn|>\n";
        }
        oss << "<|turn>model\n";
        if (!enable_thinking) {
            oss << "<|channel>thought\n<channel|>";
        }
        return oss.str();
    }

    std::vector<std::pair<std::string, std::string>> cleaned_msgs;
    cleaned_msgs.reserve(messages.size());
    std::vector<llama_chat_message> raw;
    raw.reserve(messages.size());
    size_t total_chars = 0;
    for (const auto & m : messages) {
        std::string reasoning, clean;
        if (m.role == "assistant") {
            split_past_assistant_reasoning(m.content, reasoning, clean);
        } else {
            clean = m.content;
        }
        cleaned_msgs.emplace_back(m.role, std::move(clean));
    }
    for (const auto & cm : cleaned_msgs) {
        raw.push_back({cm.first.c_str(), cm.second.c_str()});
        total_chars += cm.first.size() + cm.second.size() + 32;
    }

    const char * tmpl = model_ ? llama_model_chat_template(model_, nullptr) : nullptr;
    std::vector<char> buf(std::max<size_t>(1024, total_chars * 2 + 256));
    int32_t res = llama_chat_apply_template(tmpl, raw.data(), raw.size(), true, buf.data(), (int32_t) buf.size());
    if (res > (int32_t) buf.size()) {
        buf.resize((size_t) res + 64);
        res = llama_chat_apply_template(tmpl, raw.data(), raw.size(), true, buf.data(), (int32_t) buf.size());
    }
    if (res > 0) {
        return std::string(buf.data(), (size_t) res);
    }

    std::ostringstream oss;
    for (const auto & cm : cleaned_msgs) {
        oss << "<|im_start|>" << cm.first << "\n" << cm.second << "<|im_end|>\n";
    }
    oss << "<|im_start|>assistant\n";
    return oss.str();
}

bool DualMoeEngine::generate_chat(const std::vector<ChatMessage> & messages,
                                  const SamplingParams & sampling_override,
                                  bool use_override,
                                  const TokenCallback & on_token,
                                  std::string & full_reply,
                                  int32_t & prompt_tokens_out,
                                  int32_t & completion_tokens_out,
                                  std::string & err) {
    if (loading_.load(std::memory_order_acquire)) {
        err = "Модель сейчас загружается — дождитесь завершения шага 8/8";
        return false;
    }
    std::lock_guard<std::mutex> lk(api_mu_);
    if (!loaded_.load(std::memory_order_acquire) || !ctx_ || !vocab_) {
        err = "Модель ещё не загружена в движок";
        return false;
    }

    bool enable_thinking = true;
    {
        std::lock_guard<std::mutex> slk(state_mu_);
        enable_thinking = cfg_.enable_thinking;
    }

    std::vector<llama_token> tokens;
    std::vector<std::string> stop_strings = {"<turn|>", "<end_of_turn>", "<|eot_id|>", "<|im_end|>"};
    const bool want_bos = llama_vocab_get_add_bos(vocab_);
    const llama_token bos_id = llama_vocab_bos(vocab_);

    bool jinja_ok = false;
    if (chat_templates_ && common_chat_templates_was_explicit(chat_templates_)) {
        try {
            common_chat_templates_inputs inputs;
            inputs.add_generation_prompt = true;
            inputs.use_jinja             = true;
            inputs.enable_thinking       = enable_thinking;
            inputs.add_bos               = want_bos;
            inputs.add_eos               = llama_vocab_get_add_eos(vocab_);
            inputs.messages.reserve(messages.size());

            for (const auto & m : messages) {
                common_chat_msg cm;
                cm.role = m.role;
                if (m.role == "assistant") {
                    split_past_assistant_reasoning(m.content, cm.reasoning_content, cm.content);
                } else {
                    cm.content = m.content;
                }
                inputs.messages.push_back(std::move(cm));
            }

            common_chat_params cp = common_chat_templates_apply(chat_templates_, inputs);
            for (const auto & st : cp.additional_stops) {
                if (!st.empty() && std::find(stop_strings.begin(), stop_strings.end(), st) == stop_strings.end()) {
                    stop_strings.push_back(st);
                }
            }

            for (const auto & part : cp.prompt.parts) {
                if (part.text.empty()) continue;
                auto seg = common_tokenize(vocab_, part.text, /*add_special=*/false, /*parse_special=*/!part.is_input);
                tokens.insert(tokens.end(), seg.begin(), seg.end());
            }

            // Guarantee exact single BOS when vocab mandates add_bos (never duplicate, never omit)
            if (want_bos && bos_id != LLAMA_TOKEN_NULL) {
                if (tokens.empty() || tokens[0] != bos_id) {
                    tokens.insert(tokens.begin(), bos_id);
                } else if (tokens.size() >= 2 && tokens[0] == bos_id && tokens[1] == bos_id) {
                    tokens.erase(tokens.begin());
                }
            }
            jinja_ok = !tokens.empty();
        } catch (const std::exception & e) {
            append_backend_log(GGML_LOG_LEVEL_WARN, (std::string("[Jinja2] Fallback chat formatter: ") + e.what() + "\n").c_str());
            tokens.clear();
        } catch (...) {
            tokens.clear();
        }
    }

    if (!jinja_ok) {
        const std::string prompt = format_chat_prompt(messages);
        tokens = common_tokenize(vocab_, prompt, /*add_special=*/want_bos, /*parse_special=*/true);
        if (want_bos && bos_id != LLAMA_TOKEN_NULL && tokens.size() >= 2 && tokens[0] == bos_id && tokens[1] == bos_id) {
            tokens.erase(tokens.begin());
        }
    }

    if (tokens.empty()) {
        err = "Не удалось токенизировать запрос";
        return false;
    }

    const SamplingParams sp = use_override ? sampling_override : cfg_.sampling;
    const bool ok = run_tokens(tokens, sp, stop_strings, on_token, full_reply,
                               prompt_tokens_out, completion_tokens_out, err);
    if (ok) {
        // First session (no warm state yet): the experts that the structure + system prompt selected
        // are exactly the static class worth pinning for the rest of the session.
        if (!static_class_active_ && ctx_ && active_vram_cache_mb_ > 0) {
            std::lock_guard<std::mutex> wl(warm_mu_);
            for (size_t il = 0; il < prompt_route_.size(); ++il) {
                auto & src = prompt_route_[il];
                if (src.empty() || il >= static_experts_.size()) continue;
                std::sort(src.begin(), src.end());
                src.erase(std::unique(src.begin(), src.end()), src.end());
                static_experts_[il] = src;
            }
            apply_static_class();
        }
        schedule_warm_state_save();
    }
    return ok;
}

bool DualMoeEngine::generate_raw(const std::string & prompt,
                                 int32_t max_tokens,
                                 float temperature,
                                 uint32_t seed,
                                 const TokenCallback & on_token,
                                 std::string & full_reply,
                                 std::string & err) {
    std::lock_guard<std::mutex> lk(api_mu_);
    if (!loaded_.load(std::memory_order_acquire) || !ctx_ || !vocab_) {
        err = "Модель не загружена";
        return false;
    }

    llama_memory_seq_rm(llama_get_memory(ctx_), -1, -1, -1);
    kv_tokens_.clear();

    const bool want_bos = llama_vocab_get_add_bos(vocab_);
    std::vector<llama_token> tokens = common_tokenize(vocab_, prompt, /*add_special=*/want_bos, /*parse_special=*/false);
    if (tokens.empty()) {
        llama_token bos = llama_vocab_bos(vocab_);
        tokens = {bos >= 0 ? bos : 0};
    }

    SamplingParams sp = cfg_.sampling;
    sp.max_tokens  = max_tokens;
    sp.temperature = temperature;
    sp.seed        = seed;

    int32_t p_out = 0, c_out = 0;
    std::vector<std::string> no_stops;
    return run_tokens(tokens, sp, no_stops, on_token, full_reply, p_out, c_out, err);
}

bool DualMoeEngine::run_tokens(const std::vector<llama_token> & tokens,
                               const SamplingParams & sp,
                               const std::vector<std::string> & stop_strings,
                               const TokenCallback & on_token,
                               std::string & full_reply,
                               int32_t & prompt_tokens_out,
                               int32_t & completion_tokens_out,
                               std::string & err) {
    full_reply.clear();
    prompt_tokens_out     = (int32_t) tokens.size();
    completion_tokens_out = 0;
    cancel_flag_.store(false, std::memory_order_release);
    generating_.store(true, std::memory_order_release);

    const uint32_t n_ctx = llama_n_ctx(ctx_);
    const int32_t  max_new = std::max<int32_t>(1, sp.max_tokens);
    if (tokens.size() + 8 >= n_ctx) {
        generating_.store(false, std::memory_order_release);
        err = "Длина промпта превышает размер контекста (увеличьте Context Budget)";
        return false;
    }

    size_t common_prefix = 0;
    const size_t max_prefix = std::min(kv_tokens_.size(), tokens.size() > 0 ? tokens.size() - 1 : 0);
    while (common_prefix < max_prefix && kv_tokens_[common_prefix] == tokens[common_prefix]) {
        ++common_prefix;
    }
    if (common_prefix < kv_tokens_.size()) {
        if (!llama_memory_seq_rm(llama_get_memory(ctx_), 0, (llama_pos) common_prefix, -1)) {
            llama_memory_seq_rm(llama_get_memory(ctx_), -1, -1, -1);
            common_prefix = 0;
        }
        kv_tokens_.resize(common_prefix);
    }

    // A new prompt starts a new route recording. The route of the previous prompt is the best free
    // predictor for the same (or a repeated) prefix, so its experts are pushed into the arena now,
    // while the prompt is still being tokenised/queued — the first prefill ubatch finds them in RAM.
    {
        std::vector<std::vector<int32_t>> prev_prompt;
        {
            std::lock_guard<std::mutex> wl(warm_mu_);
            prev_prompt = prompt_route_;
            for (auto & v : prompt_route_) v.clear();
        }
        if (cfg_.prefetch_ahead > 0) {
            for (size_t il = 0; il < prev_prompt.size(); ++il) {
                if (prev_prompt[il].empty() || !arena_.has_layer((int32_t) il)) continue;
                arena_.prefetch_experts_async((int32_t) il, prev_prompt[il].data(), prev_prompt[il].size());
            }
        }
    }

    const auto t_prefill_start = std::chrono::steady_clock::now();
    const size_t batch_cap = std::max<size_t>(32, llama_n_batch(ctx_));
    llama_batch batch = llama_batch_init((int32_t) batch_cap, 0, 1);

    size_t pos = common_prefix;
    while (pos < tokens.size()) {
        const size_t chunk = std::min(batch_cap, tokens.size() - pos);
        batch.n_tokens = (int32_t) chunk;
        for (size_t i = 0; i < chunk; ++i) {
            batch.token[i]     = tokens[pos + i];
            batch.pos[i]       = (llama_pos) (pos + i);
            batch.n_seq_id[i]  = 1;
            batch.seq_id[i][0] = 0;
            batch.logits[i]    = (pos + i + 1 == tokens.size()) ? 1 : 0;
        }
        const int rc = llama_decode(ctx_, batch);
        arena_.flush_deferred_promotions();
        if (rc != 0) {
            llama_batch_free(batch);
            generating_.store(false, std::memory_order_release);
            if (cancel_flag_.load(std::memory_order_acquire)) {
                err = "Остановлено пользователем";
                return false;
            }
            err = "Ошибка декодирования промпта (код " + std::to_string(rc) + ")";
            return false;
        }
        for (size_t i = 0; i < chunk; ++i) {
            kv_tokens_.push_back(tokens[pos + i]);
        }
        pos += chunk;
        kv_used_.store((uint32_t) kv_tokens_.size(), std::memory_order_relaxed);
    }

    const auto t_prefill_end = std::chrono::steady_clock::now();
    const double prefill_ms = std::chrono::duration<double, std::milli>(t_prefill_end - t_prefill_start).count();
    const size_t evaluated_prompt = tokens.size() - common_prefix;
    ttft_ms_.store(prefill_ms, std::memory_order_relaxed);
    if (prefill_ms > 0.5 && evaluated_prompt > 0) {
        prefill_tps_.store((double) evaluated_prompt * 1000.0 / prefill_ms, std::memory_order_relaxed);
    }
    prompt_tokens_.fetch_add(evaluated_prompt, std::memory_order_relaxed);

    llama_sampler * smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (sp.temperature <= 0.0f) {
        llama_sampler_chain_add(smpl, llama_sampler_init_greedy());
    } else {
        // Put top_k FIRST so 262,144-token vocabularies (Gemma 4 / Qwen3) narrow to ~64 candidates in 1 pass
        // before running DRY phrase anti-loop & repetition penalties!
        if (sp.top_k > 0) {
            llama_sampler_chain_add(smpl, llama_sampler_init_top_k(sp.top_k));
        }
        if (sp.dry_multiplier > 0.01f) {
            static const char * kDryBreakers[] = {"\n"};
            llama_sampler_chain_add(smpl, llama_sampler_init_dry(
                vocab_, sp.dry_multiplier, 1.75f, 3, 256, kDryBreakers, 1));
        }
        if (sp.repeat_penalty > 1.001f && sp.repeat_last_n > 0) {
            llama_sampler_chain_add(smpl, llama_sampler_init_penalties(
                llama_vocab_n_tokens(vocab_), sp.repeat_last_n, sp.repeat_penalty, 0.0f, 0.0f));
        }
        if (sp.top_p < 1.0f) llama_sampler_chain_add(smpl, llama_sampler_init_top_p(sp.top_p, 1));
        if (sp.min_p > 0.0f) llama_sampler_chain_add(smpl, llama_sampler_init_min_p(sp.min_p, 1));
        llama_sampler_chain_add(smpl, llama_sampler_init_temp(sp.temperature));
        llama_sampler_chain_add(smpl, llama_sampler_init_dist(sp.seed));
    }

    if (sp.temperature > 0.0f) {
        const size_t pen_tail = std::min<size_t>(tokens.size(), (size_t) std::max<int32_t>(256, sp.repeat_last_n));
        for (size_t i = tokens.size() - pen_tail; i < tokens.size(); ++i) {
            llama_sampler_accept(smpl, tokens[i]);
        }
    }

    std::string utf8_pending;
    bool hit_stop_string = false;
    const auto t_decode_start = std::chrono::steady_clock::now();

    // Speculative ("Разгон") bookkeeping.
    //
    // KV-cache invariant of the decode loop: every emitted token is in the cache. That is what makes
    // the next token free (it is sampled from the cached logits), and it is why a verify graph pays
    // for itself: one graph consumes the pending token plus N drafted candidates and, with the logits
    // rows it leaves behind, confirms up to N of them -- 1+confirmed tokens for the price of one.
    //
    // Row indexing (llama.cpp): the row of the token at position p predicts the token at position
    // p+1. So with the batch [next_tok, cand[0]..cand[N-1]] at base..base+N, row i predicts cand[i]
    // for i in [0, N], where row N predicts the token that follows the last candidate.
    bool spec_on = cfg_.spec_tokens > 0 && spec_.enabled();
    // The verify batch is 1 + spec_n tokens, so it must never exceed the context batch size.
    const int32_t spec_batch_cap = std::max<int32_t>(2, (int32_t) cfg_.n_batch - 1);
    const int32_t spec_n = std::clamp(cfg_.spec_tokens, 2, spec_batch_cap);
    spec_.reset_stats();
    std::deque<llama_token> pending;    // emitted next, already inside the KV cache
    uint64_t n_graphs  = 0;             // decode graphs spent on this answer
    uint64_t n_emitted = 0;             // tokens emitted on this answer
    // Measured profitability: speculation only wins when a graph confirms more than one draft token,
    // so a model that rejects them is served by plain decode instead of paying extra graphs.
    // NOTE: the guard must look at verify rounds, not at graphs, or it would disable itself after a
    // handful of plain tokens before the drafter ever gets a chance to prove itself.
    auto spec_profitable = [&]() {
        return spec_on && spec_.effective();
    };

    for (int32_t step = 0; step < max_new; ++step) {
        if (cancel_flag_.load(std::memory_order_acquire)) break;
        if (kv_tokens_.size() + (spec_on ? (size_t) spec_n + 2 : 2) >= n_ctx) {
            spec_on = false; // out of context: finish the answer with plain decode
        }
        if (kv_tokens_.size() + 2 >= n_ctx) break;

        // Tokens confirmed by the previous graph are already inside the KV cache: emitting them
        // costs nothing, and only after the queue drains does the model need another graph.
        llama_token next_tok;
        if (!pending.empty()) {
            next_tok = pending.front();
            pending.pop_front();
        } else {
            next_tok = llama_sampler_sample(smpl, ctx_, -1);
            llama_sampler_accept(smpl, next_tok);
        }

        if (llama_vocab_is_eog(vocab_, next_tok)) {
            break;
        }

        // Preserve non-EOG special delimiters (<|channel>, <channel|>, <think>, </think>) intact!
        const std::string piece = common_token_to_piece(vocab_, next_tok, /*special=*/true);
        utf8_pending += piece;

        // Stop-string check before emitting text to UI
        for (const auto & st : stop_strings) {
            if (!st.empty()) {
                const size_t p_st = utf8_pending.find(st);
                if (p_st != std::string::npos) {
                    utf8_pending.resize(p_st);
                    hit_stop_string = true;
                    break;
                }
            }
        }
        if (hit_stop_string) {
            break;
        }

        const size_t utf8_tail = incomplete_utf8_tail(utf8_pending);
        const size_t stop_tail = stop_strings.empty() ? 0 : longest_stop_prefix_tail(utf8_pending, stop_strings);
        const size_t hold_tail = std::max(utf8_tail, stop_tail);
        if (utf8_pending.size() > hold_tail) {
            const std::string valid_chunk = utf8_pending.substr(0, utf8_pending.size() - hold_tail);
            utf8_pending.erase(0, valid_chunk.size());
            full_reply += valid_chunk;
            if (on_token && !on_token(valid_chunk)) {
                cancel_flag_.store(true, std::memory_order_release);
                break;
            }
        }

        ++completion_tokens_out;
        ++n_emitted;
        generated_tokens_.fetch_add(1, std::memory_order_relaxed);

        const auto now = std::chrono::steady_clock::now();
        const double dec_ms = std::chrono::duration<double, std::milli>(now - t_decode_start).count();
        if (dec_ms > 1.0 && completion_tokens_out > 0) {
            decode_tps_.store((double) completion_tokens_out * 1000.0 / dec_ms, std::memory_order_relaxed);
        }

        if (step + 1 >= max_new) break;

        // The queue still holds confirmed tokens that are already inside the KV cache: emitting them
        // is free, so no graph is spent until it drains.
        if (!pending.empty()) continue;

        // ---------------------------------------------------------------- feed the pending token
        // `next_tok` was emitted above and is not in the KV cache yet, so one graph must consume it.
        // The drafter proposes the tokens that most often followed this history; the graph verifies
        // them for free, and only the confirmed prefix is kept, so the emitted text stays exactly
        // what plain greedy decode would have produced.
        bool speculated = false;
        if (spec_on && spec_profitable() &&
            (int) kv_tokens_.size() + spec_n + 2 < (int) n_ctx) {
            std::vector<int32_t> ctx_tokens(kv_tokens_.begin(), kv_tokens_.end());
            ctx_tokens.push_back(next_tok);
            std::vector<int32_t> cand = spec_.draft(ctx_tokens);
            if ((int32_t) cand.size() > spec_n) cand.resize((size_t) spec_n);
            if (cand.size() >= 2) {
                const int32_t n_verify = (int32_t) cand.size();
                const llama_pos base   = (llama_pos) kv_tokens_.size();
                batch.n_tokens = 1 + n_verify;
                batch.token[0] = next_tok;
                batch.pos[0]   = base;
                batch.n_seq_id[0] = 1;
                batch.seq_id[0][0] = 0;
                batch.logits[0] = 1;
                for (int32_t i = 0; i < n_verify; ++i) {
                    batch.token[1 + i]     = cand[(size_t) i];
                    batch.pos[1 + i]       = base + 1 + i;
                    batch.n_seq_id[1 + i]  = 1;
                    batch.seq_id[1 + i][0] = 0;
                    batch.logits[1 + i]    = 1;
                }
                const int rc = llama_decode(ctx_, batch);
                arena_.flush_deferred_promotions();
                ++n_graphs;
                record_parallel_seam();
                if (rc != 0) break;

                // Row i predicts the token after position base+i, i.e. it confirms cand[i], so the
                // number of confirmed candidates is the number of leading rows that agree.
                int32_t keep = 0;
                while (keep < n_verify) {
                    const llama_token pred = llama_sampler_sample(smpl, ctx_, keep);
                    if (pred != (llama_token) cand[(size_t) keep]) break;
                    llama_sampler_accept(smpl, pred);
                    ++keep;
                }
                // The confirmed prefix is base..base+keep; the graph wrote up to base+n_verify, so
                // every position after the prefix is history the model never predicted and goes.
                if (keep < n_verify &&
                    !llama_memory_seq_rm(llama_get_memory(ctx_), 0, base + 1 + keep, -1)) {
                    // Defensive: the cache cannot be trimmed in place, so rebuild it from the prompt
                    // instead of continuing on a history the model never predicted.
                    std::lock_guard<std::mutex> slk(state_mu_);
                    kv_tokens_.assign(tokens.begin(), tokens.end());
                    llama_memory_seq_rm(llama_get_memory(ctx_), -1, -1, -1);
                    pending.clear();
                    speculated = true;     // the token is consumed by the re-prefill below
                    continue;
                }
                kv_tokens_.push_back(next_tok);
                for (int32_t i = 0; i < keep; ++i) kv_tokens_.push_back((llama_token) cand[(size_t) i]);
                kv_used_.store((uint32_t) kv_tokens_.size(), std::memory_order_relaxed);

                spec_.add_drafts((uint64_t) n_verify);
                spec_.add_accepted((uint64_t) keep);
                spec_.recompute_efficiency();
                spec_drafted_  += (uint64_t) n_verify;
                spec_accepted_ += (uint64_t) keep;
                // Confirmed drafts are inside the cache and cost nothing to emit. Row `keep` predicts
                // the token that follows the last confirmed one, so it becomes pending as well: it
                // will be emitted for free and paid for by the next graph.
                for (int32_t i = 0; i < keep; ++i) pending.push_back((llama_token) cand[(size_t) i]);
                // Row `keep` holds the logits of the last confirmed position, so it already contains
                // the model's own next token: queuing it saves one more graph. It must be sampled
                // here even when it is EOG -- the rows past `keep` belong to the rejected draft and
                // their logits must never be used.
                const llama_token follow = llama_sampler_sample(smpl, ctx_, keep);
                llama_sampler_accept(smpl, follow);
                pending.push_back(follow);
                speculated = true;
            }
        }

        if (!speculated) {
            batch.logits[0]    = 1; // a previous verify round may have cleared it
            batch.n_tokens     = 1;
            batch.token[0]     = next_tok;
            batch.pos[0]       = (llama_pos) kv_tokens_.size();
            batch.n_seq_id[0]  = 1;
            batch.seq_id[0][0] = 0;
            batch.logits[0]    = 1;

            const int rc = llama_decode(ctx_, batch);
            arena_.flush_deferred_promotions();
            ++n_graphs;
            record_parallel_seam();
            if (rc != 0) break;
            kv_tokens_.push_back(next_tok);
            kv_used_.store((uint32_t) kv_tokens_.size(), std::memory_order_relaxed);
        }

        // Feed the drafter the fresh history once per round.
        if (spec_on) {
            std::vector<int32_t> hist(kv_tokens_.begin(), kv_tokens_.end());
            spec_.observe(hist);
        }
    }

    if (!utf8_pending.empty()) {
        const size_t utf8_tail = incomplete_utf8_tail(utf8_pending);
        const std::string flush_chunk = utf8_pending.substr(0, utf8_pending.size() - utf8_tail);
        if (!flush_chunk.empty()) {
            full_reply += flush_chunk;
            if (on_token) on_token(flush_chunk);
        }
    }

    // Session-wide speed-up telemetry: tokens emitted per decode graph.
    spec_graphs_  += n_graphs;
    spec_emitted_ += n_emitted;

    if (std::getenv("DUALMOE_VERBOSE")) {
        append_backend_log(GGML_LOG_LEVEL_INFO,
            ("[spec] графов=" + std::to_string(n_graphs) + " токенов=" + std::to_string(n_emitted) +
             " черновиков=" + std::to_string(spec_drafted_) +
             " принято=" + std::to_string(spec_accepted_) + "\n").c_str());
    }
    llama_sampler_free(smpl);
    llama_batch_free(batch);
    generating_.store(false, std::memory_order_release);
    return true;
}

void DualMoeEngine::record_parallel_seam() {
    if (!ctx_) return;
    llama_parallel_profile prof;
    if (!llama_moe_cache_parallel_profile(ctx_, &prof)) return;
    if (prof.n_dual > 0) {
        par_dual_total_.fetch_add((uint64_t) prof.n_dual, std::memory_order_relaxed);
    }
    par_serial_total_.fetch_add((uint64_t) prof.n_serial, std::memory_order_relaxed);
    last_par_gpu_ms_ = prof.gpu_ms;
    last_par_cpu_ms_ = prof.cpu_ms;
    last_par_overlap_ms_ = prof.overlap_ms;
    if (std::getenv("DUALMOE_VERBOSE")) {
        append_backend_log(GGML_LOG_LEVEL_INFO,
            ("[seam] граф: GPU " + std::to_string(prof.n_gpu) + " сплитов " +
             std::to_string((int) prof.gpu_ms) + " мс, CPU " + std::to_string(prof.n_cpu) +
             " сплитов " + std::to_string((int) prof.cpu_ms) + " мс, параллельно " +
             std::to_string(prof.n_dual) + " слоёв, последовательно " +
             std::to_string(prof.n_serial) + " слоёв\n").c_str());
    }
}

TelemetrySnapshot DualMoeEngine::telemetry() const {
    TelemetrySnapshot t;
    t.model_loaded      = loaded_.load(std::memory_order_acquire);
    t.loading           = loading_.load(std::memory_order_acquire);
    t.generating        = generating_.load(std::memory_order_acquire);
    t.load_progress_pct = load_progress_pct_.load(std::memory_order_relaxed);
    t.load_phase_index  = load_phase_index_.load(std::memory_order_relaxed);
    t.load_phase_total  = 8;

    SystemMemoryInfo sm;
    uint64_t prewarm_vram_b = 0;
    bool     is_moe_model   = false;
    {
        std::lock_guard<std::mutex> slk(state_mu_);
        t.model_name              = layout_.summary.model_name;
        t.model_arch              = layout_.summary.arch;
        is_moe_model              = layout_.summary.is_moe;
        t.status_text             = status_text_;
        t.kv_max_tokens           = cfg_.n_ctx;
        t.vram_cache_budget_mb    = active_vram_cache_mb_;
        t.vram_prewarmed_mb       = vram_prewarmed_bytes_ >> 20;
        t.ram_preloaded_mb        = ram_preloaded_bytes_ >> 20;
        prewarm_vram_b            = vram_prewarmed_bytes_;
        t.auto_split_active       = auto_split_active_;
        t.parallel_gpu_cpu_active = parallel_gpu_cpu_;
        t.zero_hdd_active         = zero_hdd_active_;
        t.disjoint_swap_active    = disjoint_swap_active_;
        t.auto_balance_active     = auto_balance_active_;
        t.active_gpu_layers       = active_gpu_layers_;
        t.active_ubatch           = active_ubatch_;
        t.thinking_enabled        = cfg_.enable_thinking;
        sm                        = diag_.sys_mem;
    }
    t.active_cpu_share  = active_cpu_share_.load(std::memory_order_relaxed);
    t.active_seam_share = active_seam_share_.load(std::memory_order_relaxed);

    if (!t.generating) {
        sm = query_system_memory();
    }
    t.os_ram_avail_mb       = sm.ram_avail_mb;
    t.os_commit_avail_mb    = sm.commit_avail_mb;
    t.gpu_vram_free_mb      = sm.vram_free_mb;
    t.gpu_vram_total_mb     = sm.vram_total_mb;
    t.arena_commit_failures = arena_.commit_failures();

    t.prefill_tps          = prefill_tps_.load(std::memory_order_relaxed);
    t.decode_tps           = decode_tps_.load(std::memory_order_relaxed);
    // GPU || CPU seam: read the split timeline of the last decode graph. n_dual > 0 means the same
    // token really had experts computed on the GPU and on the CPU at the same time.
    if (ctx_) {
        llama_parallel_profile prof;
        if (llama_moe_cache_parallel_profile(ctx_, &prof)) {
            t.par_layers_dual   = par_dual_total_.load(std::memory_order_relaxed);
            t.par_layers_serial = par_serial_total_.load(std::memory_order_relaxed);
            t.par_gpu_ms        = last_par_gpu_ms_;
            t.par_cpu_ms        = last_par_cpu_ms_;
            t.par_overlap_pct   = (prof.cpu_ms > 1e-9) ? 100.0 * prof.overlap_ms / prof.cpu_ms : 0.0;
            t.par_gpu_nodes     = (uint64_t) prof.n_gpu;
            t.par_cpu_nodes     = (uint64_t) prof.n_cpu;
        }
    }
    t.spec_tokens          = spec_.n_max();
    t.spec_drafted         = spec_drafted_;
    t.spec_accepted        = spec_accepted_;
    // Tokens per graph over the whole session: the number the user actually cares about. Before the
    // first graph it falls back to the per-round efficiency of the drafter.
    t.spec_efficiency      = (spec_graphs_ > 0)
        ? (double) spec_emitted_ / (double) spec_graphs_ : spec_.stats().efficiency;
    t.ttft_ms              = ttft_ms_.load(std::memory_order_relaxed);
    t.prompt_tokens        = prompt_tokens_.load(std::memory_order_relaxed);
    t.generated_tokens     = generated_tokens_.load(std::memory_order_relaxed);
    t.kv_used_tokens       = kv_used_.load(std::memory_order_relaxed);

    t.gpu_expert_hits       = gpu_expert_hits_.load(std::memory_order_relaxed);
    t.gpu_expert_fills      = gpu_expert_fills_.load(std::memory_order_relaxed);
    t.cpu_parallel_experts  = cpu_parallel_experts_.load(std::memory_order_relaxed);
    t.vram_upload_bytes     = vram_upload_bytes_.load(std::memory_order_relaxed);

    t.ram_cache_hits        = arena_.ram_hits();
    t.ram_to_vram_swaps     = arena_.ram_to_vram_swaps();
    t.vram_to_ram_swaps     = arena_.vram_to_ram_swaps();
    t.ssd_expert_reads      = arena_.ssd_reads();
    t.ssd_bytes_read        = arena_.ssd_bytes_read();
    t.ssd_bandwidth_mbs     = arena_.ssd_bandwidth_mbs();
    t.preload_bandwidth_mbs = arena_.preload_bandwidth_mbs();

    const uint64_t comm_b   = arena_.committed_bytes();
    t.ram_arena_used_mb     = comm_b > 0 ? ((comm_b + (1ull << 20) - 1) >> 20) : 0;
    t.ram_arena_budget_mb   = arena_.budget_bytes() >> 20;
    const uint64_t anon_b   = arena_.dense_anon_bytes();
    t.dense_anon_mb         = anon_b > 0 ? ((anon_b + (1ull << 20) - 1) >> 20) : 0;
    t.mmap_released         = arena_.mmap_released();

    // Auto budget: what the planner decided, then what is REALLY resident (measured on the arena).
    {
        std::lock_guard<std::mutex> slk(state_mu_);
        t.vram_reserve_mb   = vram_reserve_mb_;
        t.ram_reserve_mb    = ram_reserve_mb_;
        t.auto_budget_active = auto_budget_active_;
        t.expert_total_mb   = layout_.summary.expert_bytes >> 20;
    }
    if (is_moe_model && arena_.total_expert_bytes() > 0) {
        const uint64_t total_b = arena_.total_expert_bytes();
        const uint64_t vram_b  = std::min<uint64_t>(prewarm_vram_b, total_b);
        uint64_t ram_b = arena_.committed_bytes();
        if (t.disjoint_swap_active) ram_b = std::min<uint64_t>(ram_b, total_b > vram_b ? total_b - vram_b : 0);
        const uint64_t disk_b = total_b > vram_b + ram_b ? total_b - vram_b - ram_b : 0;
        t.expert_in_vram_mb = vram_b >> 20;
        t.expert_in_ram_mb  = ram_b >> 20;
        t.expert_on_disk_mb = disk_b >> 20;
        t.expert_slots      = arena_.total_expert_slots();
        const uint64_t per_exp = arena_.min_bytes_per_expert();
        if (per_exp > 0) {
            t.expert_slots_vram = t.expert_in_vram_mb / std::max<uint64_t>(1, (per_exp + (1ull << 20) - 1) >> 20);
        }
        t.expert_slots_ram = arena_.resident_expert_slots();
        const uint64_t sil_b = arena_.silicon_resident_bytes(prewarm_vram_b);
        t.silicon_residency_pct = std::min(100.0, 100.0 * (double) sil_b / (double) total_b);
        t.zero_hdd_active = disk_b <= (4ull << 20);
    } else if (t.model_loaded) {
        t.silicon_residency_pct = t.zero_hdd_active ? 100.0 : 50.0;
    }
    return t;
}

} // namespace dualmoe
