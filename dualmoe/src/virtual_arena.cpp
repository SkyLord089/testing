#include "virtual_arena.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cwctype>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace dualmoe {

namespace {

constexpr uint8_t kStateEmpty   = 0;
constexpr uint8_t kStateLoading = 1;
constexpr uint8_t kStateReady   = 2;
constexpr float   kCrfHalfLife  = 32.0f;

size_t detect_page_size() {
#if defined(_WIN32)
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    return si.dwPageSize > 0 ? (size_t) si.dwPageSize : 4096u;
#else
    const long ps = sysconf(_SC_PAGESIZE);
    return ps > 0 ? (size_t) ps : 4096u;
#endif
}

uint8_t * reserve_virtual_range(size_t bytes) {
#if defined(_WIN32)
    return static_cast<uint8_t *>(VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS));
#else
    void * p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<uint8_t *>(p);
#endif
}

void release_virtual_range(uint8_t * ptr, size_t bytes) {
    if (!ptr || bytes == 0) return;
#if defined(_WIN32)
    (void) bytes;
    VirtualFree(ptr, 0, MEM_RELEASE);
#else
    munmap(ptr, bytes);
#endif
}

bool commit_pages_os(uint8_t * ptr, size_t bytes) {
    if (!ptr || bytes == 0) return true;
#if defined(_WIN32)
    return VirtualAlloc(ptr, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr;
#else
    (void) ptr;
    (void) bytes; // Demand-paged on first write under Linux MAP_ANONYMOUS
    return true;
#endif
}

void decommit_pages_os(uint8_t * ptr, size_t bytes) {
    if (!ptr || bytes == 0) return;
#if defined(_WIN32)
    VirtualFree(ptr, bytes, MEM_DECOMMIT);
#else
    madvise(ptr, bytes, MADV_DONTNEED);
#endif
}

#if defined(_WIN32)
std::wstring lower_wstring(std::wstring s) {
    for (wchar_t & ch : s) {
        ch = (wchar_t) std::towlower(ch);
    }
    return s;
}

// Resolve K32GetMappedFileNameW from kernel32.dll so we strictly verify a MEM_MAPPED region belongs
// to one of our .gguf shard files and NEVER unmap AMD Adrenalin Vulkan SPIR-V / PCIe BAR views!
bool is_mapped_gguf_shard_win32(void * allocation_base, const std::vector<std::wstring> & shard_tails_lower) {
    if (!allocation_base || shard_tails_lower.empty()) return false;
    using fn_t = DWORD (WINAPI *)(HANDLE, LPVOID, LPWSTR, DWORD);
    static fn_t pGetMapped = reinterpret_cast<fn_t>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "K32GetMappedFileNameW")));
    if (!pGetMapped) return false;

    wchar_t buf[2048]{};
    const DWORD len = pGetMapped(GetCurrentProcess(), allocation_base, buf, 2047);
    if (len == 0) return false;
    const std::wstring mapped = lower_wstring(std::wstring(buf, len));
    for (const std::wstring & tail : shard_tails_lower) {
        if (!tail.empty() && mapped.size() >= tail.size() &&
            mapped.compare(mapped.size() - tail.size(), tail.size(), tail) == 0) {
            return true;
        }
    }
    return false;
}
#endif

} // namespace

struct VirtualExpertArena::ShardHandle {
#if defined(_WIN32)
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~ShardHandle() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
#else
    int fd = -1;
    ino_t inode = 0;
    ~ShardHandle() {
        if (fd >= 0) {
            ::close(fd);
        }
    }
#endif
};

VirtualExpertArena::VirtualExpertArena() : page_size_(detect_page_size()) {}

VirtualExpertArena::~VirtualExpertArena() {
    shutdown();
}

bool VirtualExpertArena::open_shards(std::string & err) {
    close_shards();
    shards_.reserve(shard_paths_.size());
    for (const std::string & p : shard_paths_) {
        auto sh = std::make_unique<ShardHandle>();
#if defined(_WIN32)
        const std::filesystem::path fsp = std::filesystem::u8path(p);
        sh->handle = CreateFileW(fsp.wstring().c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (sh->handle == INVALID_HANDLE_VALUE) {
            err = "Не удалось открыть файл модели для чтения: " + p;
            return false;
        }
#else
        sh->fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
        if (sh->fd < 0) {
            err = "Не удалось открыть файл модели для чтения: " + p;
            return false;
        }
        struct stat st{};
        if (fstat(sh->fd, &st) == 0) {
            sh->inode = st.st_ino;
        }
#if defined(POSIX_FADV_RANDOM)
        posix_fadvise(sh->fd, 0, 0, POSIX_FADV_RANDOM);
#endif
#endif
        shards_.push_back(std::move(sh));
    }
    return true;
}

void VirtualExpertArena::close_shards() {
    shards_.clear();
}

bool VirtualExpertArena::pread_exact(int32_t shard_idx, void * dst, size_t len, uint64_t offset) {
    if (!dst || shard_idx < 0 || (size_t) shard_idx >= shards_.size()) return false;
    uint8_t * out = static_cast<uint8_t *>(dst);
    size_t done = 0;
#if defined(_WIN32)
    HANDLE h = shards_[(size_t) shard_idx]->handle;
    while (done < len) {
        const DWORD chunk = (DWORD) std::min<size_t>(len - done, 64ull << 20);
        OVERLAPPED ov{};
        const uint64_t cur_off = offset + done;
        ov.Offset     = (DWORD) (cur_off & 0xFFFFFFFFull);
        ov.OffsetHigh = (DWORD) (cur_off >> 32);
        DWORD got = 0;
        if (!ReadFile(h, out + done, chunk, &got, &ov) || got == 0) {
            return false;
        }
        done += (size_t) got;
    }
    return true;
#else
    const int fd = shards_[(size_t) shard_idx]->fd;
    while (done < len) {
        const ssize_t got = ::pread(fd, out + done, len - done, (off_t) (offset + done));
        if (got < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (got == 0) return false;
        done += (size_t) got;
    }
    return true;
#endif
}

bool VirtualExpertArena::init(const std::vector<std::string> & shard_paths,
                              int32_t n_layer,
                              int32_t n_expert,
                              size_t ram_budget_bytes,
                              int32_t io_lanes,
                              std::string & err) {
    shutdown();
    shard_paths_  = shard_paths;
    n_layer_      = std::max<int32_t>(1, n_layer);
    n_expert_     = std::max<int32_t>(1, n_expert);
    budget_bytes_ = ram_budget_bytes;

    if (!open_shards(err)) {
        return false;
    }

    layers_.resize((size_t) n_layer_);
    for (int32_t il = 0; il < n_layer_; ++il) {
        layers_[(size_t) il].il = il;
    }
    slot_meta_.assign((size_t) n_layer_ * (size_t) n_expert_, CacheSlotMeta{});

    stop_workers_ = false;
    const int lanes = std::clamp<int>(io_lanes, 1, 16);
    workers_.reserve((size_t) lanes);
    for (int i = 0; i < lanes; ++i) {
        workers_.emplace_back(&VirtualExpertArena::worker_loop, this);
    }
    return true;
}

void VirtualExpertArena::shutdown() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_workers_ = true;
        io_queue_.clear();
    }
    io_cv_.notify_all();
    for (std::thread & t : workers_) {
        if (t.joinable()) t.join();
    }
    workers_.clear();

    for (StreamedLayer & L : layers_) {
        for (StreamedProjection & P : L.projs) {
            release_virtual_range(P.arena_base, P.arena_cap);
            P.arena_base = nullptr;
            P.arena_cap  = 0;
        }
    }
    layers_.clear();
    tensor_to_proj_.clear();
    slot_meta_.clear();

    if (dense_arena_) {
        release_virtual_range(dense_arena_, dense_bytes_);
        dense_arena_ = nullptr;
        dense_bytes_ = 0;
    }

#if defined(_WIN32)
    for (const auto & ph : win_placeholders_) {
        if (ph.first) VirtualFree(ph.first, 0, MEM_RELEASE);
    }
#endif
    win_placeholders_.clear();
    mmap_released_ = false;

    close_shards();
    committed_bytes_.store(0, std::memory_order_relaxed);
    ram_hits_.store(0, std::memory_order_relaxed);
    ssd_reads_.store(0, std::memory_order_relaxed);
    ssd_bytes_read_.store(0, std::memory_order_relaxed);
    ssd_active_ns_.store(0, std::memory_order_relaxed);
    commit_failures_.store(0, std::memory_order_relaxed);
}

bool VirtualExpertArena::bind_layer_projection(int32_t il,
                                               ggml_tensor * tensor,
                                               int32_t shard_idx,
                                               uint64_t file_offset,
                                               uint64_t nbytes,
                                               std::string & err) {
    if (!tensor || il < 0) return false;
    if ((size_t) il >= layers_.size()) {
        layers_.resize((size_t) il + 1);
        slot_meta_.resize(layers_.size() * (size_t) n_expert_);
    }

    const uint64_t nb2 = (uint64_t) tensor->nb[2];
    if (nb2 == 0 || (int32_t) tensor->ne[2] != n_expert_) {
        err = std::string("Неверная размерность экспертного тензора: ") + tensor->name;
        return false;
    }

    // Reserve extra trailing page so SIMD/MMQ kernels never read past the mapped boundary
    const uint64_t raw_cap = nbytes + page_size_;
    const uint64_t cap     = ((raw_cap + page_size_ - 1) / page_size_) * page_size_;
    uint8_t * base         = reserve_virtual_range((size_t) cap);
    if (!base) {
        err = std::string("Не удалось зарезервировать виртуальное адресное пространство (MEM_RESERVE) для ") + tensor->name;
        return false;
    }
    // Commit trailing guard page once so padded tail reads see clean zeros
    if (!commit_pages_os(base + cap - page_size_, page_size_)) {
        release_virtual_range(base, (size_t) cap);
        err = std::string("Исчерпан лимит виртуальной памяти Windows (Commit Limit / pagefile.sys) при создании guard-страницы для ") + tensor->name;
        return false;
    }

    StreamedProjection P;
    P.tensor      = tensor;
    P.shard_idx   = shard_idx;
    P.file_offset = file_offset;
    P.nb2         = nb2;
    P.total_bytes = nbytes;
    P.arena_base  = base;
    P.arena_cap   = cap;
    P.page_refs.assign((size_t) (cap / page_size_), 0);
    P.state       = std::make_unique<std::atomic<uint8_t>[]>((size_t) n_expert_);
    for (int32_t e = 0; e < n_expert_; ++e) {
        P.state[(size_t) e].store(kStateEmpty, std::memory_order_relaxed);
    }

    // Rebind ggml_tensor->data directly onto our virtual arena!
    tensor->data = base;

    StreamedLayer & L = layers_[(size_t) il];
    L.il = il;
    const int32_t proj_idx = (int32_t) L.projs.size();
    L.bytes_per_expert += nb2;
    L.projs.push_back(std::move(P));
    tensor_to_proj_[tensor] = {il, proj_idx};
    return true;
}

bool VirtualExpertArena::rehome_dense_weights(const std::vector<DenseRehomeEntry> & entries, std::string & err) {
    if (entries.empty()) return true;
    uint64_t total = 0;
    for (const auto & e : entries) {
        total += ((e.nbytes + 63ull) & ~63ull);
    }
    total = ((total + page_size_ - 1) / page_size_) * page_size_;

    uint8_t * buf = reserve_virtual_range((size_t) total);
    if (!buf) {
        err = "Недостаточно виртуального адресного пространства для буфера плотных весов";
        return false;
    }
    if (!commit_pages_os(buf, (size_t) total)) {
        release_virtual_range(buf, (size_t) total);
        err = "Недостаточно виртуальной памяти ОС (Commit Limit / файл подкачки) для переноса плотных весов";
        return false;
    }

    uint64_t cursor = 0;
    for (const auto & e : entries) {
        uint8_t * dst = buf + cursor;
        if (!pread_exact(e.shard_idx, dst, (size_t) e.nbytes, e.file_offset)) {
            release_virtual_range(buf, (size_t) total);
            err = std::string("Ошибка чтения плотного тензора: ") + (e.primary ? e.primary->name : "?");
            return false;
        }
        if (e.primary) e.primary->data = dst;
        for (ggml_tensor * alias : e.aliases) {
            if (alias) alias->data = dst;
        }
        cursor += ((e.nbytes + 63ull) & ~63ull);
    }

    dense_arena_ = buf;
    dense_bytes_ = total;
    return true;
}

bool VirtualExpertArena::release_file_mappings(const std::vector<const void *> & check_host_ptrs) {
#if defined(_WIN32)
    // Build lowercase filename suffixes of our exact .gguf shards so we NEVER touch Vulkan driver mappings!
    std::vector<std::wstring> shard_tails;
    for (const std::string & p : shard_paths_) {
        const std::filesystem::path fsp = std::filesystem::u8path(p);
        const std::wstring fname = lower_wstring(fsp.filename().wstring());
        if (!fname.empty()) {
            shard_tails.push_back(L"\\" + fname);
        }
    }
    if (shard_tails.empty()) return false;

    MEMORY_BASIC_INFORMATION mbi{};
    uint8_t * addr = nullptr;
    struct ViewRange { void * base; size_t size; };
    std::vector<ViewRange> candidates;
    while (VirtualQuery(addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_MAPPED && mbi.AllocationBase == addr && mbi.RegionSize >= (1ull << 20)) {
            size_t total_sz = 0;
            uint8_t * scan = addr;
            MEMORY_BASIC_INFORMATION sub{};
            while (VirtualQuery(scan, &sub, sizeof(sub)) == sizeof(sub) && sub.AllocationBase == addr && sub.State != MEM_FREE) {
                total_sz += sub.RegionSize;
                scan += sub.RegionSize;
            }
            if (is_mapped_gguf_shard_win32(addr, shard_tails)) {
                bool referenced = false;
                for (const void * p : check_host_ptrs) {
                    if (p >= addr && p < addr + total_sz) {
                        referenced = true;
                        break;
                    }
                }
                if (!referenced) {
                    candidates.push_back({addr, total_sz});
                }
            }
            addr += std::max<size_t>(total_sz, page_size_);
            continue;
        }
        uint8_t * next = static_cast<uint8_t *>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= addr) break;
        addr = next;
    }

    int unmapped = 0;
    for (const auto & c : candidates) {
        if (UnmapViewOfFile(c.base)) {
            void * ph = VirtualAlloc(c.base, c.size, MEM_RESERVE, PAGE_NOACCESS);
            if (ph) win_placeholders_.emplace_back(ph, c.size);
            ++unmapped;
        }
    }
    if (unmapped > 0) {
        std::string ignore_err;
        open_shards(ignore_err);
        mmap_released_ = true;
    }
    return mmap_released_;
#elif defined(__linux__)
    std::ifstream maps("/proc/self/maps");
    if (!maps.is_open()) return false;
    struct MapRegion { uintptr_t lo; uintptr_t hi; };
    std::vector<MapRegion> regions;
    std::string line;
    while (std::getline(maps, line)) {
        std::istringstream iss(line);
        std::string range, perms, offset, dev;
        unsigned long inode = 0;
        if (!(iss >> range >> perms >> offset >> dev >> inode)) continue;
        if (inode == 0) continue;
        bool match_shard = false;
        for (const auto & sh : shards_) {
            if (sh->inode != 0 && (unsigned long) sh->inode == inode) {
                match_shard = true;
                break;
            }
        }
        if (!match_shard) continue;
        const size_t dash = range.find('-');
        if (dash == std::string::npos) continue;
        const uintptr_t lo = std::strtoull(range.substr(0, dash).c_str(), nullptr, 16);
        const uintptr_t hi = std::strtoull(range.substr(dash + 1).c_str(), nullptr, 16);
        if (hi > lo) regions.push_back({lo, hi});
    }

    int replaced = 0;
    for (const auto & r : regions) {
        bool referenced = false;
        for (const void * p : check_host_ptrs) {
            const uintptr_t pv = reinterpret_cast<uintptr_t>(p);
            if (pv >= r.lo && pv < r.hi) {
                referenced = true;
                break;
            }
        }
        if (referenced) continue;
        void * res = mmap(reinterpret_cast<void *>(r.lo), r.hi - r.lo,
                          PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (res != MAP_FAILED) {
            ++replaced;
        }
    }
    mmap_released_ = (replaced > 0);
    return mmap_released_;
#else
    (void) check_host_ptrs;
    return false;
#endif
}

bool VirtualExpertArena::evict_one_lowest_locked(uint64_t current_pin_epoch) {
    int32_t best_il  = -1;
    int32_t best_exp = -1;
    double  lowest_score = 1e300;

    for (int32_t il = 0; il < (int32_t) layers_.size(); ++il) {
        const StreamedLayer & L = layers_[(size_t) il];
        if (L.projs.empty()) continue;
        for (int32_t e = 0; e < n_expert_; ++e) {
            const CacheSlotMeta & m = slot_meta_[(size_t) il * (size_t) n_expert_ + (size_t) e];
            if (!m.resident || m.pin_epoch == current_pin_epoch) continue;

            bool loading = false;
            for (const StreamedProjection & P : L.projs) {
                if (P.state[(size_t) e].load(std::memory_order_acquire) == kStateLoading) {
                    loading = true;
                    break;
                }
            }
            if (loading) continue;

            const double age = (double) (step_clock_ - m.last_step);
            const double decayed = (double) m.crf * std::exp2(-age / (double) kCrfHalfLife);
            if (decayed < lowest_score) {
                lowest_score = decayed;
                best_il      = il;
                best_exp     = e;
            }
        }
    }
    if (best_il < 0) return false;
    evict_expert_locked(best_il, best_exp);
    return true;
}

bool VirtualExpertArena::commit_pages_with_recovery_locked(uint8_t * ptr, size_t bytes, uint64_t current_pin_epoch) {
    if (commit_pages_os(ptr, bytes)) return true;
    commit_failures_.fetch_add(1, std::memory_order_relaxed);
    // OS Commit Limit hit: aggressively evict cold slices until VirtualAlloc succeeds
    for (int tries = 0; tries < 64; ++tries) {
        if (!evict_one_lowest_locked(current_pin_epoch)) break;
        if (commit_pages_os(ptr, bytes)) return true;
    }
    return false;
}

bool VirtualExpertArena::commit_expert_locked(int32_t il, int32_t exp, uint64_t current_pin_epoch) {
    StreamedLayer & L = layers_[(size_t) il];
    uint64_t newly_committed = 0;
    for (size_t pi = 0; pi < L.projs.size(); ++pi) {
        StreamedProjection & P = L.projs[pi];
        const uint64_t byte_beg = (uint64_t) exp * P.nb2;
        const uint64_t byte_end = byte_beg + P.nb2;
        const size_t   p_beg    = (size_t) (byte_beg / page_size_);
        const size_t   p_end    = (size_t) ((byte_end + page_size_ - 1) / page_size_);
        size_t run_start = p_end;
        for (size_t p = p_beg; p < p_end; ++p) {
            if (P.page_refs[p]++ == 0) {
                if (run_start == p_end) run_start = p;
            } else if (run_start < p_end) {
                const size_t bytes = (p - run_start) * page_size_;
                if (!commit_pages_with_recovery_locked(P.arena_base + run_start * page_size_, bytes, current_pin_epoch)) {
                    // Roll back incremented page_refs up to p on this projection
                    for (size_t rb = p_beg; rb <= p; ++rb) {
                        if (P.page_refs[rb] > 0) --P.page_refs[rb];
                    }
                    return false;
                }
                newly_committed += bytes;
                run_start = p_end;
            }
        }
        if (run_start < p_end) {
            const size_t bytes = (p_end - run_start) * page_size_;
            if (!commit_pages_with_recovery_locked(P.arena_base + run_start * page_size_, bytes, current_pin_epoch)) {
                for (size_t rb = p_beg; rb < p_end; ++rb) {
                    if (P.page_refs[rb] > 0) --P.page_refs[rb];
                }
                return false;
            }
            newly_committed += bytes;
        }
    }
    committed_bytes_.fetch_add(newly_committed, std::memory_order_relaxed);
    return true;
}

void VirtualExpertArena::evict_expert_locked(int32_t il, int32_t exp) {
    StreamedLayer & L = layers_[(size_t) il];
    uint64_t freed = 0;
    for (StreamedProjection & P : L.projs) {
        P.state[(size_t) exp].store(kStateEmpty, std::memory_order_release);
        const uint64_t byte_beg = (uint64_t) exp * P.nb2;
        const uint64_t byte_end = byte_beg + P.nb2;
        const size_t   p_beg    = (size_t) (byte_beg / page_size_);
        const size_t   p_end    = (size_t) ((byte_end + page_size_ - 1) / page_size_);
        size_t run_start = p_end;
        for (size_t p = p_beg; p < p_end; ++p) {
            if (P.page_refs[p] > 0 && --P.page_refs[p] == 0) {
                if (run_start == p_end) run_start = p;
            } else if (run_start < p_end) {
                const size_t bytes = (p - run_start) * page_size_;
                decommit_pages_os(P.arena_base + run_start * page_size_, bytes);
                freed += bytes;
                run_start = p_end;
            }
        }
        if (run_start < p_end) {
            const size_t bytes = (p_end - run_start) * page_size_;
            decommit_pages_os(P.arena_base + run_start * page_size_, bytes);
            freed += bytes;
        }
    }
    const uint64_t cur = committed_bytes_.load(std::memory_order_relaxed);
    committed_bytes_.store(cur > freed ? cur - freed : 0, std::memory_order_relaxed);
    slot_meta_[(size_t) il * (size_t) n_expert_ + (size_t) exp].resident = false;
}

void VirtualExpertArena::evict_to_budget_locked(uint64_t needed_bytes, uint64_t current_pin_epoch) {
    if (budget_bytes_ == 0) return;
    while (committed_bytes_.load(std::memory_order_relaxed) + needed_bytes > budget_bytes_) {
        if (!evict_one_lowest_locked(current_pin_epoch)) break;
    }
}

void VirtualExpertArena::ensure_experts_sync(int32_t il, const int32_t * experts, size_t count) {
    if (!experts || count == 0 || !has_layer(il)) return;
    prefetch_experts_async(il, experts, count);
    StreamedLayer & L = layers_[(size_t) il];
    for (size_t i = 0; i < count; ++i) {
        const int32_t e = experts[i];
        if (e < 0 || e >= n_expert_) continue;
        for (const StreamedProjection & P : L.projs) {
            wait_tensor_expert_ready(P.tensor, e);
        }
    }
}

void VirtualExpertArena::prefetch_experts_async(int32_t il, const int32_t * experts, size_t count) {
    if (!experts || count == 0 || !has_layer(il)) return;
    bool queued_any = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        ++step_clock_;
        const uint64_t epoch = ++pin_epoch_;
        StreamedLayer & L = layers_[(size_t) il];

        // First mark all requested experts as pinned so eviction cannot drop siblings in the same batch
        for (size_t i = 0; i < count; ++i) {
            const int32_t e = experts[i];
            if (e < 0 || e >= n_expert_) continue;
            CacheSlotMeta & m = slot_meta_[(size_t) il * (size_t) n_expert_ + (size_t) e];
            const double age = (double) (step_clock_ - m.last_step);
            m.crf       = (float) ((double) m.crf * std::exp2(-age / (double) kCrfHalfLife) + 1.0);
            m.last_step = step_clock_;
            m.pin_epoch = epoch;
        }

        for (size_t i = 0; i < count; ++i) {
            const int32_t e = experts[i];
            if (e < 0 || e >= n_expert_) continue;
            CacheSlotMeta & m = slot_meta_[(size_t) il * (size_t) n_expert_ + (size_t) e];
            if (m.resident) {
                ram_hits_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            evict_to_budget_locked(L.bytes_per_expert, epoch);
            if (!commit_expert_locked(il, e, epoch)) {
                // Commit failed even after full recovery: mark state ready only if already valid; avoid worker write to unmapped page
                for (int32_t p = 0; p < (int32_t) L.projs.size(); ++p) {
                    L.projs[(size_t) p].state[(size_t) e].store(kStateReady, std::memory_order_release);
                }
                continue;
            }
            m.resident = true;
            ssd_reads_.fetch_add(1, std::memory_order_relaxed);

            for (int32_t p = 0; p < (int32_t) L.projs.size(); ++p) {
                L.projs[(size_t) p].state[(size_t) e].store(kStateLoading, std::memory_order_release);
                io_queue_.push_back(IoJob{il, p, e});
                queued_any = true;
            }
        }
    }
    if (queued_any) {
        io_cv_.notify_all();
    }
}

void VirtualExpertArena::worker_loop() {
    while (true) {
        IoJob job{};
        uint8_t * dst = nullptr;
        int32_t shard_idx = 0;
        uint64_t file_off = 0;
        uint64_t len      = 0;
        std::atomic<uint8_t> * state_cell = nullptr;

        {
            std::unique_lock<std::mutex> lk(mu_);
            io_cv_.wait(lk, [&] { return stop_workers_ || !io_queue_.empty(); });
            if (stop_workers_ && io_queue_.empty()) return;
            job = io_queue_.front();
            io_queue_.pop_front();

            StreamedProjection & P = layers_[(size_t) job.il].projs[(size_t) job.proj];
            dst        = P.arena_base + (uint64_t) job.exp * P.nb2;
            shard_idx  = P.shard_idx;
            file_off   = P.file_offset + (uint64_t) job.exp * P.nb2;
            len        = P.nb2;
            state_cell = &P.state[(size_t) job.exp];
        }

        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = pread_exact(shard_idx, dst, (size_t) len, file_off);
        const auto t1 = std::chrono::steady_clock::now();
        if (!ok) {
            std::memset(dst, 0, (size_t) len);
        }
        const uint64_t ns = (uint64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        ssd_bytes_read_.fetch_add(len, std::memory_order_relaxed);
        ssd_active_ns_.fetch_add(std::max<uint64_t>(1, ns), std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> lk(mu_);
            state_cell->store(kStateReady, std::memory_order_release);
        }
        ready_cv_.notify_all();
    }
}

void VirtualExpertArena::wait_tensor_expert_ready(const ggml_tensor * src0, int32_t expert_id) {
    if (!src0 || expert_id < 0 || expert_id >= n_expert_) return;
    const auto it = tensor_to_proj_.find(src0);
    if (it == tensor_to_proj_.end()) return;

    const int32_t il   = it->second.first;
    const int32_t proj = it->second.second;
    std::atomic<uint8_t> & cell = layers_[(size_t) il].projs[(size_t) proj].state[(size_t) expert_id];

    // Lock-free fast path (~1 ns when already resident in DDR4)
    if (cell.load(std::memory_order_acquire) == kStateReady) {
        return;
    }

    // Fallback safety: if an unprefetched expert was ever touched directly (e.g. wide batch), trigger load now
    if (cell.load(std::memory_order_acquire) == kStateEmpty) {
        prefetch_experts_async(il, &expert_id, 1);
    }

    std::unique_lock<std::mutex> lk(mu_);
    ready_cv_.wait(lk, [&] {
        return stop_workers_ || cell.load(std::memory_order_acquire) == kStateReady;
    });
}

void VirtualExpertArena::c_expert_ready_hook(const struct ggml_tensor * src0, int32_t expert_id, void * user_data) {
    if (!user_data) return;
    static_cast<VirtualExpertArena *>(user_data)->wait_tensor_expert_ready(src0, expert_id);
}

double VirtualExpertArena::ssd_bandwidth_mbs() const {
    const uint64_t bytes = ssd_bytes_read_.load(std::memory_order_relaxed);
    const uint64_t ns    = ssd_active_ns_.load(std::memory_order_relaxed);
    if (bytes == 0 || ns == 0) return 0.0;
    const double sec = (double) ns / 1e9 / std::max<size_t>(1, workers_.size());
    return sec > 1e-6 ? ((double) bytes / (1024.0 * 1024.0)) / sec : 0.0;
}

} // namespace dualmoe
