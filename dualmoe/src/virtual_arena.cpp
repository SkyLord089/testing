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
constexpr uint8_t kStateInVram  = 3;
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
    (void) bytes;
    return true;
#endif
}

void lock_pages_best_effort(uint8_t * ptr, size_t bytes) {
    if (!ptr || bytes == 0) return;
#if defined(_WIN32)
    // Best-effort pin in physical DDR4 working set so Windows 11 avoids paging warm experts to HDD pagefile.sys
    (void) VirtualLock(ptr, bytes);
#else
    (void) ptr;
    (void) bytes;
#endif
}

void decommit_pages_os(uint8_t * ptr, size_t bytes) {
    if (!ptr || bytes == 0) return;
#if defined(_WIN32)
    (void) VirtualUnlock(ptr, bytes);
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
    uint64_t file_size = 0;
    ~ShardHandle() {
        if (handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
#else
    int fd = -1;
    ino_t inode = 0;
    uint64_t file_size = 0;
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
        const DWORD flags = sequential_hdd_mode_ ? FILE_FLAG_SEQUENTIAL_SCAN : FILE_FLAG_RANDOM_ACCESS;
        sh->handle = CreateFileW(fsp.wstring().c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, flags, nullptr);
        if (sh->handle == INVALID_HANDLE_VALUE) {
            err = "Не удалось открыть файл модели для чтения: " + p;
            return false;
        }
        LARGE_INTEGER sz{};
        if (GetFileSizeEx(sh->handle, &sz) && sz.QuadPart > 0) {
            sh->file_size = (uint64_t) sz.QuadPart;
        }
#else
        sh->fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
        if (sh->fd < 0) {
            err = "Не удалось открыть файл модели для чтения: " + p;
            return false;
        }
        struct stat st{};
        if (fstat(sh->fd, &st) == 0) {
            sh->inode     = st.st_ino;
            sh->file_size = (uint64_t) st.st_size;
        }
#if defined(POSIX_FADV_SEQUENTIAL) && defined(POSIX_FADV_RANDOM)
        posix_fadvise(sh->fd, 0, 0, sequential_hdd_mode_ ? POSIX_FADV_SEQUENTIAL : POSIX_FADV_RANDOM);
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
        const DWORD chunk = (DWORD) std::min<size_t>(len - done, 16ull << 20);
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
        const ssize_t got = ::pread(fd, out + done, std::min<size_t>(len - done, 16ull << 20), (off_t) (offset + done));
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
                              bool sequential_hdd_mode,
                              std::string & err) {
    shutdown();
    shard_paths_         = shard_paths;
    n_layer_             = std::max<int32_t>(1, n_layer);
    n_expert_            = std::max<int32_t>(1, n_expert);
    budget_bytes_        = ram_budget_bytes;
    sequential_hdd_mode_ = sequential_hdd_mode;

    if (!open_shards(err)) {
        return false;
    }

    layers_.resize((size_t) n_layer_);
    for (int32_t il = 0; il < n_layer_; ++il) {
        layers_[(size_t) il].il = il;
    }
    slot_meta_.assign((size_t) n_layer_ * (size_t) n_expert_, CacheSlotMeta{});

    stop_workers_ = false;
    const int lanes = sequential_hdd_mode_ ? 1 : std::clamp<int>(io_lanes, 1, 16);
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
    pending_promoted_.clear();
    prewarm_ra_         = PrewarmReadAhead{};
    prewarm_disk_bytes_ = 0;
    prewarm_disk_ns_    = 0;
    total_expert_bytes_ = 0;
    disjoint_vram_mode_ = false;
    vram_rescue_fn_     = nullptr;

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
    ram_to_vram_swaps_.store(0, std::memory_order_relaxed);
    vram_to_ram_swaps_.store(0, std::memory_order_relaxed);
    commit_failures_.store(0, std::memory_order_relaxed);
    preload_mbs_.store(0.0, std::memory_order_relaxed);
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

    const uint64_t raw_cap = nbytes + page_size_;
    const uint64_t cap     = ((raw_cap + page_size_ - 1) / page_size_) * page_size_;
    uint8_t * base         = reserve_virtual_range((size_t) cap);
    if (!base) {
        err = std::string("Не удалось зарезервировать виртуальное адресное пространство (MEM_RESERVE) для ") + tensor->name;
        return false;
    }
    if (!commit_pages_os(base + cap - page_size_, page_size_)) {
        release_virtual_range(base, (size_t) cap);
        err = std::string("Исчерпан лимит виртуальной памяти Windows (Commit Limit / pagefile.sys) при создании guard-страницы для ") + tensor->name;
        return false;
    }

    StreamedProjection P;
    P.tensor      = tensor;
    P.name        = tensor->name ? tensor->name : "";
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

    tensor->data = base;

    StreamedLayer & L = layers_[(size_t) il];
    L.il = il;
    const int32_t proj_idx = (int32_t) L.projs.size();
    L.bytes_per_expert += nb2;
    total_expert_bytes_ += nbytes;
    L.projs.push_back(std::move(P));
    tensor_to_proj_[tensor] = {il, proj_idx};
    return true;
}

int32_t VirtualExpertArena::find_proj_by_name(int32_t il, const char * tensor_name) const {
    if (!tensor_name || !has_layer(il)) return -1;
    const StreamedLayer & L = layers_[(size_t) il];
    for (size_t i = 0; i < L.projs.size(); ++i) {
        if (L.projs[i].name == tensor_name) {
            return (int32_t) i;
        }
    }
    return -1;
}

bool VirtualExpertArena::rehome_dense_weights(const std::vector<DenseRehomeEntry> & entries, std::string & err) {
    if (entries.empty()) return true;

    // Sort dense entries by ascending (shard_idx, file_offset) so HDD reads sequentially!
    std::vector<size_t> order(entries.size());
    for (size_t i = 0; i < entries.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (entries[a].shard_idx != entries[b].shard_idx) return entries[a].shard_idx < entries[b].shard_idx;
        return entries[a].file_offset < entries[b].file_offset;
    });

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
    lock_pages_best_effort(buf, (size_t) total);

    uint64_t cursor = 0;
    for (size_t idx : order) {
        const auto & e = entries[idx];
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

bool VirtualExpertArena::fetch_expert_for_vram_prewarm(int32_t il,
                                                       const char * tensor_name,
                                                       int32_t exp,
                                                       void * dst_buf,
                                                       size_t nbytes) {
    if (!dst_buf || exp < 0 || exp >= n_expert_ || !has_layer(il)) return false;
    const int32_t pi = find_proj_by_name(il, tensor_name);
    if (pi < 0) return false;

    std::lock_guard<std::mutex> lk(mu_);
    StreamedProjection & P = layers_[(size_t) il].projs[(size_t) pi];
    if (nbytes != P.nb2) return false;

    CacheSlotMeta & m = slot_meta_[(size_t) il * (size_t) n_expert_ + (size_t) exp];

    // Fast path: if already committed in DDR4 RAM (mirrored mode), copy straight from RAM with 0 disk I/O!
    if (m.resident && P.state[(size_t) exp].load(std::memory_order_acquire) == kStateReady) {
        std::memcpy(dst_buf, P.arena_base + (uint64_t) exp * P.nb2, nbytes);
        m.in_vram = true;
        return true;
    }

    // Disjoint or pre-RAM path: read from 16 MiB contiguous sequential read-ahead buffer (0 random HDD seeks!)
    const uint64_t slice_off = P.file_offset + (uint64_t) exp * P.nb2;
    const uint64_t proj_end  = P.file_offset + P.total_bytes;

    if (prewarm_ra_.shard_idx != P.shard_idx ||
        slice_off < prewarm_ra_.base_off ||
        slice_off + nbytes > prewarm_ra_.base_off + prewarm_ra_.valid_len) {
        const size_t kRaChunk = std::max<size_t>(16ull << 20, nbytes);
        if (prewarm_ra_.buf.size() < kRaChunk) {
            prewarm_ra_.buf.resize(kRaChunk);
        }
        const uint64_t max_avail = proj_end > slice_off ? (proj_end - slice_off) : nbytes;
        const size_t   to_read   = (size_t) std::min<uint64_t>(kRaChunk, max_avail);
        const auto t0 = std::chrono::steady_clock::now();
        if (!pread_exact(P.shard_idx, prewarm_ra_.buf.data(), to_read, slice_off)) {
            return false;
        }
        const auto t1 = std::chrono::steady_clock::now();
        prewarm_ra_.shard_idx = P.shard_idx;
        prewarm_ra_.base_off  = slice_off;
        prewarm_ra_.valid_len = to_read;
        prewarm_disk_bytes_  += to_read;
        prewarm_disk_ns_     += (uint64_t) std::max<int64_t>(1,
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }

    std::memcpy(dst_buf, prewarm_ra_.buf.data() + (slice_off - prewarm_ra_.base_off), nbytes);
    m.in_vram = true;
    if (disjoint_vram_mode_ && !m.resident) {
        P.state[(size_t) exp].store(kStateInVram, std::memory_order_release);
    }
    return true;
}

uint64_t VirtualExpertArena::preload_remaining_to_ram_sequential(
    uint64_t max_ram_bytes,
    const std::function<void(uint64_t done_bytes, uint64_t target_bytes, double mbs)> & progress_cb) {
    // Release temporary prewarm read-ahead buffer first so RAM is free for experts
    {
        std::lock_guard<std::mutex> lk(mu_);
        prewarm_ra_.buf.clear();
        prewarm_ra_.buf.shrink_to_fit();
        prewarm_ra_.valid_len = 0;
        prewarm_ra_.shard_idx = -1;
    }

    struct ContigSpan {
        int32_t  il        = 0;
        int32_t  proj_idx  = 0;
        int32_t  shard_idx = 0;
        int32_t  exp_beg   = 0;
        int32_t  exp_end   = 0;
        uint64_t file_off  = 0;
        uint64_t nbytes    = 0;
        uint8_t * dst      = nullptr;
    };

    std::vector<ContigSpan> spans;
    uint64_t total_planned = 0;

    {
        std::lock_guard<std::mutex> lk(mu_);
        const uint64_t cap_bytes = max_ram_bytes > 0 ? max_ram_bytes : budget_bytes_;
        const uint64_t epoch     = ++pin_epoch_;

        // Commit experts in layer-interleaved order up to safe DDR4 RAM cap so every layer gets warm RAM coverage,
        // THEN sort all resulting file spans in strictly ascending (shard_idx, file_offset) order for 1 sequential HDD sweep!
        for (int32_t exp = 0; exp < n_expert_; ++exp) {
            bool any_committed_this_pass = false;
            for (int32_t il = 0; il < (int32_t) layers_.size(); ++il) {
                StreamedLayer & L = layers_[(size_t) il];
                if (L.projs.empty()) continue;

                CacheSlotMeta & m = slot_meta_[(size_t) il * (size_t) n_expert_ + (size_t) exp];
                if (m.resident) continue;
                if (disjoint_vram_mode_ && m.in_vram) {
                    // Already held in RX 580 VRAM — skip committing in DDR4 RAM to save up to 5 GB of physical RAM!
                    continue;
                }
                if (committed_bytes_.load(std::memory_order_relaxed) + L.bytes_per_expert > cap_bytes) {
                    continue;
                }
                if (!commit_expert_locked(il, exp, epoch)) {
                    break;
                }
                m.resident  = true;
                m.crf       = 1.0f;
                m.last_step = step_clock_;
                total_planned += L.bytes_per_expert;
                any_committed_this_pass = true;
            }
            if (!any_committed_this_pass && committed_bytes_.load(std::memory_order_relaxed) >= cap_bytes) {
                break;
            }
        }

        // Build coalesced contiguous projection spans for every newly committed expert
        for (int32_t il = 0; il < (int32_t) layers_.size(); ++il) {
            StreamedLayer & L = layers_[(size_t) il];
            if (L.projs.empty()) continue;
            int32_t run_beg = -1;
            for (int32_t exp = 0; exp <= n_expert_; ++exp) {
                const bool want = (exp < n_expert_) &&
                    slot_meta_[(size_t) il * (size_t) n_expert_ + (size_t) exp].resident &&
                    L.projs[0].state[(size_t) exp].load(std::memory_order_relaxed) != kStateReady;
                if (want) {
                    if (run_beg < 0) run_beg = exp;
                } else if (run_beg >= 0) {
                    for (int32_t pi = 0; pi < (int32_t) L.projs.size(); ++pi) {
                        StreamedProjection & P = L.projs[(size_t) pi];
                        const uint64_t off = P.file_offset + (uint64_t) run_beg * P.nb2;
                        const uint64_t len = (uint64_t) (exp - run_beg) * P.nb2;
                        spans.push_back(ContigSpan{
                            il, pi, P.shard_idx, run_beg, exp, off, len,
                            P.arena_base + (uint64_t) run_beg * P.nb2
                        });
                    }
                    run_beg = -1;
                }
            }
        }
    }

    // Sort strictly by ascending (shard_idx, file_off) -> ZERO mechanical HDD random seeks!
    std::sort(spans.begin(), spans.end(), [](const ContigSpan & a, const ContigSpan & b) {
        if (a.shard_idx != b.shard_idx) return a.shard_idx < b.shard_idx;
        return a.file_off < b.file_off;
    });

    uint64_t done_bytes = 0;
    const auto t_start  = std::chrono::steady_clock::now();
    auto t_last_cb      = t_start;

    for (const ContigSpan & sp : spans) {
        const bool ok = pread_exact(sp.shard_idx, sp.dst, (size_t) sp.nbytes, sp.file_off);
        if (!ok) {
            std::memset(sp.dst, 0, (size_t) sp.nbytes);
        }
        lock_pages_best_effort(sp.dst, (size_t) sp.nbytes);

        {
            std::lock_guard<std::mutex> lk(mu_);
            StreamedProjection & P = layers_[(size_t) sp.il].projs[(size_t) sp.proj_idx];
            for (int32_t e = sp.exp_beg; e < sp.exp_end; ++e) {
                P.state[(size_t) e].store(kStateReady, std::memory_order_release);
            }
        }

        done_bytes += sp.nbytes;
        const auto now = std::chrono::steady_clock::now();
        const double total_sec = std::max(1e-6,
            std::chrono::duration<double>(now - t_start).count() + (double) prewarm_disk_ns_ / 1e9);
        const double mbs = ((double) (done_bytes + prewarm_disk_bytes_) / (1024.0 * 1024.0)) / total_sec;
        preload_mbs_.store(mbs, std::memory_order_relaxed);

        if (progress_cb && (done_bytes == total_planned ||
            std::chrono::duration_cast<std::chrono::milliseconds>(now - t_last_cb).count() >= 180)) {
            t_last_cb = now;
            progress_cb(done_bytes, std::max<uint64_t>(1, total_planned), mbs);
        }
    }

    if (done_bytes == 0 && prewarm_disk_bytes_ > 0 && prewarm_disk_ns_ > 0) {
        const double mbs = ((double) prewarm_disk_bytes_ / (1024.0 * 1024.0)) / ((double) prewarm_disk_ns_ / 1e9);
        preload_mbs_.store(mbs, std::memory_order_relaxed);
    }

    ready_cv_.notify_all();
    return done_bytes;
}

void VirtualExpertArena::mark_preload_complete() {
    ssd_reads_.store(0, std::memory_order_relaxed);
    ssd_bytes_read_.store(0, std::memory_order_relaxed);
    ssd_active_ns_.store(0, std::memory_order_relaxed);
}

void * VirtualExpertArena::prepare_vram_readback_dst(int32_t il,
                                                     const char * tensor_name,
                                                     int32_t exp,
                                                     size_t nbytes,
                                                     bool keep_in_vram) {
    if (exp < 0 || exp >= n_expert_ || !has_layer(il)) return nullptr;
    const int32_t pi = find_proj_by_name(il, tensor_name);
    if (pi < 0) return nullptr;

    std::lock_guard<std::mutex> lk(mu_);
    // Any earlier layer's async DMA promotion has already completed past llama_moe_cache::copy()'s
    // router readback synchronization fence, so retire those DDR4 pages before committing new ones!
    retire_completed_promotions_locked();

    StreamedLayer & L      = layers_[(size_t) il];
    StreamedProjection & P = L.projs[(size_t) pi];
    if (nbytes != P.nb2) return nullptr;

    CacheSlotMeta & m = slot_meta_[(size_t) il * (size_t) n_expert_ + (size_t) exp];
    if (!keep_in_vram) {
        m.in_vram = false;
    }

    // Check strictly THIS projection's state BEFORE modifying sibling projections!
    // In Mirrored mode (or if already rescued), THIS projection already holds valid weights in DDR4 RAM.
    if (m.resident && P.state[(size_t) exp].load(std::memory_order_acquire) == kStateReady) {
        return nullptr;
    }

    // Disjoint mode: commit DDR4 slice for all projections of (il, exp) once on ip=0, while leaving
    // sibling projections (ip=1, ip=2) in non-ready state until their own evict_cb / rescue_cb copies them!
    if (!m.resident) {
        const uint64_t epoch = ++pin_epoch_;
        m.pin_epoch = epoch;
        if (budget_bytes_ > 0 && committed_bytes_.load(std::memory_order_relaxed) > budget_bytes_ + L.bytes_per_expert * 3) {
            evict_to_budget_locked(L.bytes_per_expert, epoch);
        }
        if (!commit_expert_locked(il, exp, epoch)) {
            return nullptr;
        }
        m.resident  = true;
        m.crf       = std::max(0.5f, m.crf);
        m.last_step = step_clock_;
        for (size_t j = 0; j < L.projs.size(); ++j) {
            if ((int32_t) j != pi) {
                L.projs[j].state[(size_t) exp].store(keep_in_vram ? kStateInVram : kStateEmpty, std::memory_order_release);
            }
        }
    }

    P.state[(size_t) exp].store(kStateReady, std::memory_order_release);
    return P.arena_base + (uint64_t) exp * P.nb2;
}

void * VirtualExpertArena::prepare_vram_eviction_dst(int32_t evicted_il,
                                                     const char * tensor_name,
                                                     int32_t evicted_exp,
                                                     size_t nbytes) {
    return prepare_vram_readback_dst(evicted_il, tensor_name, evicted_exp, nbytes, /*keep_in_vram=*/false);
}

void * VirtualExpertArena::prepare_vram_rescue_dst(int32_t il,
                                                   const char * tensor_name,
                                                   int32_t exp,
                                                   size_t nbytes) {
    return prepare_vram_readback_dst(il, tensor_name, exp, nbytes, /*keep_in_vram=*/true);
}

void VirtualExpertArena::retire_completed_promotions_locked() {
    if (pending_promoted_.empty()) return;
    for (const auto & pr : pending_promoted_) {
        const int32_t old_il  = pr.first;
        const int32_t old_exp = pr.second;
        if (!has_layer(old_il) || old_exp < 0 || old_exp >= n_expert_) continue;
        CacheSlotMeta & m = slot_meta_[(size_t) old_il * (size_t) n_expert_ + (size_t) old_exp];
        // Only decommit if the expert is STILL live in VRAM (not evicted in the meantime)
        if (disjoint_vram_mode_ && m.in_vram && m.resident) {
            evict_expert_locked(old_il, old_exp, kStateInVram);
            m.in_vram = true;
        }
    }
    pending_promoted_.clear();
}

void VirtualExpertArena::flush_deferred_promotions() {
    std::lock_guard<std::mutex> lk(mu_);
    retire_completed_promotions_locked();
}

void VirtualExpertArena::on_experts_promoted_to_vram(int32_t il,
                                                     const int32_t * promoted_experts,
                                                     size_t n_promoted,
                                                     size_t n_evicted_to_ram) {
    if (n_promoted > 0) {
        ram_to_vram_swaps_.fetch_add(n_promoted, std::memory_order_relaxed);
    }
    if (n_evicted_to_ram > 0) {
        vram_to_ram_swaps_.fetch_add(n_evicted_to_ram, std::memory_order_relaxed);
    }

    std::lock_guard<std::mutex> lk(mu_);
    // Retire promotions from previous layer(s) whose async DMA has already crossed copy()'s sync barrier
    retire_completed_promotions_locked();

    if (!promoted_experts || n_promoted == 0 || !has_layer(il)) return;

    for (size_t i = 0; i < n_promoted; ++i) {
        const int32_t e = promoted_experts[i];
        if (e < 0 || e >= n_expert_) continue;
        CacheSlotMeta & m = slot_meta_[(size_t) il * (size_t) n_expert_ + (size_t) e];
        m.in_vram = true;
        if (disjoint_vram_mode_ && m.resident) {
            // Defer DDR4 page decommit until the next layer/decode barrier so async Vulkan DMA upload
            // reads intact host memory while overlapping parallel CPU compute with 0 extra GPU stalls!
            pending_promoted_.emplace_back(il, e);
        }
    }
}

uint64_t VirtualExpertArena::silicon_resident_bytes(uint64_t vram_bytes) const {
    const uint64_t ram = committed_bytes_.load(std::memory_order_relaxed);
    if (disjoint_vram_mode_) {
        return std::min<uint64_t>(total_expert_bytes_, ram + vram_bytes);
    }
    return std::min<uint64_t>(total_expert_bytes_, std::max<uint64_t>(ram, vram_bytes));
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
    evict_expert_locked(best_il, best_exp, kStateEmpty);
    return true;
}

bool VirtualExpertArena::commit_pages_with_recovery_locked(uint8_t * ptr, size_t bytes, uint64_t current_pin_epoch) {
    if (commit_pages_os(ptr, bytes)) return true;
    commit_failures_.fetch_add(1, std::memory_order_relaxed);
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

void VirtualExpertArena::evict_expert_locked(int32_t il, int32_t exp, uint8_t post_state) {
    StreamedLayer & L = layers_[(size_t) il];
    uint64_t freed = 0;
    for (StreamedProjection & P : L.projs) {
        P.state[(size_t) exp].store(post_state, std::memory_order_release);
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
    std::vector<int32_t> need_vram_rescue;
    bool queued_any = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        ++step_clock_;
        const uint64_t epoch = ++pin_epoch_;
        StreamedLayer & L = layers_[(size_t) il];

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
            if (m.in_vram && vram_rescue_fn_) {
                need_vram_rescue.push_back(e);
                continue;
            }

            evict_to_budget_locked(L.bytes_per_expert, epoch);
            if (!commit_expert_locked(il, e, epoch)) {
                for (int32_t p = 0; p < (int32_t) L.projs.size(); ++p) {
                    L.projs[(size_t) p].state[(size_t) e].store(kStateReady, std::memory_order_release);
                }
                continue;
            }
            m.resident = true;
            ssd_reads_.fetch_add(1, std::memory_order_relaxed);

            for (int32_t p = 0; p < (int32_t) L.projs.size(); ++p) {
                StreamedProjection & P = L.projs[(size_t) p];
                P.state[(size_t) e].store(kStateLoading, std::memory_order_release);
                io_queue_.push_back(IoJob{il, p, e, P.shard_idx, P.file_offset + (uint64_t) e * P.nb2});
                queued_any = true;
            }
        }

        // HDD head-saver: keep pending jobs sorted by ascending file offset so 1-lane HDD reader sweeps smoothly!
        if (queued_any && sequential_hdd_mode_ && io_queue_.size() > 1) {
            std::sort(io_queue_.begin(), io_queue_.end(), [](const IoJob & a, const IoJob & b) {
                if (a.shard != b.shard) return a.shard < b.shard;
                return a.file_off < b.file_off;
            });
        }
    }

    // Rescue any requested VRAM-exclusive expert directly over PCIe instead of touching the HDD!
    for (int32_t e : need_vram_rescue) {
        if (vram_rescue_fn_ && vram_rescue_fn_(il, e)) {
            ram_hits_.fetch_add(1, std::memory_order_relaxed);
            ready_cv_.notify_all();
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

    const uint8_t cur = cell.load(std::memory_order_acquire);
    if (cur == kStateReady) {
        return;
    }

    if (cur == kStateInVram && vram_rescue_fn_) {
        if (vram_rescue_fn_(il, expert_id) && cell.load(std::memory_order_acquire) == kStateReady) {
            return;
        }
    }

    if (cell.load(std::memory_order_acquire) == kStateEmpty || cell.load(std::memory_order_acquire) == kStateInVram) {
        // Temporarily clear in_vram if rescue didn't populate so fallback disk loader can populate it
        {
            std::lock_guard<std::mutex> lk(mu_);
            slot_meta_[(size_t) il * (size_t) n_expert_ + (size_t) expert_id].in_vram = false;
        }
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
