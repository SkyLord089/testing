#include "engine.h"

#include "ggml-backend.h"
#include "ggml-cpu.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

void ggml_abort_forensics_cb(const char * error_message) {
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
        {5, "virtual_arena", "5. Привязка VirtualExpertArena (DDR4 + SSD пул)",
         "Резервирование виртуальных диапазонов MEM_RESERVE (0 МБ физической RAM на старте) и открытие SSD-потоков",
         "Исключает мгновенное заполнение 16 ГБ RAM весами всех экспертов при старте.", "pending", 0.0},
        {6, "mmap_guard", "6. Безопасная политика файловых проекций (Vulkan Guard)",
         "Сохранение целостности внутренних проекций драйвера AMD Adrenalin Vulkan",
         "Защищает от сбоя 0xC0000005 из-за выгрузки шейдерного кэша и BAR-областей драйвера.", "pending", 0.0},
        {7, "stage2_context", "7. Этап 2: Рабочий контекст + GPU-банк ClassLRU",
         "Выделение KV-кэша, буферов расчёта (uBatch<=32) и VRAM-кэша горячих экспертов с авто-адаптацией",
         "Защищает от переполнения 8 ГБ VRAM и гарантирует, что все микробатчи идут через кэш слотов.", "pending", 0.0},
        {8, "parallel_hooks", "8. Активация параллельного конвейера GPU ∥ CPU ∥ SSD",
         "Подключение асинхронных хуков подкачки экспертов и проверка готовности токенизатора",
         "Финальная точка запуска генерации.", "pending", 0.0},
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

void DualMoeEngine::request_cancel() {
    cancel_flag_.store(true, std::memory_order_release);
}

void DualMoeEngine::unload_model_locked() {
    install_cpu_expert_hook(nullptr, nullptr);
    if (ctx_) {
        llama_set_moe_cache_host_cb(ctx_, nullptr, nullptr);
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

    loaded_.store(false, std::memory_order_release);
    generating_.store(false, std::memory_order_release);
    kv_used_.store(0, std::memory_order_relaxed);
    gpu_expert_hits_.store(0, std::memory_order_relaxed);
    gpu_expert_fills_.store(0, std::memory_order_relaxed);
    cpu_parallel_experts_.store(0, std::memory_order_relaxed);
    vram_upload_bytes_.store(0, std::memory_order_relaxed);

    {
        std::lock_guard<std::mutex> slk(state_mu_);
        layout_ = GgufLayout{};
        active_vram_cache_mb_ = 0;
        auto_split_active_    = false;
        parallel_gpu_cpu_     = false;
        active_gpu_layers_    = 0;
        active_cpu_share_     = 0.0f;
        active_ubatch_        = 32;
        status_text_          = "Готов к загрузке модели";
    }
}

void DualMoeEngine::unload_model() {
    if (async_load_thread_.joinable() && async_load_thread_.get_id() != std::this_thread::get_id()) {
        async_load_thread_.join();
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
            std::fprintf(fp, "=== DualMoE v0.2.0 Flight Recorder ===\n");
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

    // Fix #4: Clamp DDR4 arena budget against available Windows Commit Limit so VirtualAlloc never starves!
    if (stream_moe && pre_mem.commit_avail_mb > 0) {
        const uint64_t safety_commit_reserve_mb = 2048; // Keep >= 2 GiB commit headroom for Vulkan driver, KV host, threads
        if (pre_mem.commit_avail_mb <= safety_commit_reserve_mb + 384) {
            add_warning("Внимание: в системе осталось мало виртуальной памяти Commit (" +
                        std::to_string(pre_mem.commit_avail_mb) +
                        " МБ). Рекомендуется включить автоматический файл подкачки Windows (>= 16 ГБ). Бюджет RAM-арены снижен до 384 МБ.");
            cfg.ram_cache_mb = 384;
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

    int32_t effective_ngl = has_gpu_device ? std::max<int32_t>(0, cfg.n_gpu_layers) : 0;
    if (!has_gpu_device && cfg.n_gpu_layers > 0) {
        add_warning("Дискретный GPU Vulkan не обнаружен — активирован безопасный режим CPU + SSD (NGL = 0).");
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
        mparams.load_mode       = LLAMA_LOAD_MODE_MMAP;
        mparams.use_extra_bufts = false;
        mparams.lazy_mode       = stream_moe ? LLAMA_LAZY_MODE_OFF : LLAMA_LAZY_MODE_AUTO;
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
        cp.n_threads       = std::max<int32_t>(1, cfg.n_threads);
        cp.n_threads_batch = std::max<int32_t>(1, cfg.n_threads_batch > 0 ? cfg.n_threads_batch : cfg.n_threads);
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

    // -------------------------------------------------------------------------
    // STEP 4, 5, 6: Stage-1 Graph Capture, VirtualExpertArena, Safe MMap Guard
    // -------------------------------------------------------------------------
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
        const int rc = llama_decode(cap_ctx, warm);
        capture_mode_ = false;
        llama_free(cap_ctx);

        if (rc != 0) {
            llama_model_free(model_);
            model_ = nullptr;
            return fail_at(4, "Пробный прогон захвата графа вернул код " + std::to_string(rc));
        }

        size_t total_captured_exps = 0;
        for (const auto & v : captured_layer_experts_) total_captured_exps += v.size();
        finish_step(4, "done", "Обнаружено " + std::to_string(total_captured_exps) +
                               " 3D-проекций экспертов и " + std::to_string(captured_weights_.size()) + " весовых листьев");

        // STEP 5: VirtualExpertArena
        begin_step(5, "Инициализация виртуальной арены DDR4 (бюджет " + std::to_string(cfg.ram_cache_mb) +
                      " МБ, потоков SSD: " + std::to_string(cfg.io_lanes) + ")...");
        const size_t ram_budget = cfg.ram_cache_mb > 0 ? (cfg.ram_cache_mb << 20) : (2048ull << 20);
        std::string arena_err;
        if (!arena_.init(lay.summary.shard_paths, n_layer, lay.summary.n_expert, ram_budget, cfg.io_lanes, arena_err)) {
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
        // Conservative estimate of KV + compute buffer VRAM so RX 580 never hits vk::OutOfDeviceMemoryError
        const uint64_t kv_elem_b = bytes_per_kv_elem_num(parse_kv_type(cfg.kv_type_k)) +
                                   bytes_per_kv_elem_num(parse_kv_type(cfg.kv_type_v));
        // GQA models typically use 1/4 to 1/8 of n_embd for K/V; bound conservatively at n_embd / 3
        const uint64_t kv_dim_est = std::max<uint64_t>(256, (uint64_t) std::max<int32_t>(512, lay.summary.n_embd) / 3);
        const uint64_t gpu_layers_est = (uint64_t) std::min<int32_t>(effective_ngl, std::max<int32_t>(1, n_layer));
        const uint64_t est_kv_mb = (gpu_layers_est * (uint64_t) target_ctx * kv_dim_est * kv_elem_b) >> 20;
        const uint64_t compute_headroom_mb = 420; // Compute graph + Vulkan staging safety margin

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
        cparams.moe_cache_cpu_share = std::clamp(cfg.cpu_share, 0.0f, 1.0f);
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
    // STEP 8: Arm Parallel GPU || CPU || SSD Hooks & Ready!
    // -------------------------------------------------------------------------
    begin_step(8, "Подключение асинхронного маршрутизатора GPU ∥ CPU ∥ SSD...");

    if (cparams.moe_cache_size > 0 && effective_ngl > 0) {
        llama_set_moe_cache_host_cb(ctx_, &DualMoeEngine::c_moe_cache_callback, this);
    }

    {
        std::lock_guard<std::mutex> slk(state_mu_);
        cfg_                  = cfg;
        layout_               = std::move(lay);
        active_vram_cache_mb_ = (uint64_t) (cparams.moe_cache_size >> 20);
        auto_split_active_    = split_experts;
        parallel_gpu_cpu_     = (cparams.moe_cache_size > 0 && cparams.moe_cache_cpu_share > 0.0f);
        active_gpu_layers_    = effective_ngl;
        active_cpu_share_     = cparams.moe_cache_size > 0 ? cparams.moe_cache_cpu_share : 1.0f;
        active_ubatch_        = cparams.n_ubatch;
        status_text_          = parallel_gpu_cpu_
            ? "Готов • Параллельный режим GPU (VRAM) ∥ CPU (DDR4) ∥ SSD"
            : (stream_moe ? "Готов • Стриминг экспертов DDR4 ∥ SSD" : "Готов • Модель загружена");
        diag_.loading         = false;
        diag_.current_stage   = status_text_;
    }

    finish_step(8, "done", "Модель полностью готова к работе и принимает запросы");
    mark_flight_header("READY", "Модель успешно загружена");
    loaded_.store(true, std::memory_order_release);
    loading_.store(false, std::memory_order_release);
    return true;
}

bool DualMoeEngine::c_eval_callback(struct ggml_tensor * t, bool ask, void * user_data) {
    if (!user_data) return true;
    return static_cast<DualMoeEngine *>(user_data)->on_eval_node(t, ask);
}

void DualMoeEngine::c_moe_cache_callback(const struct llama_moe_cache_event * ev, void * user_data) {
    if (!user_data || !ev) return;
    static_cast<DualMoeEngine *>(user_data)->on_moe_cache_event(ev);
}

bool DualMoeEngine::c_abort_callback(void * user_data) {
    if (!user_data) return false;
    return static_cast<DualMoeEngine *>(user_data)->cancel_flag_.load(std::memory_order_acquire);
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
                if (leaf->op == GGML_OP_NONE) {
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

    cpu_parallel_experts_.fetch_add(selected.size(), std::memory_order_relaxed);

    if (cfg_.moe_stream && arena_.has_layer(il) && !selected.empty()) {
        arena_.prefetch_experts_async(il, selected.data(), selected.size());
        if ((size_t) il < prev_route_.size()) {
            prev_route_[(size_t) il] = selected;
        }
        if (cfg_.prefetch_ahead > 0 && n_tokens == 1) {
            const int32_t next_il = il + 1;
            if ((size_t) next_il < prev_route_.size() && arena_.has_layer(next_il) &&
                !prev_route_[(size_t) next_il].empty()) {
                arena_.prefetch_experts_async(next_il, prev_route_[(size_t) next_il].data(),
                                              prev_route_[(size_t) next_il].size());
            }
        }
    }
    return true;
}

std::string DualMoeEngine::format_chat_prompt(const std::vector<ChatMessage> & messages) const {
    if (messages.empty()) return "";
    std::vector<llama_chat_message> raw;
    raw.reserve(messages.size());
    size_t total_chars = 0;
    for (const auto & m : messages) {
        raw.push_back({m.role.c_str(), m.content.c_str()});
        total_chars += m.role.size() + m.content.size() + 32;
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
    for (const auto & m : messages) {
        oss << "<|im_start|>" << m.role << "\n" << m.content << "<|im_end|>\n";
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

    const std::string prompt = format_chat_prompt(messages);
    int32_t n_tok = -llama_tokenize(vocab_, prompt.c_str(), (int32_t) prompt.size(), nullptr, 0, true, true);
    if (n_tok <= 0) {
        err = "Не удалось токенизировать запрос";
        return false;
    }
    std::vector<llama_token> tokens((size_t) n_tok);
    const int32_t got = llama_tokenize(vocab_, prompt.c_str(), (int32_t) prompt.size(),
                                       tokens.data(), (int32_t) tokens.size(), true, true);
    if (got < 0) {
        err = "Ошибка буфера токенизатора";
        return false;
    }
    tokens.resize((size_t) got);

    const SamplingParams sp = use_override ? sampling_override : cfg_.sampling;
    return run_tokens(tokens, sp, on_token, full_reply, prompt_tokens_out, completion_tokens_out, err);
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

    int32_t n_tok = -llama_tokenize(vocab_, prompt.c_str(), (int32_t) prompt.size(), nullptr, 0, true, false);
    if (n_tok <= 0) n_tok = 1;
    std::vector<llama_token> tokens((size_t) n_tok);
    const int32_t got = llama_tokenize(vocab_, prompt.c_str(), (int32_t) prompt.size(),
                                       tokens.data(), (int32_t) tokens.size(), true, false);
    if (got > 0) {
        tokens.resize((size_t) got);
    } else {
        llama_token bos = llama_vocab_bos(vocab_);
        tokens = {bos >= 0 ? bos : 0};
    }

    SamplingParams sp = cfg_.sampling;
    sp.max_tokens  = max_tokens;
    sp.temperature = temperature;
    sp.seed        = seed;

    int32_t p_out = 0, c_out = 0;
    return run_tokens(tokens, sp, on_token, full_reply, p_out, c_out, err);
}

bool DualMoeEngine::run_tokens(const std::vector<llama_token> & tokens,
                               const SamplingParams & sp,
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
    if (sp.repeat_penalty > 1.001f && sp.repeat_last_n > 0) {
        llama_sampler_chain_add(smpl, llama_sampler_init_penalties(
            llama_vocab_n_tokens(vocab_), sp.repeat_last_n, sp.repeat_penalty, 0.0f, 0.0f));
    }
    if (sp.temperature <= 0.0f) {
        llama_sampler_chain_add(smpl, llama_sampler_init_greedy());
    } else {
        if (sp.top_k > 0) llama_sampler_chain_add(smpl, llama_sampler_init_top_k(sp.top_k));
        if (sp.top_p < 1.0f) llama_sampler_chain_add(smpl, llama_sampler_init_top_p(sp.top_p, 1));
        if (sp.min_p > 0.0f) llama_sampler_chain_add(smpl, llama_sampler_init_min_p(sp.min_p, 1));
        llama_sampler_chain_add(smpl, llama_sampler_init_temp(sp.temperature));
        llama_sampler_chain_add(smpl, llama_sampler_init_dist(sp.seed));
    }

    const size_t pen_tail = sp.repeat_last_n > 0 ? std::min<size_t>(tokens.size(), (size_t) sp.repeat_last_n) : 0;
    for (size_t i = tokens.size() - pen_tail; i < tokens.size(); ++i) {
        llama_sampler_accept(smpl, tokens[i]);
    }

    std::string utf8_pending;
    const auto t_decode_start = std::chrono::steady_clock::now();

    for (int32_t step = 0; step < max_new; ++step) {
        if (cancel_flag_.load(std::memory_order_acquire)) break;
        if (kv_tokens_.size() + 2 >= n_ctx) break;

        const llama_token next_tok = llama_sampler_sample(smpl, ctx_, -1);
        llama_sampler_accept(smpl, next_tok);

        if (llama_vocab_is_eog(vocab_, next_tok)) {
            break;
        }

        char piece_buf[256];
        int32_t piece_len = llama_token_to_piece(vocab_, next_tok, piece_buf, sizeof(piece_buf), 0, false);
        std::string piece;
        if (piece_len > (int32_t) sizeof(piece_buf)) {
            std::vector<char> big((size_t) piece_len + 8);
            piece_len = llama_token_to_piece(vocab_, next_tok, big.data(), (int32_t) big.size(), 0, false);
            if (piece_len > 0) piece.assign(big.data(), (size_t) piece_len);
        } else if (piece_len > 0) {
            piece.assign(piece_buf, (size_t) piece_len);
        }

        utf8_pending += piece;
        const size_t tail = incomplete_utf8_tail(utf8_pending);
        if (utf8_pending.size() > tail) {
            const std::string valid_chunk = utf8_pending.substr(0, utf8_pending.size() - tail);
            utf8_pending.erase(0, valid_chunk.size());
            full_reply += valid_chunk;
            if (on_token && !on_token(valid_chunk)) {
                cancel_flag_.store(true, std::memory_order_release);
                break;
            }
        }

        ++completion_tokens_out;
        generated_tokens_.fetch_add(1, std::memory_order_relaxed);

        const auto now = std::chrono::steady_clock::now();
        const double dec_ms = std::chrono::duration<double, std::milli>(now - t_decode_start).count();
        if (dec_ms > 1.0 && completion_tokens_out > 0) {
            decode_tps_.store((double) completion_tokens_out * 1000.0 / dec_ms, std::memory_order_relaxed);
        }

        if (step + 1 >= max_new) break;

        batch.n_tokens     = 1;
        batch.token[0]     = next_tok;
        batch.pos[0]       = (llama_pos) kv_tokens_.size();
        batch.n_seq_id[0]  = 1;
        batch.seq_id[0][0] = 0;
        batch.logits[0]    = 1;

        const int rc = llama_decode(ctx_, batch);
        if (rc != 0) break;
        kv_tokens_.push_back(next_tok);
        kv_used_.store((uint32_t) kv_tokens_.size(), std::memory_order_relaxed);
    }

    if (!utf8_pending.empty()) {
        full_reply += utf8_pending;
        if (on_token) on_token(utf8_pending);
    }

    llama_sampler_free(smpl);
    llama_batch_free(batch);
    generating_.store(false, std::memory_order_release);
    return true;
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
    {
        std::lock_guard<std::mutex> slk(state_mu_);
        t.model_name              = layout_.summary.model_name;
        t.model_arch              = layout_.summary.arch;
        t.status_text             = status_text_;
        t.kv_max_tokens           = cfg_.n_ctx;
        t.vram_cache_budget_mb    = active_vram_cache_mb_;
        t.auto_split_active       = auto_split_active_;
        t.parallel_gpu_cpu_active = parallel_gpu_cpu_;
        t.active_gpu_layers       = active_gpu_layers_;
        t.active_cpu_share        = active_cpu_share_;
        t.active_ubatch           = active_ubatch_;
        sm                        = diag_.sys_mem;
    }
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
    t.ttft_ms              = ttft_ms_.load(std::memory_order_relaxed);
    t.prompt_tokens        = prompt_tokens_.load(std::memory_order_relaxed);
    t.generated_tokens     = generated_tokens_.load(std::memory_order_relaxed);
    t.kv_used_tokens       = kv_used_.load(std::memory_order_relaxed);

    t.gpu_expert_hits      = gpu_expert_hits_.load(std::memory_order_relaxed);
    t.gpu_expert_fills     = gpu_expert_fills_.load(std::memory_order_relaxed);
    t.cpu_parallel_experts = cpu_parallel_experts_.load(std::memory_order_relaxed);
    t.vram_upload_bytes    = vram_upload_bytes_.load(std::memory_order_relaxed);

    t.ram_cache_hits       = arena_.ram_hits();
    t.ssd_expert_reads     = arena_.ssd_reads();
    t.ssd_bytes_read       = arena_.ssd_bytes_read();
    t.ssd_bandwidth_mbs    = arena_.ssd_bandwidth_mbs();
    const uint64_t comm_b  = arena_.committed_bytes();
    t.ram_arena_used_mb    = comm_b > 0 ? ((comm_b + (1ull << 20) - 1) >> 20) : 0;
    t.ram_arena_budget_mb  = arena_.budget_bytes() >> 20;
    const uint64_t anon_b  = arena_.dense_anon_bytes();
    t.dense_anon_mb        = anon_b > 0 ? ((anon_b + (1ull << 20) - 1) >> 20) : 0;
    t.mmap_released        = arena_.mmap_released();
    return t;
}

} // namespace dualmoe
