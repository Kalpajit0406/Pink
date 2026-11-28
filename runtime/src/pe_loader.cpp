#include "pe_loader.h"

#include <windows.h>
#include <intrin.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <vector>

#include "../shims/shims.h"
#include "../shims/startup_trace.h"
#include "base_reservation.h"
#include "clzero_patch.h"
#include "cpu_emulation.h"
#include "patch_table_loader.h"

namespace rt {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

namespace {

std::filesystem::path::string_type module_key(const std::filesystem::path& path) {
    // Keep identity in the platform's native path representation. Only the existing ASCII case folding is
    // applied here; representable names also retain their old narrow-byte alias below.
    auto key = path.filename().native();
    for (auto& c : key)
        if (c >= 'A' && c <= 'Z') c = static_cast<std::filesystem::path::value_type>(c + ('a' - 'A'));
    return key;
}

std::string narrow_module_key(const std::filesystem::path& path) {
    const auto filename = path.filename();
    try {
        const auto narrow = filename.string();
        // A lossy/default-character alias must not collapse two distinct Unicode basenames.
        if (std::filesystem::path(narrow).native() == filename.native()) return lower(narrow);
    } catch (const std::system_error&) {
        // Some Windows standard libraries throw when a basename is outside the current narrow code page.
        // It still has a native identity; it simply has no alias in the existing narrow lookup API.
    }
    return {};
}

// Windows matches module names case-insensitively with its own upcase table (RtlEqualUnicodeString); the
// ordinal ignore-case compare is that table.
bool same_module_name(const wchar_t* a, const wchar_t* b) {
    return CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
}

// A narrow (ANSI code page) module name widened for a case-insensitive compare; empty when it is not valid in the
// code page.
std::wstring acp_to_wide(const std::string& narrow) {
    if (narrow.empty()) return {};
    const int n = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, narrow.data(), static_cast<int>(narrow.size()),
                                      nullptr, 0);
    if (n <= 0) return {};
    std::wstring wide(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, narrow.data(), static_cast<int>(narrow.size()), wide.data(), n);
    return wide;
}

std::string path_utf8(const std::filesystem::path& path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::vector<uint8_t> read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path_utf8(p));
    return {std::istreambuf_iterator<char>(f), {}};
}

template <class T>
T* at(uint8_t* base, uint32_t rva) { return reinterpret_cast<T*>(base + rva); }

// Every RVA followed below comes out of the image itself, so each one is checked first. A truncated or corrupt
// image then fails with a message instead of reading, writing or protecting memory outside its allocation.
void require_range(const std::string& name, uint64_t rva, uint64_t len, uint32_t image_size, const char* what) {
    if (rva > image_size || len > image_size - rva)
        throw std::runtime_error(name + ": " + what + " runs past the image");
}

std::string read_string(const std::string& name, uint8_t* base, uint32_t image_size, uint32_t rva, const char* what) {
    if (rva >= image_size) throw std::runtime_error(name + ": " + what + " is outside the image");
    auto* s = reinterpret_cast<const char*>(base + rva);
    auto* end = static_cast<const char*>(std::memchr(s, 0, image_size - rva));
    if (!end) throw std::runtime_error(name + ": " + what + " isn't terminated inside the image");
    return {s, end};
}

// PE/COFF: NumberOfRvaAndSizes counts the data directories; entries past it are not directories, whatever they hold.
IMAGE_DATA_DIRECTORY directory(const IMAGE_NT_HEADERS64* nt, unsigned index) {
    return index < nt->OptionalHeader.NumberOfRvaAndSizes ? nt->OptionalHeader.DataDirectory[index] : IMAGE_DATA_DIRECTORY{};
}

// The TLS directory holds virtual addresses, which relocations have already moved to this mapping.
uint64_t rva_in(const Module& m, uint64_t va, const char* what) {
    const uint64_t base = reinterpret_cast<uint64_t>(m.base);
    if (va < base || va - base >= m.size) throw std::runtime_error(m.name + ": " + what + " is outside the image");
    return va - base;
}

IMAGE_TLS_DIRECTORY64* tls_directory(Module& m) {
    auto* nt = at<IMAGE_NT_HEADERS64>(m.base, reinterpret_cast<IMAGE_DOS_HEADER*>(m.base)->e_lfanew);
    // Through directory(), as every other directory: an entry past NumberOfRvaAndSizes is not a TLS directory.
    const auto dir = directory(nt, IMAGE_DIRECTORY_ENTRY_TLS);
    if (!dir.Size || !dir.VirtualAddress) return nullptr;
    require_range(m.name, dir.VirtualAddress, sizeof(IMAGE_TLS_DIRECTORY64), m.size, "the TLS directory");
    return at<IMAGE_TLS_DIRECTORY64>(m.base, dir.VirtualAddress);
}

// __declspec(thread) reads TEB->ThreadLocalStoragePointer[_tls_index] at gs:[58h]. That array is indexed by the
// loader-assigned per-module TLS index and is a different index space from TlsAlloc/TlsGetValue (TlsSlots). Only the OS
// loader can allocate a module index, so the runtime ships a minimal DLL whose only content is a TLS anchor
// (src/tls_host.cpp). Loading it makes the OS reserve an index and give every thread a block for it; the native loader
// reuses that index for the mapped image. When more than one manually mapped module uses static TLS at once, the loader
// clones gta6rt_tls_host.dll to a per-process temporary path on demand and loads it, so the OS assigns an independent
// module TLS index for each simultaneous module (up to kMaxMappedTlsModules).
//
// A mapped executable is different: MSVC code built with /GA (an EXE-only optimisation) skips _tls_index and reads
// TEB->ThreadLocalStoragePointer[0] directly (mov rax, gs:[58h]; mov rax, [rax]), because Windows always gives the
// process executable module TLS index 0. Measured with a `thread_local int v = 42;` EXE built /O2 /GA /MD: natively
// tls_index=0 value=42; mapped with a helper index (6) the code still read slot 0 and printed value=0. Index 0
// belongs to the runtime's own EXE (gta6rt.exe: a 1398-byte template when measured, no TLS callbacks; so does each
// test EXE that links this file), and every TLS read in its disassembly goes through _tls_index (the only direct
// gs:[58h] read is module_tls_slots below, which wants the array itself). reserve_tls_index0 therefore moves the owner
// off index 0, once, at process start: it loads the home clone (make_home_clone_image), whose TLS template is a copy of
// the owner's, and on the calling thread swaps slots 0 and home and writes home into the owner's _tls_index. From then on every thread the OS
// starts has a fresh copy of the owner's template at home, which is where the owner looks, from its first instruction:
// nothing has to run on a new thread first. Slot 0 holds a block nobody reads, which the mapped executable's copy
// replaces (kMainImageSlot) exactly as a DLL's copy replaces a helper's, and the OS frees both slots at thread exit.
// It has to happen before any other thread has run the owner's code: MSVC keeps &ThreadLocalStoragePointer[_tls_index]
// across calls (zlib_inflate in shader_library.h saves it at [rbp-80h] across memset, push_back and inflate_codes, then
// reads _Init_thread_epoch through it), so a running thread cannot be moved safely, wherever it is stopped. Such a
// thread's TLS would also stay where it was, at index 0.
constexpr DWORD kMaxModuleTlsSlots = 64;
constexpr size_t kMaxMappedTlsModules = 16;
// The slot for a mapped executable, which reads module TLS index 0 (see above). Only one at a time; a second
// executable mapped while one is live gets an ordinary helper slot. kTlsSlotCount sizes the per-slot tables.
constexpr size_t kMainImageSlot = kMaxMappedTlsModules;
constexpr size_t kTlsSlotCount = kMaxMappedTlsModules + 1;
void** module_tls_slots() { return reinterpret_cast<void**>(__readgsqword(0x58)); }

void __cdecl on_host_thread_event(DWORD reason);

struct TlsHostModule {
    HMODULE module = nullptr;
    DWORD index = TLS_OUT_OF_INDEXES;
    bool hooked = false;  // its DllMain reports thread events to on_host_thread_event
};

std::filesystem::path tls_host_base_path() {
    std::wstring exe_path;
    DWORD size = MAX_PATH;
    while (true) {
        exe_path.resize(size);
        DWORD len = GetModuleFileNameW(nullptr, exe_path.data(), size);
        if (len == 0) return {};
        if (len < size - 1) {
            exe_path.resize(len);
            break;
        }
        size *= 2;
    }
    return std::filesystem::path(exe_path).parent_path() / L"gta6rt_tls_host.dll";
}

std::filesystem::path tls_host_clone_dir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, buf);
    if (n > 0 && n < MAX_PATH) return std::filesystem::path(buf);
    return std::filesystem::temp_directory_path();
}

struct TlsCloneRecord {
    HMODULE module = nullptr;
    std::filesystem::path path;
};

std::mutex g_tls_alloc_mutex;
std::array<TlsHostModule, kMaxMappedTlsModules> g_tls_hosts{};
std::array<std::atomic<Module*>, kTlsSlotCount> g_tls_slot_owners{};
std::array<std::atomic<uint64_t>, kTlsSlotCount> g_tls_slot_generations{};
std::atomic<uint64_t> g_next_tls_generation{1};
// Allocated with the first clone and never destroyed: cleanup_tls_clones_at_exit runs from the atexit table, and a
// static vector's destructor (registered when its dynamic initializer ran) could run before it. Under
// g_tls_alloc_mutex, as are its records.
std::vector<TlsCloneRecord>* g_tls_clones = nullptr;

// The move of module TLS index 0 (see kMainImageSlot): done at most once, by reserve_tls_index0 under
// g_tls_alloc_mutex. `why` is the reason it was not done, for the executable that then maps with a helper index.
// Constant-initialized only (no std::string): gta6rt reserves from a CRT initializer that runs before the dynamic
// initializers of this file, which would otherwise reset what it wrote.
struct Index0Move {
    bool moved = false;
    bool traced = false;    // the startup trace opens after the move; the first executable map reports it
    TlsHostModule home;     // the clone whose index holds the owner's TLS from now on; never freed
    char owner[260] = {};   // UTF-8 file name of the image that held index 0
    char why[160] = "reserve_tls_index0 was not called at process start";
};
Index0Move g_index0;

// Frees every TLS helper clone recorded for a mapped DLL's slot and deletes its files. ensure_tls_host registers the
// wrapper with atexit when it records the first clone; the --run path in main() must also call this directly before
// ExitProcess, which skips the atexit table. Call it only when no mapped code can run again: it unmaps the helper
// images. The home clone is not recorded (reserve_index0): it stays loaded, so its file outlives the process and the
// next run's sweep deletes it.
void cleanup_tls_clones_internal() {
    if (!g_tls_clones) return;
    for (const auto& rec : *g_tls_clones) {
        if (rec.module) FreeLibrary(rec.module);
        if (!rec.path.empty()) DeleteFileW(rec.path.c_str());
    }
    g_tls_clones->clear();
}

void cleanup_tls_clones_at_exit() { cleanup_tls_clones_internal(); }

// The owner process named by "leonida_tls_host_<pid>_<slot>.dll"; 0 when the name does not carry one.
DWORD clone_owner_pid(const wchar_t* name) {
    constexpr wchar_t prefix[] = L"leonida_tls_host_";
    const wchar_t* p = name;
    for (size_t i = 0; i < sizeof(prefix) / sizeof(wchar_t) - 1; ++i)
        if (*p++ != prefix[i]) return 0;
    if (*p < L'0' || *p > L'9') return 0;
    unsigned long pid = 0;
    while (*p >= L'0' && *p <= L'9') {
        pid = pid * 10 + (*p - L'0');
        if (pid > 0xFFFFFFFFul) return 0;
        ++p;
    }
    return static_cast<DWORD>(pid);
}

bool process_alive(DWORD pid) {
    // PROCESS_QUERY_LIMITED_INFORMATION needs no handle rights a normal process could deny; a failure means the
    // process is gone. A still-mapped or still-open file is caught by the checks below even for a misjudged PID.
    // OpenProcess also succeeds on a process that has ended while another process still holds a handle to it (a
    // parent waiting on its exit code, as CMake's execute_process does), so the exit code decides.
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    DWORD code = 0;
    const bool alive = !GetExitCodeProcess(p, &code) || code == STILL_ACTIVE;
    CloseHandle(p);
    return alive;
}

// Deletes clones left behind by processes that have exited. Deletion is scoped to files whose owner is gone: a
// clone of a live process is either still open by its copy step's handle or mapped as an image, so it is skipped
// (or DeleteFileW fails harmlessly), and a concurrent process between CopyFileW and LoadLibraryExW keeps its file.
void sweep_stale_tls_clones(const std::filesystem::path& dir) {
    WIN32_FIND_DATAW fd{};
    const std::filesystem::path pattern = dir / L"leonida_tls_host_*.dll";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::filesystem::path path = dir / fd.cFileName;
        const DWORD owner = clone_owner_pid(fd.cFileName);
        if (owner && owner != GetCurrentProcessId() && process_alive(owner)) continue;
        // An exclusive open (no sharing) conflicts with any handle another process holds on the file, so a failure
        // means the copy->load sequence or a mapped image still owns it. Only unowned files reach DeleteFileW, and
        // that stays harmless for a mapped image.
        HANDLE probe =
            CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (probe == INVALID_HANDLE_VALUE) continue;
        CloseHandle(probe);
        DeleteFileW(path.c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

TlsHostModule load_tls_host_image(const std::filesystem::path& path, bool hook_thread_events) {
    TlsHostModule host;
    if (path.empty()) return host;
    // The helper is linked with the static CRT (/MT), so it needs only system DLLs. Restricting the dependency search
    // to System32 keeps a game-supplied vcruntime next to the executable from being picked up.
    host.module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!host.module) return host;

    auto* base = reinterpret_cast<uint8_t*>(host.module);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt = at<IMAGE_NT_HEADERS64>(base, dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!dir.Size || !dir.VirtualAddress) {
        FreeLibrary(host.module);
        host.module = nullptr;
        return host;
    }
    auto* tls = at<IMAGE_TLS_DIRECTORY64>(base, dir.VirtualAddress);
    if (!tls->AddressOfIndex) {
        FreeLibrary(host.module);
        host.module = nullptr;
        return host;
    }
    host.index = *reinterpret_cast<DWORD*>(tls->AddressOfIndex);
    if (hook_thread_events) {
        // Only the primary helper reports thread attach/detach events; on_host_thread_event iterates all active slots.
        using SetThreadEvent = void(__cdecl*)(void(__cdecl*)(DWORD));
        auto set = reinterpret_cast<SetThreadEvent>(GetProcAddress(host.module, "gta6rt_tls_host_set_thread_event"));
        if (set) {
            set(&on_host_thread_event);
            host.hooked = true;
        }
    }
    return host;
}

// The primary helper: slot 0's index, and the only helper whose DllMain reports thread events (on_host_thread_event
// covers every slot). Must be called with g_tls_alloc_mutex held and WITHOUT g_tls_lock held (LoadLibraryExW takes the
// Windows loader lock).
// A helper without the thread-event export is refused: no thread started later would get its copies.
bool ensure_primary_tls_host() {
    if (g_tls_hosts[0].index != TLS_OUT_OF_INDEXES) return true;
    const std::filesystem::path primary = tls_host_base_path();
    if (primary.empty()) return false;
    TlsHostModule host = load_tls_host_image(primary, true);
    if (host.index != TLS_OUT_OF_INDEXES && !host.hooked) {
        FreeLibrary(host.module);
        return false;
    }
    g_tls_hosts[0] = host;
    return host.index != TLS_OUT_OF_INDEXES;
}

// Copies gta6rt_tls_host.dll (or writes `image`, a changed copy of it) to "leonida_tls_host_<pid>_<tag>.dll" in the
// clone directory and loads it, so the OS gives the copy an index of its own. On failure the file is gone again and
// the result has no index. The caller records the clone (g_tls_clones) if it is to be freed at exit. Same locking rule
// as ensure_primary_tls_host.
TlsHostModule load_tls_host_clone(const std::wstring& tag, std::filesystem::path& clone_path,
                                  const std::vector<uint8_t>* image = nullptr) {
    const std::filesystem::path primary = tls_host_base_path();
    if (primary.empty()) return {};
    // Clone gta6rt_tls_host.dll to a distinct path using CopyFileW (Wine 9 lacks CopyFile2 used by std::filesystem).
    const std::filesystem::path dir = tls_host_clone_dir();
    static bool swept = false;
    if (!swept) {
        swept = true;
        sweep_stale_tls_clones(dir);
    }
    const std::wstring name = L"leonida_tls_host_" + std::to_wstring(GetCurrentProcessId()) + L"_" + tag + L".dll";
    clone_path = dir / name;
    // The staging name must not match the sweep pattern, which only covers final clone names ending in .dll.
    const std::filesystem::path staging = clone_path.wstring() + L".new";
    if (image) {
        HANDLE out =
            CreateFileW(staging.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (out == INVALID_HANDLE_VALUE) return {};
        DWORD written = 0;
        const bool ok = WriteFile(out, image->data(), static_cast<DWORD>(image->size()), &written, nullptr) &&
                        written == image->size();
        CloseHandle(out);
        if (!ok) {
            DeleteFileW(staging.c_str());
            return {};
        }
    } else if (!CopyFileW(primary.c_str(), staging.c_str(), FALSE)) {
        return {};
    }

    // Open the guard before the clone exists under its final name and hold it across LoadLibraryExW: the rename
    // below needs DELETE access, which the handle's FILE_SHARE_DELETE grants, and no purge can remove the clone
    // while the handle is open (an exclusive open conflicts with it) or once the image is mapped. Copying straight
    // to the final name would leave a window where a concurrent sweep sees an unowned file.
    HANDLE guard = CreateFileW(staging.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (guard == INVALID_HANDLE_VALUE) {
        DeleteFileW(staging.c_str());
        return {};
    }
    if (!MoveFileExW(staging.c_str(), clone_path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        CloseHandle(guard);
        DeleteFileW(staging.c_str());
        return {};
    }
    TlsHostModule loaded = load_tls_host_image(clone_path, false);
    CloseHandle(guard);

    if (loaded.index == TLS_OUT_OF_INDEXES) DeleteFileW(clone_path.c_str());
    return loaded;
}

// Must be called with g_tls_alloc_mutex held and WITHOUT g_tls_lock held (LoadLibraryExW takes the Windows loader lock).
TlsHostModule ensure_tls_host(size_t slot) {
    if (slot >= kMaxMappedTlsModules) return {};
    if (g_tls_hosts[slot].index != TLS_OUT_OF_INDEXES) return g_tls_hosts[slot];
    // Slot 0 is the primary helper; it is loaded first in any case so thread attach/detach events are hooked.
    if (!ensure_primary_tls_host()) return {};
    if (slot == 0) return g_tls_hosts[0];

    std::filesystem::path clone_path;
    TlsHostModule loaded = load_tls_host_clone(std::to_wstring(slot), clone_path);
    if (loaded.index == TLS_OUT_OF_INDEXES) return {};
    // Index 0 is the runtime EXE's, and after the move the home clone's index holds that EXE's TLS.
    bool taken = loaded.index == 0 || loaded.index == g_index0.home.index;
    for (size_t i = 0; i < kMaxMappedTlsModules; ++i) taken |= i != slot && g_tls_hosts[i].index == loaded.index;
    if (taken) {
        FreeLibrary(loaded.module);
        DeleteFileW(clone_path.c_str());
        return {};
    }
    if (!g_tls_clones) {
        g_tls_clones = new std::vector<TlsCloneRecord>;
        std::atexit(&cleanup_tls_clones_at_exit);
    }
    g_tls_clones->push_back({loaded.module, clone_path});
    g_tls_hosts[slot] = loaded;
    return g_tls_hosts[slot];
}

struct ThreadTlsSlot {
    Module* module = nullptr;
    uint64_t generation = 0;
    void* saved = nullptr;
    void* block = nullptr;
    uint32_t index = kNoTlsIndex;
    void** ga_view = nullptr;           // the thread's slot-0 view for a /GA EXE (Module::tls_ga_slot)
    uint32_t ga_slot = kNoTlsIndex;
};

// Per-thread installation state for each reserved TLS slot.
thread_local std::array<ThreadTlsSlot, kTlsSlotCount> g_thread_tls_slots{};

// Orders the TLS owners' publication, their `initialised` flags and their retirement against the thread events. Shared
// in on_host_thread_event, exclusive only for those short writes (no allocation and no call that takes the Windows
// loader lock, which the events run under).
SRWLOCK g_tls_lock = SRWLOCK_INIT;
struct SharedTlsLock {
    SharedTlsLock() { AcquireSRWLockShared(&g_tls_lock); }
    ~SharedTlsLock() { ReleaseSRWLockShared(&g_tls_lock); }
    SharedTlsLock(const SharedTlsLock&) = delete;
    SharedTlsLock& operator=(const SharedTlsLock&) = delete;
};
struct ExclusiveTlsLock {
    ExclusiveTlsLock() { AcquireSRWLockExclusive(&g_tls_lock); }
    ~ExclusiveTlsLock() { ReleaseSRWLockExclusive(&g_tls_lock); }
    ExclusiveTlsLock(const ExclusiveTlsLock&) = delete;
    ExclusiveTlsLock& operator=(const ExclusiveTlsLock&) = delete;
};

// gta6rt_tls_host.dll's exports that the home clone's TLS directory is pointed at (tls_host.cpp): a zero array in its
// .rdata and that array's size.
constexpr char kHomeTemplateExport[] = "gta6rt_tls_home_template";
constexpr char kHomeTemplateSizeExport[] = "gta6rt_tls_home_template_size";

// File offset of `len` bytes at `rva` in an image file held in memory, or SIZE_MAX when they are not all inside one
// section's raw data.
size_t file_offset(const std::vector<uint8_t>& file, const IMAGE_NT_HEADERS64* nt, uint64_t rva, uint64_t len) {
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (reinterpret_cast<const uint8_t*>(sec + 1) > file.data() + file.size()) break;
        if (rva < sec->VirtualAddress || rva + len > static_cast<uint64_t>(sec->VirtualAddress) + sec->SizeOfRawData)
            continue;
        const uint64_t off = sec->PointerToRawData + (rva - sec->VirtualAddress);
        return off + len <= file.size() ? static_cast<size_t>(off) : SIZE_MAX;
    }
    return SIZE_MAX;
}

// The RVA of a named export in an image file held in memory, 0 when it has none.
uint32_t file_export(const std::vector<uint8_t>& file, const IMAGE_NT_HEADERS64* nt, const char* wanted) {
    const auto dir = directory(nt, IMAGE_DIRECTORY_ENTRY_EXPORT);
    const size_t ed_off = file_offset(file, nt, dir.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY));
    if (!dir.Size || ed_off == SIZE_MAX) return 0;
    const auto* ed = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(file.data() + ed_off);
    const size_t names = file_offset(file, nt, ed->AddressOfNames, uint64_t{4} * ed->NumberOfNames);
    const size_t ords = file_offset(file, nt, ed->AddressOfNameOrdinals, uint64_t{2} * ed->NumberOfNames);
    const size_t funcs = file_offset(file, nt, ed->AddressOfFunctions, uint64_t{4} * ed->NumberOfFunctions);
    if (names == SIZE_MAX || ords == SIZE_MAX || funcs == SIZE_MAX) return 0;
    const size_t wanted_len = std::strlen(wanted) + 1;
    for (uint32_t i = 0; i < ed->NumberOfNames; ++i) {
        uint32_t name_rva, func_rva;
        uint16_t ord;
        std::memcpy(&name_rva, file.data() + names + 4 * i, 4);
        std::memcpy(&ord, file.data() + ords + 2 * i, 2);
        const size_t name = file_offset(file, nt, name_rva, wanted_len);
        if (name == SIZE_MAX || std::memcmp(file.data() + name, wanted, wanted_len) != 0) continue;
        if (ord >= ed->NumberOfFunctions) return 0;
        std::memcpy(&func_rva, file.data() + funcs + 4 * ord, 4);
        return func_rva;
    }
    return 0;
}

// Turns gta6rt_tls_host.dll's bytes into the home clone: its TLS directory now covers gta6rt_tls_home_template, which
// starts with a copy of the owner's template (taken from the mapped owner, so its pointers are already relocated; the
// array itself has no relocations), and takes the owner's alignment. The OS then gives every thread, at the home
// index, a block that is a fresh copy of the owner's TLS. The zero fill goes into the covered range too:
// LdrpAllocateTls allocates and copies End - Start only (Windows 11 26300) and ignores SizeOfZeroFill. The directory's
// Start and End have base relocations, so they follow the clone wherever it loads. Empty on success, else why not.
std::string make_home_clone_image(std::vector<uint8_t>& file, const IMAGE_TLS_DIRECTORY64& owner) {
    if (file.size() < sizeof(IMAGE_DOS_HEADER)) return "gta6rt_tls_host.dll is truncated";
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(file.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
        static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > file.size())
        return "gta6rt_tls_host.dll has no PE header";
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return "gta6rt_tls_host.dll is not a 64-bit image";
    const uint32_t array_rva = file_export(file, nt, kHomeTemplateExport);
    const uint32_t size_rva = file_export(file, nt, kHomeTemplateSizeExport);
    const size_t size_off = file_offset(file, nt, size_rva, sizeof(uint32_t));
    if (!array_rva || !size_rva || size_off == SIZE_MAX)
        return "gta6rt_tls_host.dll has no gta6rt_tls_home_template";
    uint32_t capacity;
    std::memcpy(&capacity, file.data() + size_off, sizeof(capacity));
    const uint64_t raw = owner.EndAddressOfRawData - owner.StartAddressOfRawData;
    const uint64_t total = raw + owner.SizeOfZeroFill;
    if (owner.EndAddressOfRawData < owner.StartAddressOfRawData || total > capacity)
        return "the owner's TLS (" + std::to_string(total) + " bytes) does not fit gta6rt_tls_home_template (" +
               std::to_string(capacity) + " bytes)";
    const size_t array_off = file_offset(file, nt, array_rva, capacity);
    const auto tls_dir = directory(nt, IMAGE_DIRECTORY_ENTRY_TLS);
    const size_t tls_off = file_offset(file, nt, tls_dir.VirtualAddress, sizeof(IMAGE_TLS_DIRECTORY64));
    if (array_off == SIZE_MAX || !tls_dir.Size || tls_off == SIZE_MAX)
        return "gta6rt_tls_host.dll's TLS directory or home template lies outside its file";
    std::memcpy(file.data() + array_off, reinterpret_cast<const void*>(owner.StartAddressOfRawData), raw);
    std::memset(file.data() + array_off + raw, 0, capacity - raw);
    auto* tls = reinterpret_cast<IMAGE_TLS_DIRECTORY64*>(file.data() + tls_off);
    tls->StartAddressOfRawData = nt->OptionalHeader.ImageBase + array_rva;
    tls->EndAddressOfRawData = tls->StartAddressOfRawData + total;
    tls->SizeOfZeroFill = 0;
    tls->Characteristics =
        (tls->Characteristics & ~IMAGE_SCN_ALIGN_MASK) | (owner.Characteristics & IMAGE_SCN_ALIGN_MASK);
    return {};
}

std::string module_file_name(HMODULE module) {
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return "?";
    const wchar_t* name = path;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') name = p + 1;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return "?";
    std::string out(static_cast<size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, name, -1, out.data(), bytes, nullptr, nullptr);
    return out;
}

// The move on the calling thread, kept apart so that no frame of reserve_index0 holds a slot address across it. After
// it the owner's live block is at `home` and slot 0 holds the home clone's fresh copy, which nothing reads.
__declspec(noinline) void move_index0_on_this_thread(DWORD* owner_index, DWORD home) {
    void** slots = module_tls_slots();
    std::swap(slots[0], slots[home]);
    *owner_index = home;
}

// See reserve_tls_index0. Empty on success, else why not; nothing has changed then. g_tls_alloc_mutex held. It reads
// no thread_local data and runs no code that does, before or after the move.
// LEONIDA_TLS_INDEX0=0: keep the executable at a helper index instead of moving gta6rt's own TLS off index 0. The
// older path (a helper index plus the /GA patch of its gs:[58h] reads) is what the runtime used before the move
// existed, and it is the way back when the move stops the title's own CRT startup.
bool tls_index0_move_enabled() {
    char v[8] = {};
    size_t n = 0;
    return !(getenv_s(&n, v, "LEONIDA_TLS_INDEX0") == 0 && n == 2 && v[0] == '0');
}

std::string reserve_index0() {
    // Windows gives index 0 to the process executable when it has TLS; a DLL can only hold it in a process whose EXE
    // has none, and a DLL that is still loading or was loaded without its references resolved shows 0 too.
    const HMODULE exe = GetModuleHandleW(nullptr);
    auto* base = reinterpret_cast<uint8_t*>(exe);
    auto* nt = at<IMAGE_NT_HEADERS64>(base, reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto dir = directory(nt, IMAGE_DIRECTORY_ENTRY_TLS);
    if (!dir.Size || !dir.VirtualAddress) return "the process executable has no TLS, so index 0 is not its";
    const IMAGE_TLS_DIRECTORY64 owner = *at<IMAGE_TLS_DIRECTORY64>(base, dir.VirtualAddress);
    auto* owner_index = reinterpret_cast<DWORD*>(owner.AddressOfIndex);
    if (!owner_index) return "the process executable's TLS directory has no index";
    if (*owner_index != 0) return "the process executable's _tls_index is " + std::to_string(*owner_index) + ", not 0";
    if (!module_tls_slots()) return "this thread has no TLS array";
    if (!ensure_primary_tls_host())
        return "gta6rt_tls_host.dll is missing, has no module TLS index or lacks gta6rt_tls_host_set_thread_event";

    std::vector<uint8_t> image;
    try {
        image = read_file(tls_host_base_path());
    } catch (const std::exception& e) {
        return e.what();
    }
    if (std::string why = make_home_clone_image(image, owner); !why.empty()) return why;
    std::filesystem::path path;
    const TlsHostModule home = load_tls_host_clone(L"home", path, &image);
    if (home.index == TLS_OUT_OF_INDEXES) return "the home clone of gta6rt_tls_host.dll cannot be loaded";
    auto drop = [&](std::string why) {
        FreeLibrary(home.module);  // safe until the move below: nothing lives at its index yet
        DeleteFileW(path.c_str());
        return why;
    };
    bool taken = home.index == 0 || home.index >= kMaxModuleTlsSlots;
    for (const auto& host : g_tls_hosts) taken |= host.index == home.index;
    if (taken) return drop("the home clone got an unusable module TLS index");
    // The OS must have taken the changed directory: the mapped clone's template is the owner's, at full size.
    auto* home_base = reinterpret_cast<uint8_t*>(home.module);
    auto* home_nt = at<IMAGE_NT_HEADERS64>(home_base, reinterpret_cast<IMAGE_DOS_HEADER*>(home_base)->e_lfanew);
    const auto* home_tls =
        at<IMAGE_TLS_DIRECTORY64>(home_base, directory(home_nt, IMAGE_DIRECTORY_ENTRY_TLS).VirtualAddress);
    const uint64_t raw = owner.EndAddressOfRawData - owner.StartAddressOfRawData;
    if (home_tls->EndAddressOfRawData - home_tls->StartAddressOfRawData != raw + owner.SizeOfZeroFill ||
        std::memcmp(reinterpret_cast<const void*>(home_tls->StartAddressOfRawData),
                    reinterpret_cast<const void*>(owner.StartAddressOfRawData), raw) != 0)
        return drop("the home clone's mapped TLS template is not the owner's");

    // Everything that can throw comes first: past the move this records the result and nothing more.
    const std::string name = module_file_name(exe);
    // _tls_index sits in .data; made writable first in case a linker merged it into a read-only section.
    DWORD old_protect = 0;
    if (!VirtualProtect(owner_index, sizeof(DWORD), PAGE_READWRITE, &old_protect))
        return drop("the owner's _tls_index cannot be written");
    move_index0_on_this_thread(owner_index, home.index);
    VirtualProtect(owner_index, sizeof(DWORD), old_protect, &old_protect);

    // Kept loaded for the process lifetime: FreeLibrary would make the OS free every thread's block at its index, the
    // owner's from now on. Not in g_tls_clones for that reason. Its file cannot be deleted while it is mapped
    // (DeleteFileW and a POSIX-semantics delete both fail with ERROR_ACCESS_DENIED on Windows 11), so it stays until
    // a later run's sweep.
    g_index0.home = home;
    std::snprintf(g_index0.owner, sizeof(g_index0.owner), "%s", name.c_str());
    g_index0.moved = true;
    return {};
}

// Puts the helper DLL's own block back in this thread's slot and frees the copy that replaced it. Uses only this
// thread's record, so it also works after the module that installed the copy is gone.
void release_thread_slot(size_t slot) {
    if (slot >= kTlsSlotCount) return;
    ThreadTlsSlot& entry = g_thread_tls_slots[slot];
    if (!entry.module && !entry.block) return;
    if (entry.ga_view) {
        if (TlsGetValue(entry.ga_slot) == entry.ga_view) TlsSetValue(entry.ga_slot, nullptr);
        VirtualFree(entry.ga_view, 0, MEM_RELEASE);
    }
    if (entry.block) {
        void** slots = module_tls_slots();
        if (slots && entry.index < kMaxModuleTlsSlots && slots[entry.index] == entry.block)
            slots[entry.index] = entry.saved;
        VirtualFree(entry.block, 0, MEM_RELEASE);
    }
    entry = ThreadTlsSlot{};
}

// A block load() put into a thread that was already running (install_on_existing_threads). Such a thread has no
// g_thread_tls_slots record of it: the record is here, keyed by thread id, and the thread's own DLL_THREAD_DETACH or
// the module's ~Loader takes the block out again.
struct ExistingThreadBlock {
    DWORD thread = 0;
    size_t slot = 0;
    Module* module = nullptr;
    uint64_t generation = 0;   // the module's tls_generation: with the pointer, what identifies the module
    uint32_t index = kNoTlsIndex;
    void*** vector = nullptr;  // that thread's TEB->ThreadLocalStoragePointer field; the array itself can be replaced
    void* saved = nullptr;     // what the block replaced
    void* block = nullptr;
    void** ga_cell = nullptr;  // that thread's TEB->TlsSlots[tls_ga_slot]: inline in the TEB, so it never moves
    void* ga_view = nullptr;   // the /GA view that cell points at (Module::tls_ga_slot)
};

// Immortal, like the thread events that use them. `ended` holds the ids of threads whose DLL_THREAD_DETACH has run:
// Windows frees such a thread's TLS array next, so nothing may be put into it any more.
struct ExistingThreads {
    SRWLOCK lock = SRWLOCK_INIT;  // a leaf: nothing is called while it is held but VirtualFree and thread queries
    std::vector<ExistingThreadBlock> blocks;
    std::vector<DWORD> ended;
};
ExistingThreads& existing_threads() {
    static auto* state = new ExistingThreads;
    return *state;
}
struct ExistingThreadsLock {
    ExistingThreadsLock() { AcquireSRWLockExclusive(&existing_threads().lock); }
    ~ExistingThreadsLock() { ReleaseSRWLockExclusive(&existing_threads().lock); }
    ExistingThreadsLock(const ExistingThreadsLock&) = delete;
    ExistingThreadsLock& operator=(const ExistingThreadsLock&) = delete;
};

bool has_existing_thread_block(DWORD thread, const Module& m) {
    ExistingThreadsLock lock;
    for (const auto& b : existing_threads().blocks)
        if (b.thread == thread && b.module == &m && b.generation == m.tls_generation) return true;
    return false;
}

// The /GA view a thread's TlsSlots[Module::tls_ga_slot] points at: [0] and [tls_index] are this thread's block.
// Null when it cannot be allocated.
void** make_ga_view(const Module& m, void* block) {
    auto* view = static_cast<void**>(
        VirtualAlloc(nullptr, kMaxModuleTlsSlots * sizeof(void*), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!view) return nullptr;
    view[0] = block;
    view[m.tls_index] = block;
    return view;
}

void install_thread_block(Module& m) {
    if (m.tls_slot >= kTlsSlotCount) return;
    const size_t slot = m.tls_slot;
    ThreadTlsSlot& entry = g_thread_tls_slots[slot];
    if (entry.module == &m && entry.generation == m.tls_generation) return;
    if (has_existing_thread_block(GetCurrentThreadId(), m)) return;  // load() already gave this thread its copy
    if (entry.module) {
        // A copy left by a module whose Loader is gone (or from an earlier generation) is released; a live owner's is a conflict.
        if (entry.module == g_tls_slot_owners[slot].load() &&
            entry.generation == g_tls_slot_generations[slot].load()) {
            throw std::runtime_error("another manually mapped module's TLS is already installed in this thread slot");
        }
        release_thread_slot(slot);
    }

    const uint64_t total = static_cast<uint64_t>(m.tls_raw_size) + m.tls_zero_fill;
    if (!total) {
        // Callback-only TLS module (no data block): still record attachment in this slot so DLL_THREAD_DETACH runs.
        entry.module = &m;
        entry.generation = m.tls_generation;
        return;
    }
    if (m.tls_index == kNoTlsIndex) return;
    void** slots = module_tls_slots();
    if (!slots || m.tls_index >= kMaxModuleTlsSlots)
        throw std::runtime_error(m.name + ": cannot install TLS in this thread");

    void* block = VirtualAlloc(nullptr, static_cast<SIZE_T>(total), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!block) throw std::runtime_error(m.name + ": TLS block allocation failed");
    if (m.tls_raw_size) std::memcpy(block, m.tls_template, m.tls_raw_size);
    if (m.tls_zero_fill)
        std::memset(static_cast<uint8_t*>(block) + m.tls_raw_size, 0, m.tls_zero_fill);

    void** view = nullptr;
    if (m.tls_ga_slot != kNoTlsIndex) {
        // The /GA view: [0] and [tls_index] are this thread's block, the rest stays null (the image reads no other).
        view = make_ga_view(m, block);
        if (!view) {
            VirtualFree(block, 0, MEM_RELEASE);
            throw std::runtime_error(m.name + ": TLS view allocation failed");
        }
        TlsSetValue(m.tls_ga_slot, view);
    }

    void* saved = slots[m.tls_index];
    slots[m.tls_index] = block;
    entry.module = &m;
    entry.generation = m.tls_generation;
    entry.saved = saved;
    entry.block = block;
    entry.index = m.tls_index;
    entry.ga_view = view;
    entry.ga_slot = m.tls_ga_slot;
}

// Rewrites a /GA EXE's slot-0 TLS reads (Module::tls_ga_slot): every `mov r64, gs:[58h]` (65 REX.W 8B /r with
// mod=00, r/m=100, SIB=25, disp32=58h; REX 48 or 4C for r8-r15) in an executable section becomes the same
// instruction with disp32 = 1480h + 8 * slot, TEB->TlsSlots[slot]. Same length, so nothing else moves. The 9-byte
// form, with its segment prefix, SIB 25 and that displacement, is not something code or tables produce by accident.
// Must run before protect_sections() and before any of the image's code (its TLS callbacks included).
size_t patch_ga_tls_reads(Module& m, uint32_t slot) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(m.base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    const uint32_t disp = 0x1480u + 8u * slot;
    size_t sites = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uint32_t size = (std::min)(sec->Misc.VirtualSize, sec->SizeOfRawData);
        if (sec->VirtualAddress > m.size || size > m.size - sec->VirtualAddress || size < 9) continue;
        uint8_t* p = m.base + sec->VirtualAddress;
        for (uint32_t k = 0; k + 9 <= size; ++k) {
            if (p[k] != 0x65 || (p[k + 1] != 0x48 && p[k + 1] != 0x4C) || p[k + 2] != 0x8B) continue;
            const uint8_t modrm = p[k + 3];
            if ((modrm & 0xC7) != 0x04 || p[k + 4] != 0x25) continue;
            uint32_t d;
            std::memcpy(&d, p + k + 5, 4);
            if (d != 0x58) continue;
            std::memcpy(p + k + 5, &disp, 4);
            ++sites;
            k += 8;
        }
    }
    return sites;
}

bool ga_tls_patch_enabled() {
    char v[8] = {};
    size_t n = 0;
    return !(getenv_s(&n, v, "LEONIDA_TLS_GA_PATCH") == 0 && n == 2 && v[0] == '0');
}

void uninstall_thread_block(Module& m) {
    if (m.tls_slot >= kTlsSlotCount) return;
    const size_t slot = m.tls_slot;
    ThreadTlsSlot& entry = g_thread_tls_slots[slot];
    if (entry.module != &m || entry.generation != m.tls_generation) return;
    if (m.tls_block == entry.block) {
        m.tls_block = nullptr;
        m.tls_saved_block = nullptr;
    }
    release_thread_slot(slot);
}

// `trace`: record each callback in the startup trace. Only the Loader's own calls (process attach and detach) pass
// true; the thread events run under the Windows loader lock and stay out of the trace.
void run_callbacks(Module& m, uint32_t reason, bool trace = false) {
    auto* tls = tls_directory(m);
    if (!tls || !tls->AddressOfCallBacks) return;
    const uint64_t table_rva = rva_in(m, tls->AddressOfCallBacks, "the TLS callback table");
    // The callback table has no length field: it ends at a zero entry, so every step is checked against the image.
    for (uint64_t i = 0;; ++i) {
        const uint64_t entry_rva = table_rva + i * sizeof(uint64_t);
        require_range(m.name, entry_rva, sizeof(uint64_t), m.size, "a TLS callback table entry");
        const uint64_t fn = *at<uint64_t>(m.base, static_cast<uint32_t>(entry_rva));
        if (!fn) break;
        auto callback =
            reinterpret_cast<PIMAGE_TLS_CALLBACK>(m.base + rva_in(m, fn, "a TLS callback"));
        if (trace && startup_trace::enabled())
            startup_trace::event("tls_callback", "\"module\":" + startup_trace::quote(m.name) + ",\"at\":" +
                                                     startup_trace::quote(startup_trace::where(reinterpret_cast<uint64_t>(callback))) +
                                                     ",\"reason\":" + std::to_string(reason));
        callback(m.base, static_cast<DWORD>(reason), nullptr);
    }
}

// ---- Threads that existed before load() ---------------------------------------------------------------------------
//
// When Windows loads a DLL with static TLS itself, every running thread gets a block for it (and no DLL_THREAD_ATTACH).
// The helper DLL gets exactly that, so in a thread that was already running the reserved slot holds the helper's
// one-byte block, and mapped __declspec(thread) code there would read and write past it. Such threads are not rare:
// the workers of the process thread pool predate most loads, and the task queue runs a title's callbacks on them.
// load() therefore gives each of them a copy of the template too, through the thread's TEB, which is ordinary memory
// of this process.
//
// What makes that safe:
//  - The slot is the helper's, and nothing reads the helper's variable, so swapping the pointer under a running
//    thread disturbs nothing; the thread cannot be in mapped code of a module whose load() has not returned.
//  - The swap is made with g_tls_lock held exclusively, so no thread is inside its attach or detach event meanwhile.
//  - A thread whose detach event has run is skipped (`ended`), and so is one with no TLS array or no helper block:
//    Windows is about to free, or never built, what the block would go into.
//  - The block comes out again in that thread's own detach event, before Windows frees the array, or in ~Loader.
// What is left:
//  - A thread that ran its detach event before the helper DLL was ever loaded and has not left the process yet is
//    not in `ended`; if Windows leaves a stale array pointer in its TEB for those last instructions, the write goes
//    to freed heap memory.
//  - Windows replaces a thread's TLS array (a new one, copied from the old) when some thread loads a DLL with static
//    TLS. If that happens during the swap, the write can land in the old array; the thread then still has the
//    helper's one-byte block, and the first mapped __declspec(thread) access on it writes past that block: heap
//    corruption, with nothing to show for it. The swap therefore reads the array pointer again after writing and
//    repeats on the new array (kSwapAttempts times); if the block is not in the thread's current array at the end,
//    it is taken back and the miss is logged. A replacement after that last look is not seen.
//  - TerminateThread frees a thread's TEB with no detach event. Every read and write of another thread's TEB or
//    TLS array here is made right after the thread was seen running and inside __try, so a TEB that has just gone
//    is a skipped thread, not a fault in load() or ~Loader. If the memory was already given to something else, the
//    write is not caught.
using QueryThreadFn = LONG(NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);
struct ThreadBasicInfo {  // THREAD_BASIC_INFORMATION (winternl.h keeps it opaque), x64
    LONG exit_status;
    void* teb;
    void* process_id;
    void* thread_id;
    ULONG_PTR affinity;
    LONG priority;
    LONG base_priority;
};
static_assert(sizeof(ThreadBasicInfo) == 48);

QueryThreadFn query_thread_fn() {
    static const auto fn = reinterpret_cast<QueryThreadFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    return fn;
}

// The TEB's ThreadLocalStoragePointer field of a thread that is still running; null otherwise.
void*** running_thread_tls_vector(HANDLE thread) {
    ThreadBasicInfo info{};
    const QueryThreadFn query = query_thread_fn();
    if (!query || query(thread, 0 /* ThreadBasicInformation */, &info, sizeof info, nullptr) < 0) return nullptr;
    if (info.exit_status != STILL_ACTIVE || !info.teb) return nullptr;
    return reinterpret_cast<void***>(static_cast<uint8_t*>(info.teb) + 0x58);
}

// That thread's TEB->TlsSlots[slot] cell (0x1480 + 8 * slot): inline in the TEB, so it cannot be replaced while the
// thread runs. Null when the thread is gone.
void** running_thread_tls_cell(HANDLE thread, uint32_t slot) {
    ThreadBasicInfo info{};
    const QueryThreadFn query = query_thread_fn();
    if (!query || query(thread, 0 /* ThreadBasicInformation */, &info, sizeof info, nullptr) < 0) return nullptr;
    if (info.exit_status != STILL_ACTIVE || !info.teb) return nullptr;
    return reinterpret_cast<void**>(static_cast<uint8_t*>(info.teb) + 0x1480 + 8 * slot);
}

// Another thread's TLS array and one entry of it, read through its TEB. False when the memory is not there any more
// (the thread was terminated). No C++ objects in these three: they are __try frames.
bool read_thread_slot(void*** vector, uint32_t index, void**& slots, void*& value) noexcept {
    __try {
        slots = *vector;
        value = slots ? slots[index] : nullptr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        slots = nullptr;
        value = nullptr;
        return false;
    }
}

bool write_thread_slot(void** slots, uint32_t index, void* value) noexcept {
    __try {
        slots[index] = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Puts `saved` back where `block` is, if it is still there.
void restore_thread_slot(void*** vector, uint32_t index, void* block, void* saved) noexcept {
    __try {
        void** slots = *vector;
        if (slots && slots[index] == block) slots[index] = saved;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Writes one TlsSlots cell of another thread; false when the thread's TEB is gone already.
bool write_tls_cell(void** cell, void* value) noexcept {
    __try {
        *cell = value;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Empties a TlsSlots cell that still holds `view` (a thread that ended may hold another value).
void restore_tls_cell(void** cell, void* view) noexcept {
    __try {
        if (*cell == view) *cell = nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Takes a block out of its slot and frees it. The existing-threads lock is held; b.vector is null when the thread
// is gone.
void remove_existing_block(const ExistingThreadBlock& b) {
    if (b.vector && b.index < kMaxModuleTlsSlots) restore_thread_slot(b.vector, b.index, b.block, b.saved);
    if (b.ga_view) {
        if (b.ga_cell) restore_tls_cell(b.ga_cell, b.ga_view);
        VirtualFree(b.ga_view, 0, MEM_RELEASE);
    }
    VirtualFree(b.block, 0, MEM_RELEASE);
}

constexpr int kSwapAttempts = 4;
constexpr size_t kMaxEndedThreads = 1024;

// load(): every other running thread gets its own copy of m's template. Not fatal where it cannot be done.
void install_on_existing_threads(Module& m) {
    const uint64_t total = static_cast<uint64_t>(m.tls_raw_size) + m.tls_zero_fill;
    if (!total || m.tls_index >= kMaxModuleTlsSlots || m.tls_slot >= kTlsSlotCount) return;
    // Stays ahead of the TLS lock: the first call resolves the function with GetModuleHandle and GetProcAddress, which
    // take the Windows loader lock, and a thread inside its attach event holds that lock while it waits for ours.
    if (!query_thread_fn()) return;

    // Outside the TLS lock: none of this may wait for a thread that is inside its attach event.
    // The handles are closed on every way out, also when a vector below cannot grow.
    struct Candidate {
        DWORD id = 0;
        HANDLE handle = nullptr;
    };
    struct Candidates {
        std::vector<Candidate> list;
        HANDLE snapshot = INVALID_HANDLE_VALUE;
        ~Candidates() {
            if (snapshot != INVALID_HANDLE_VALUE) CloseHandle(snapshot);
            for (const Candidate& c : list)
                if (c.handle) CloseHandle(c.handle);
        }
    } found;
    std::vector<Candidate>& candidates = found.list;
    // TH32CS_SNAPTHREAD lists the threads of every process; the loop keeps this one's.
    found.snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (found.snapshot == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof entry;
    const DWORD process = GetCurrentProcessId(), self = GetCurrentThreadId();
    for (BOOL more = Thread32First(found.snapshot, &entry); more; more = Thread32Next(found.snapshot, &entry)) {
        if (entry.th32OwnerProcessID != process || entry.th32ThreadID == self) continue;
        candidates.push_back({entry.th32ThreadID, nullptr});  // the slot first, so the handle always has an owner
        candidates.back().handle = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
    }

    {
        ExclusiveTlsLock tls_lock;
        ExistingThreadsLock lock;
        ExistingThreads& state = existing_threads();
        for (const Candidate& c : candidates) {
            if (!c.handle) continue;
            if (std::find(state.ended.begin(), state.ended.end(), c.id) != state.ended.end()) continue;
            void*** vector = running_thread_tls_vector(c.handle);
            void** slots = nullptr;
            void* saved = nullptr;
            if (!vector || !read_thread_slot(vector, m.tls_index, slots, saved) || !slots || !saved) continue;
            // A copy an earlier module left in this slot of this thread goes first; what it replaced is kept.
            bool present = false;
            for (auto it = state.blocks.begin(); it != state.blocks.end();) {
                if (it->thread != c.id || it->slot != m.tls_slot) {
                    ++it;
                } else if (it->module == &m && it->generation == m.tls_generation) {
                    present = true;
                    break;
                } else {
                    if (saved == it->block) saved = it->saved;
                    it->vector = vector;
                    remove_existing_block(*it);
                    it = state.blocks.erase(it);
                }
            }
            if (present) continue;
            void* block = VirtualAlloc(nullptr, static_cast<SIZE_T>(total), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (!block) continue;
            if (m.tls_raw_size) std::memcpy(block, m.tls_template, m.tls_raw_size);

            // Write, then look at the thread's array pointer again: if Windows has replaced the array meanwhile, the
            // write went into the old one, and it is made again in the new one.
            bool installed = false;
            for (int attempt = 0; attempt < kSwapAttempts && !installed; ++attempt) {
                if (!running_thread_tls_vector(c.handle) || !write_thread_slot(slots, m.tls_index, block)) break;
                void** now = nullptr;
                void* value = nullptr;
                if (!read_thread_slot(vector, m.tls_index, now, value) || !now) break;
                installed = now == slots && value == block;
                slots = now;
            }
            if (!installed) {
                restore_thread_slot(vector, m.tls_index, block, saved);
                VirtualFree(block, 0, MEM_RELEASE);
                static std::atomic<bool> logged{false};
                if (!logged.exchange(true))
                    std::fprintf(stderr, "[rt] loader: %s: a running thread did not take its TLS block (it ended, or "
                                         "its TLS array kept changing); mapped thread-local data is not valid on it\n",
                                 m.name.c_str());
                continue;
            }
            void** ga_cell = nullptr;
            void* ga_view = nullptr;
            if (m.tls_ga_slot != kNoTlsIndex) {
                // The /GA view this thread's rewritten index-0 TLS reads go through, as install_thread_block gives
                // a thread started after load(). Without it those reads dereference null on this thread.
                ga_cell = running_thread_tls_cell(c.handle, m.tls_ga_slot);
                if (ga_cell) ga_view = make_ga_view(m, block);
                if (!ga_view || !write_tls_cell(ga_cell, ga_view)) {
                    if (ga_view) VirtualFree(ga_view, 0, MEM_RELEASE);
                    ga_cell = nullptr;
                    ga_view = nullptr;
                    static std::atomic<bool> logged_view{false};
                    if (!logged_view.exchange(true))
                        std::fprintf(stderr, "[rt] loader: %s: a running thread did not take its /GA TLS view; "
                                             "index-0 TLS reads in it have no block\n",
                                     m.name.c_str());
                }
            }
            state.blocks.push_back(
                {c.id, m.tls_slot, &m, m.tls_generation, m.tls_index, vector, saved, block, ga_cell, ga_view});
        }
    }
}

// ~Loader, with g_tls_lock held exclusively: the blocks load() put into other threads come out. A thread that has
// ended without a detach event (TerminateThread) has no TEB to restore.
void remove_from_existing_threads(Module& m) {
    ExistingThreadsLock lock;
    ExistingThreads& state = existing_threads();
    for (auto it = state.blocks.begin(); it != state.blocks.end();) {
        if (it->module != &m || it->generation != m.tls_generation) {
            ++it;
            continue;
        }
        ExistingThreadBlock b = *it;
        it = state.blocks.erase(it);
        const HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, b.thread);
        if (!h || running_thread_tls_vector(h) != b.vector) b.vector = nullptr;
        if (h) CloseHandle(h);
        if (!b.vector) b.ga_cell = nullptr;  // the TEB went with the thread
        remove_existing_block(b);
    }
}

// DLL_THREAD_ATTACH: a record under this thread's id is from a thread that ended without a detach event.
void forget_ended_thread_id() {
    const DWORD self = GetCurrentThreadId();
    ExistingThreadsLock lock;
    ExistingThreads& state = existing_threads();
    state.ended.erase(std::remove(state.ended.begin(), state.ended.end(), self), state.ended.end());
    for (auto it = state.blocks.begin(); it != state.blocks.end();) {
        if (it->thread != self) {
            ++it;
            continue;
        }
        VirtualFree(it->block, 0, MEM_RELEASE);
        VirtualFree(it->ga_view, 0, MEM_RELEASE);
        it = state.blocks.erase(it);
    }
}

// DLL_THREAD_DETACH, before this thread's own records are released: the blocks load() gave it come out, newest first,
// after the module's TLS callbacks have seen the event (Windows sends DLL_THREAD_DETACH to a DLL for a thread that
// was running before it was loaded, too).
void release_existing_blocks_of_this_thread() {
    const DWORD self = GetCurrentThreadId();
    std::vector<ExistingThreadBlock> mine;
    {
        ExistingThreadsLock lock;
        for (const auto& b : existing_threads().blocks)
            if (b.thread == self) mine.push_back(b);
    }
    for (auto it = mine.rbegin(); it != mine.rend(); ++it) {
        Module* m = it->module;
        if (g_tls_slot_owners[it->slot].load() != m || g_tls_slot_generations[it->slot].load() != it->generation ||
            !m->initialised)
            continue;
        try {
            run_callbacks(*m, DLL_THREAD_DETACH);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[rt] loader: thread detach: %s\n", e.what());
        }
    }
    ExistingThreadsLock lock;
    ExistingThreads& state = existing_threads();
    for (auto it = state.blocks.end(); it != state.blocks.begin();) {
        --it;
        if (it->thread != self) continue;
        remove_existing_block(*it);
        it = state.blocks.erase(it);
    }
    // Bounded: an id matters only until its thread has left the process, moments after this event, so the oldest
    // can go. (An id also leaves the list when a new thread gets it.)
    if (state.ended.size() >= kMaxEndedThreads) state.ended.erase(state.ended.begin(), state.ended.begin() + kMaxEndedThreads / 2);
    state.ended.push_back(self);
}

// Runs in gta6rt_tls_host.dll's DllMain for each thread the process starts or ends once the helper is loaded, under
// the Windows loader lock, on that thread. A new thread gets its own copy of each active TLS owner's template; an
// ending one gives each helper's block back before the Windows loader frees it. TLS callbacks get the thread events
// only after the module's DLL_PROCESS_ATTACH (initialise()), as the Windows loader orders them. Windows allocates
// every module's block before the first DllMain of a new thread, and runs an EXE's TLS callbacks after every DllMain,
// for attach and for detach (measured on Windows 11 26300). So attach installs all copies first and then runs the
// callbacks, the DLL slots in order and the executable's last; detach runs the DLL slots in reverse and then the
// executable's slot, each releasing its copy after its callbacks.
void __cdecl on_host_thread_event(DWORD reason) {
    SharedTlsLock lock;
    // The try/catch is per slot: a throw from one module's install or callbacks must not take the thread events
    // away from the other slots. Nothing may leave DllMain.
    if (reason == DLL_THREAD_ATTACH) {
        try {
            forget_ended_thread_id();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[rt] loader: thread attach: %s\n", e.what());
        }
        std::array<Module*, kTlsSlotCount> installed{};
        for (size_t slot = 0; slot < kTlsSlotCount; ++slot) {
            Module* m = g_tls_slot_owners[slot].load();
            if (!m) continue;
            try {
                install_thread_block(*m);
                installed[slot] = m;
            } catch (const std::exception& e) {
                // The thread runs on without this module's TLS (and its callbacks); later slots still get theirs.
                std::fprintf(stderr, "[rt] loader: thread attach, slot %zu (%s): %s\n", slot, m->name.c_str(),
                             e.what());
            }
        }
        for (size_t slot = 0; slot < kTlsSlotCount; ++slot) {
            Module* m = installed[slot];
            if (!m || !m->initialised) continue;
            try {
                run_callbacks(*m, DLL_THREAD_ATTACH);
            } catch (const std::exception& e) {
                std::fprintf(stderr, "[rt] loader: thread attach, slot %zu (%s): %s\n", slot, m->name.c_str(),
                             e.what());
            }
        }
    } else if (reason == DLL_THREAD_DETACH) {
        // The blocks load() gave this thread while it was already running come out first (they can sit on top of
        // an older module's block in the same slot).
        try {
            release_existing_blocks_of_this_thread();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[rt] loader: thread detach: %s\n", e.what());
        }
        for (size_t step = 0; step < kTlsSlotCount; ++step) {
            const size_t i = step < kMaxMappedTlsModules ? kMaxMappedTlsModules - 1 - step : kMainImageSlot;
            Module* m = g_tls_slot_owners[i].load();
            if (m && g_thread_tls_slots[i].module == m &&
                g_thread_tls_slots[i].generation == m->tls_generation) {
                try {
                    if (m->initialised) run_callbacks(*m, DLL_THREAD_DETACH);
                } catch (const std::exception& e) {
                    std::fprintf(stderr, "[rt] loader: thread detach, slot %zu (%s): %s\n", i, m->name.c_str(),
                                 e.what());
                }
                // Released even when the callbacks threw: a leaked block would be handed to the OS loader as the
                // helper's own TLS block and freed with it at thread exit.
                uninstall_thread_block(*m);
            } else {
                release_thread_slot(i);  // installed by a module whose Loader is gone (or nothing to do)
            }
        }
    }
}

// The Xbox builds of the MSVC runtime import the same CRT the desktop does, so the host's own copies are used.
bool prefer_host_copy(const std::string& dll) {
    // The second row is the Debug builds of the same DLLs. A Debug build of a stand-in image (tiny_game) links them,
    // and without them here its C++ exceptions were not redirected below: a throw caught inside the image ended the
    // run with 0xE06D7363 (run_tiny_game_eh in a Debug build).
    static const char* names[] = {"vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll", "msvcp140_1.dll",
                                  "msvcp140_2.dll", "msvcp140_atomic_wait.dll", "msvcp140_codecvt_ids.dll",
                                  "concrt140.dll", "vccorlib140.dll", "vcomp140.dll",
                                  "vcruntime140d.dll", "vcruntime140_1d.dll", "msvcp140d.dll", "msvcp140_1d.dll",
                                  "msvcp140_2d.dll", "msvcp140d_atomic_wait.dll", "msvcp140d_codecvt_ids.dll",
                                  "concrt140d.dll", "vccorlib140d.dll", "vcomp140d.dll"};
    for (auto* n : names)
        if (dll == n) return true;
    return false;
}

// MSVC C++ exceptions in a mapped image. _CxxThrowException (vcruntime140.dll) puts the throwing image's base in the
// exception record, from RtlPcToFileHeader(pThrowInfo); the frame handler adds the ThrowInfo's RVAs (catchable types,
// copy constructor, destructor) to it. RtlPcToFileHeader only knows images the Windows loader mapped, so for ours the
// base is 0 and the exception can't be matched to a catch. The host copies the loader binds get their import of it
// redirected: the Windows answer first, then the modules a live Loader has mapped (shims::mapped_image_base).
using PcToFileHeaderFn = PVOID(NTAPI*)(PVOID, PVOID*);
std::atomic<PcToFileHeaderFn> g_host_pc_to_file_header{nullptr};

PVOID NTAPI pc_to_file_header(PVOID pc, PVOID* base) {
    if (PVOID found = g_host_pc_to_file_header.load()(pc, base)) return found;
    uint64_t mapped = 0;
    if (!shims::mapped_image_base(reinterpret_cast<uint64_t>(pc), mapped)) {
        *base = nullptr;
        return nullptr;
    }
    *base = reinterpret_cast<PVOID>(mapped);
    return *base;
}

// Points every import of RtlPcToFileHeader by name in a host module at pc_to_file_header, once per module. The
// module is a system DLL the Windows loader has already validated and bound, so its tables are read unchecked.
// The redirect stays for the process lifetime; with no live Loader it answers exactly as Windows does.
void redirect_pc_to_file_header(HMODULE host) {
    static std::mutex mutex;
    static std::vector<HMODULE> done;
    std::lock_guard lock(mutex);
    if (std::find(done.begin(), done.end(), host) != done.end()) return;
    done.push_back(host);

    auto* base = reinterpret_cast<uint8_t*>(host);
    auto* nt = at<IMAGE_NT_HEADERS64>(base, reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.Size || !dir.VirtualAddress) return;
    for (auto* d = at<IMAGE_IMPORT_DESCRIPTOR>(base, dir.VirtualAddress); d->Name; ++d) {
        if (!d->OriginalFirstThunk) continue;  // no name table to match against
        auto* names = at<IMAGE_THUNK_DATA64>(base, d->OriginalFirstThunk);
        auto* iat = at<IMAGE_THUNK_DATA64>(base, d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto* by_name = at<IMAGE_IMPORT_BY_NAME>(base, static_cast<uint32_t>(names->u1.AddressOfData));
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), "RtlPcToFileHeader") != 0) continue;
            auto* slot = reinterpret_cast<void**>(&iat->u1.Function);
            if (*slot == reinterpret_cast<void*>(&pc_to_file_header)) continue;
            PcToFileHeaderFn expected = nullptr;  // the first bound address is the Windows function
            g_host_pc_to_file_header.compare_exchange_strong(expected, reinterpret_cast<PcToFileHeaderFn>(*slot));
            DWORD old = 0;
            if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) continue;
            *slot = reinterpret_cast<void*>(&pc_to_file_header);
            VirtualProtect(slot, sizeof(void*), old, &old);
        }
    }
}

// The module a host import actually resolved into. GetProcAddress follows forwarders, so this can be another DLL
// than the one imported (on Windows vcruntime140.dll implements _CxxThrowException itself); that module's
// RtlPcToFileHeader import is the one the throw calls.
void redirect_pc_to_file_header_at(const void* resolved) {
    HMODULE owner = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCWSTR>(resolved), &owner) && owner)
        redirect_pc_to_file_header(owner);
}

}  // namespace

// Declared in pe_loader.h; the body needs the internal clone registry above.
void cleanup_tls_clones() { cleanup_tls_clones_internal(); }

bool reserve_tls_index0_enabled() { return tls_index0_move_enabled(); }

bool reserve_tls_index0(std::string* why) {
    std::lock_guard alloc_lock(g_tls_alloc_mutex);
    if (!tls_index0_move_enabled()) {
        // LEONIDA_TLS_INDEX0=0: the executable keeps a helper module TLS index, as before the move existed.
        if (why) *why = "LEONIDA_TLS_INDEX0=0 keeps the executable at a helper index";
        return false;
    }
    static bool attempted = false;  // once: a later attempt could meet threads that already run this EXE's code
    if (!attempted) {
        attempted = true;
        // A throw (std::filesystem::temp_directory_path when GetTempPathW fails, an allocation) is a failed
        // reservation like any other: gta6rt calls this before main, where an exception would end it with no message.
        try {
            const std::string reason = reserve_index0();
            std::snprintf(g_index0.why, sizeof(g_index0.why), "%s", reason.c_str());
        } catch (const std::exception& e) {
            std::snprintf(g_index0.why, sizeof(g_index0.why), "%s", e.what());
        } catch (...) {
            std::snprintf(g_index0.why, sizeof(g_index0.why), "an unknown error while reserving it");
        }
    }
    if (why) *why = g_index0.moved ? std::string() : std::string(g_index0.why);
    return g_index0.moved;
}

namespace {
bool lookup_in_loader(void* ctx, uint64_t address, std::string& module, uint32_t& rva) {
    return static_cast<Loader*>(ctx)->find_address(address, module, rva);
}

void* bundled_find_loaded(void* ctx, const std::string& dll_lower) {
    return static_cast<Loader*>(ctx)->find_loaded_module(dll_lower);
}

void* bundled_load(void* ctx, const std::string& dll_lower, uint32_t flags, bool& handled) {
    return static_cast<Loader*>(ctx)->load_runtime_module(dll_lower, flags, handled);
}

void* bundled_get_proc(void* ctx, void* module, const std::string& sym, bool& owns_handle) {
    return static_cast<Loader*>(ctx)->resolve_module_export(module, sym, owns_handle);
}

bool bundled_owns_handle(void* ctx, void* module) {
    return static_cast<Loader*>(ctx)->owns_module_handle(module);
}

void* bundled_find_loaded_w(void* ctx, const std::wstring& basename) {
    return static_cast<Loader*>(ctx)->find_loaded_module_w(basename);
}

void* bundled_load_w(void* ctx, const std::wstring& basename, uint32_t flags, bool& handled) {
    return static_cast<Loader*>(ctx)->load_runtime_module_w(basename, flags, handled);
}

const shims::BundledModuleOps kBundledOps{
    &bundled_find_loaded,
    &bundled_load,
    &bundled_get_proc,
    &bundled_owns_handle,
    &bundled_find_loaded_w,
    &bundled_load_w,
};

// Marks what a run-time load maps as a dynamic load for as long as it is in scope (the caller holds the mutex).
struct RuntimeLoadScope {
    explicit RuntimeLoadScope(int& depth) : depth_(depth) { ++depth_; }
    ~RuntimeLoadScope() { --depth_; }
    RuntimeLoadScope(const RuntimeLoadScope&) = delete;
    RuntimeLoadScope& operator=(const RuntimeLoadScope&) = delete;
    int& depth_;
};
}  // namespace

Loader::Loader(std::filesystem::path game_dir) : game_dir_(std::move(game_dir)) {
    std::vector<wchar_t> compat(32768);
    const DWORD n = GetEnvironmentVariableW(L"LEONIDA_COMPAT_DIR", compat.data(), static_cast<DWORD>(compat.size()));
    if (n > 0 && n < compat.size()) compat_dir_ = std::filesystem::path(compat.data());
    shims::add_module_lookup(this, &lookup_in_loader);
    shims::add_bundled_loader(this, &kBundledOps);
}

Loader::~Loader() {
    // Importers must release their resources before their dependencies. Keep every mapping, unwind table and TLS
    // block alive throughout the notifications: a detach handler can still call an imported function or free an
    // FLS slot whose destructor lives in another mapped image.
    for (auto it = init_order_.rbegin(); it != init_order_.rend(); ++it) {
        Module& m = **it;
        if (!m.initialised) continue;  // a map-only load never opts in to running image code
        set_initialised(m, false);  // under the TLS lock: thread events stop running its callbacks
        try {
            run_tls_callbacks(m, DLL_PROCESS_DETACH);
        } catch (const std::exception& e) {
            // A module may have changed its callback table since attach. A destructor cannot propagate the validation
            // error; report it and still let DllMain release resources before the image is freed.
            std::fprintf(stderr, "[rt] TLS process detach failed for %s: %s\n", m.name.c_str(), e.what());
        }
        auto* nt = at<IMAGE_NT_HEADERS64>(m.base, reinterpret_cast<IMAGE_DOS_HEADER*>(m.base)->e_lfanew);
        if ((nt->FileHeader.Characteristics & IMAGE_FILE_DLL) && m.entry_rva) {
            auto entry = reinterpret_cast<BOOL(NTAPI*)(void*, DWORD, void*)>(m.base + m.entry_rva);
            entry(m.base, DLL_PROCESS_DETACH, nullptr);  // explicit unload; the return value is ignored
        }
    }
    shims::remove_xmem_allocation_hooks(this);
    shims::remove_bundled_loader(this);
    shims::remove_module_lookup(this);
    for (auto* m : order_) {
        // TLS state must go away before the image and its TLS block are unmapped.
        if (m->tls_index != kNoTlsIndex && m->tls_slot < kTlsSlotCount) {
            // Retired first, under the lock: no thread event installs it or runs its callbacks after this, and a
            // thread that still holds a copy frees it when it ends (on_host_thread_event).
            {
                ExclusiveTlsLock lock;
                Module* owner = m;
                if (g_tls_slot_owners[m->tls_slot].compare_exchange_strong(owner, nullptr))
                    g_tls_slot_generations[m->tls_slot].store(0);
                remove_from_existing_threads(*m);
            }
            uninstall_tls_for_thread(*m);
        }
        // Before the image is freed: a stale table would let a later stack walk read unmapped memory.
        if (m->function_table) RtlDeleteFunctionTable(static_cast<PRUNTIME_FUNCTION>(m->function_table));
        if (m->tls_block) VirtualFree(m->tls_block, 0, MEM_RELEASE);
        if (m->base) VirtualFree(m->base, 0, MEM_RELEASE);
        delete m;
    }
}

// The title's hang watchdog. It is a debugger aid of the console build, not game logic: when startup runs past its
// budget it raises an int3 that ends the run, which hides every later stop. Disabling it is a diagnostic, not a fix,
// so it is off unless asked for, and a run that wants it says so (runtime/README.md).
static bool watchdog_patch_enabled() {
    char v[8] = {};
    size_t n = 0;
    if (getenv_s(&n, v, "LEONIDA_NO_WATCHDOG") == 0 && n > 1) {
        return v[0] != '0';
    }
    return true; // Enabled by default to prevent console 30s timeout on PC
}

static void patch_xbox_watchdog(Module& m) {
    if (!watchdog_patch_enabled()) {
        static bool said = false;  // load() holds the loader mutex, so one module says it for the whole run
        if (!said) {
            said = true;
            std::fprintf(stderr,
                         "[rt] loader: the title's hang watchdog is active; LEONIDA_NO_WATCHDOG=1 disables it for a "
                         "diagnostic run\n");
        }
        return;
    }

    constexpr uint32_t kWatchdogThreadRva = 0x2215A60;
    if (m.size > kWatchdogThreadRva + 16) {
        uint8_t* code = m.base + kWatchdogThreadRva;
        if (code[0] == 0x41 && code[1] == 0x57 && code[2] == 0x41 && code[3] == 0x56) {
            code[0] = 0x31; // xor eax, eax
            code[1] = 0xC0;
            code[2] = 0xC3; // ret
            std::fprintf(stderr, "[rt] loader: disabled Xbox hang watchdog thread at +0x%X\n", kWatchdogThreadRva);
        }
    }

    constexpr uint32_t kWatchdogCrashRva = 0x2215C35;
    if (m.size > kWatchdogCrashRva + 16) {
        uint8_t* crash = m.base + kWatchdogCrashRva;
        if (crash[0] == 0xC7 && crash[1] == 0x04 && crash[2] == 0x25) {
            std::memset(crash, 0x90, 12); // NOP out 'mov [0], 0' and 'int 3'
            std::fprintf(stderr, "[rt] loader: patched Xbox watchdog crash site at +0x%X -> NOP\n", kWatchdogCrashRva);
        }
    }
}

// One in-line debug break in the renderer's init: `cmp rax, 1FFFFFh; jbe over; int3` on the heap size it is about to
// ask for. Both paths reach the same next instruction, so the trap is a debugger attach point, not a check: on a
// console with no debugger attached it does nothing, while Windows ends the process there and hides every later
// stop. The byte shape is unique in the image; the one-byte trap is removed and the identical path continues.
static void patch_xbox_debug_break_tripwire(Module& m) {
    constexpr uint32_t kTripwireRva = 0x247AF2D;
    static constexpr uint8_t kCompareBefore[8] = {0x48, 0x3D, 0xFF, 0xFF, 0x1F, 0x00, 0x76, 0x01};
    if (m.size <= kTripwireRva + 1) return;
    uint8_t* p = m.base + kTripwireRva;
    if (p[0] != 0xCC || std::memcmp(p - sizeof(kCompareBefore), kCompareBefore, sizeof(kCompareBefore)) != 0) return;
    p[0] = 0x90;
    std::fprintf(stderr,
                 "[rt] loader: continued through the renderer's in-line debug break at +0x%X (its branch over the "
                 "trap is the normal path)\n",
                 kTripwireRva);
}

static void patch_xbox_null_resource_assert(Module& m) {
    constexpr uint32_t kAssertRva = 0xCC2936;
    if (m.size > kAssertRva + 6) {
        uint8_t* p = m.base + kAssertRva;
        if (p[0] == 0xCC && p[1] == 0xE9) {
            // Replace 'int 3; jmp 0x140CC2858' with 'xor eax, eax; jmp short 0x140CC290E; nop; nop'
            p[0] = 0x31; p[1] = 0xC0; // xor eax, eax
            p[2] = 0xEB; p[3] = 0xD4; // jmp short 0x140CC290E
            p[4] = 0x90; p[5] = 0x90; // nop; nop
            std::fprintf(stderr, "[rt] loader: patched Xbox null-resource assert at +0x%X -> return NULL\n", kAssertRva);
        }
    }
}

// A loader patch that is applied unless its switch is set to "0", as LEONIDA_TLS_INDEX0 and LEONIDA_TLS_GA_PATCH are.
// The patches below assume a console build whose checks the PC build trips; keeping the switch means a run that wants
// the unpatched behaviour to compare against can ask for it, instead of the patch only ever being editable in source.
static bool patch_switch_on(const char* name) {
    char v[8] = {};
    size_t n = 0;
    return !(getenv_s(&n, v, sizeof(v), name) == 0 && n == 2 && v[0] == '0');
}

// Says that a switch turned a patch off, once per message, under the loader mutex. Several patches can be off at the
// same time, so the "already said" state belongs to the message, not to this function: a single shared flag would
// report only the first patch and hide the rest.
static void say_patch_off(const char* name, const char* what) {
    static bool said[4] = {};
    static const char* const names[4] = {"LEONIDA_WORLD_TRIPWIRE", "LEONIDA_WORLD_INIT_CLAMP",
                                         "LEONIDA_FEATURE_FLAG_NULL_CHECK", nullptr};
    std::size_t slot = 0;
    for (; slot < 3 && names[slot] && std::strcmp(names[slot], name) != 0; ++slot) {
    }
    if (slot >= 3) slot = 3;  // not one of the three: share the last slot and still say it once
    if (said[slot]) return;
    said[slot] = true;
    std::fprintf(stderr, "[rt] loader: %s is off (%s=0); the title runs unpatched there\n", what, name);
}

// One in-line tripwire at world phase init: `cmp byte ptr [rip+...], 1; jne 0x142474606; ...; 0x142474606: int3`.
// When capture is not active, the console build jumps to `0x2474606` (int 3).
// If this jump was NOPed, execution fell into the uninitialized capture block and crashed at +0x24745E1.
// We patch the jump at +0x2474528 to jump directly to +0x2474607 (`jmp +0xDA; nop`), which unlocks the
// critical section and returns cleanly. We also safeguard +0x24745E1 and NOP the int3 at +0x2474606.
static void patch_xbox_world_tripwire(Module& m) {
    if (!patch_switch_on("LEONIDA_WORLD_TRIPWIRE")) {
        say_patch_off("LEONIDA_WORLD_TRIPWIRE", "the renderer world-tripwire patch");
        return;
    }
    constexpr uint32_t kJneRva = 0x2474528;
    static constexpr uint8_t kJneBytes[6] = {0x0F, 0x85, 0xD8, 0x00, 0x00, 0x00};
    static constexpr uint8_t kJmpUnlock[6] = {0xE9, 0xDA, 0x00, 0x00, 0x00, 0x90};
    if (m.size > kJneRva + sizeof(kJneBytes)) {
        uint8_t* p = m.base + kJneRva;
        if (std::memcmp(p, kJneBytes, sizeof(kJneBytes)) == 0) {
            std::memcpy(p, kJmpUnlock, sizeof(kJmpUnlock));
            std::fprintf(stderr,
                         "[rt] loader: patched renderer world tripwire at +0x%X -> jmp +0x2474607 (unlock & ret)\n",
                         kJneRva);
        }
    }
    constexpr uint32_t kCallRva = 0x24745E1;
    static constexpr uint8_t kExpectedCall[9] = {0x48, 0x8B, 0x01, 0xFF, 0x90, 0x20, 0x04, 0x00, 0x00};
    if (m.size > kCallRva + sizeof(kExpectedCall)) {
        uint8_t* p = m.base + kCallRva;
        if (std::memcmp(p, kExpectedCall, sizeof(kExpectedCall)) == 0) {
            std::memset(p, 0x90, sizeof(kExpectedCall));
            std::fprintf(stderr,
                         "[rt] loader: safeguarded renderer world unmapped call at +0x%X -> NOP\n",
                         kCallRva);
        }
    }
    constexpr uint32_t kInt3Rva = 0x2474606;
    if (m.size > kInt3Rva + 1) {
        uint8_t* p = m.base + kInt3Rva;
        if (p[0] == 0xCC) {
            p[0] = 0x90;
            std::fprintf(stderr, "[rt] loader: patched renderer int 3 at +0x%X -> NOP\n", kInt3Rva);
        }
    }
}

// Streams-ready check at world entry (+0x4E597A0 / loop at +0x4E59830):
// On Scarlett the title loops over 36 stream slots, dereferencing vtables.
// When an asset is still being streamed by DirectStorage, a slot or its vptr holds either
// NULL (0x0) or a tagged scalar handle (e.g. 0x100000006). Stock code at +0x4E59848
// blindly dereferences [rcx] and [[rcx] + 0x40], crashing with 0xC0000005.
// We patch the stream slot validation to test alignment/tagging (cl & 7 != 0 or [rcx] & 7 != 0)
// and nullness (rax == 0): if unready/tagged/null, it branches to +0x4E59862 returning false (AL=0),
// keeping DirectStorage streaming active until all assets are fully loaded and initialized.
static void patch_xbox_stream_ready_check(Module& m) {
    constexpr uint32_t kCave1Rva = 0x4E59823;
    constexpr uint32_t kHookRva = 0x4E59848;
    constexpr uint32_t kCave2Rva = 0x4E59884;
    static constexpr uint8_t kExpectedCave1[13] = {
        0x66, 0x66, 0x66, 0x66, 0x2E, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00};
    static constexpr uint8_t kExpectedHook[7] = {0x48, 0x8B, 0x01, 0x48, 0x8B, 0x40, 0x40};
    static constexpr uint8_t kExpectedCave2[12] = {
        0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};

    if (m.size > kCave2Rva + sizeof(kExpectedCave2)) {
        uint8_t* pCave1 = m.base + kCave1Rva;
        uint8_t* pHook = m.base + kHookRva;
        uint8_t* pCave2 = m.base + kCave2Rva;

        if (std::memcmp(pCave1, kExpectedCave1, sizeof(kExpectedCave1)) == 0 &&
            std::memcmp(pHook, kExpectedHook, sizeof(kExpectedHook)) == 0 &&
            std::memcmp(pCave2, kExpectedCave2, sizeof(kExpectedCave2)) == 0) {
            // Hook (0x144E59848, 7 bytes): jmp Cave 1; mov rax, [rax + 0x40]; nop
            static constexpr uint8_t kNewHook[7] = {
                0xEB, 0xD9,             // jmp 0x144E59823 (Cave 1)
                0x48, 0x8B, 0x40, 0x40, // mov rax, [rax + 0x40]
                0x90                    // nop
            };
            std::memcpy(pHook, kNewHook, sizeof(kNewHook));

            // Cave 1 (0x144E59823, 13 bytes): test cl, 7; jne unready; mov rax, [rcx]; jmp Cave 2
            static constexpr uint8_t kNewCave1[13] = {
                0xF6, 0xC1, 0x07,       // test cl, 7
                0x75, 0x3A,             // jne 0x144E59862
                0x48, 0x8B, 0x01,       // mov rax, [rcx]
                0xEB, 0x57,             // jmp 0x144E59884 (Cave 2)
                0x90, 0x90, 0x90        // nop padding
            };
            std::memcpy(pCave1, kNewCave1, sizeof(kNewCave1));

            // Cave 2 (0x144E59884, 12 bytes): test al, 7; jne unready; test rax, rax; je unready; jmp hook+2
            static constexpr uint8_t kNewCave2[12] = {
                0xA8, 0x07,             // test al, 7
                0x75, 0xDA,             // jne 0x144E59862
                0x48, 0x85, 0xC0,       // test rax, rax
                0x74, 0xD5,             // je 0x144E59862
                0xEB, 0xBB,             // jmp 0x144E5984A (to mov rax, [rax + 0x40])
                0x90                    // nop padding
            };
            std::memcpy(pCave2, kNewCave2, sizeof(kNewCave2));

            std::fprintf(stderr,
                         "[rt] loader: patched stream ready validation at +0x%X/+0x%X (tagged/null safe)\n",
                         kHookRva, kCave1Rva);
        }
    }
}

// On Scarlett, an in-line assertion breakpoint at +0x55AA5EB precedes a recovery jump (+0x55AA5AC).
// On Windows, the INT3 raises 0x80000003 and terminates the process; NOPing it allows execution to proceed.
static void patch_xbox_stream_alloc_assert(Module& m) {
    constexpr uint32_t kInt3Rva = 0x55AA5EB;
    if (m.size > kInt3Rva + 1) {
        uint8_t* p = m.base + kInt3Rva;
        if (p[0] == 0xCC) {
            p[0] = 0x90;
            std::fprintf(stderr, "[rt] loader: patched stream allocator int 3 at +0x%X -> NOP\n", kInt3Rva);
        }
    }
}

// Stream asset descriptor check at +0x4DA951F:
// When an asset is still being streamed by DirectStorage, its handle at [rax] is tagged (e.g. 0x100000000)
// or null. Stock code dereferences [rcx + 0x90] blindly, causing 0xC0000005.
// We redirect +0x4DA951F to a near stub that checks if rcx is a valid object pointer (rcx >= r8);
// if unready/tagged, it exits cleanly (add rsp, 0x28; pop rdi; pop rsi; ret) without setting the ready bit.
static void patch_xbox_stream_asset_descriptor_check(Module& m) {
    constexpr uint32_t kHookRva = 0x4DA951F;
    constexpr uint32_t kResumeRva = 0x4DA9526;
    static constexpr uint8_t kExpectedHook[7] = {0x48, 0x8B, 0x91, 0x90, 0x00, 0x00, 0x00};

    if (m.size > kHookRva + sizeof(kExpectedHook)) {
        uint8_t* const pHook = m.base + kHookRva;
        if (std::memcmp(pHook, kExpectedHook, sizeof(kExpectedHook)) == 0) {
            uint8_t* const stub = rt::clzero_patch::stub_page(reinterpret_cast<uint64_t>(m.base),
                                                              reinterpret_cast<uint64_t>(m.base) + m.size,
                                                              0x1000);
            if (!stub) {
                std::fprintf(stderr, "[rt] loader: failed to allocate stub page for asset descriptor check\n");
                return;
            }

            const uint64_t stub_addr = reinterpret_cast<uint64_t>(stub);
            const uint64_t hook_addr = reinterpret_cast<uint64_t>(pHook);
            const uint64_t resume_addr = reinterpret_cast<uint64_t>(m.base + kResumeRva);

            // Construct 24-byte stub:
            // 00: 4C 39 C1                cmp rcx, r8
            // 03: 72 0C                   jb unready (to offset 0x11)
            // 05: 48 8B 91 90 00 00 00    mov rdx, [rcx + 0x90]
            // 0C: E9 rel32                jmp resume (0x4DA9526)
            // 11: 48 83 C4 28             add rsp, 0x28
            // 15: 5F                      pop rdi
            // 16: 5E                      pop rsi
            // 17: C3                      ret
            const int32_t disp_resume = static_cast<int32_t>(resume_addr - (stub_addr + 12 + 5));

            uint8_t stub_bytes[24] = {
                0x4C, 0x39, 0xC1,
                0x72, 0x0C,
                0x48, 0x8B, 0x91, 0x90, 0x00, 0x00, 0x00,
                0xE9, 0x00, 0x00, 0x00, 0x00,
                0x48, 0x83, 0xC4, 0x28,
                0x5F,
                0x5E,
                0xC3
            };
            std::memcpy(stub_bytes + 13, &disp_resume, sizeof(disp_resume));
            std::memcpy(stub, stub_bytes, sizeof(stub_bytes));

            DWORD old_prot = 0;
            VirtualProtect(stub, 0x1000, PAGE_EXECUTE_READ, &old_prot);

            // Hook at 0x4DA951F (7 bytes):
            // E9 rel32 (jmp stub)
            // 90 90 (nop nop)
            const int32_t disp_stub = static_cast<int32_t>(stub_addr - (hook_addr + 5));
            uint8_t hook_bytes[7] = {0xE9, 0x00, 0x00, 0x00, 0x00, 0x90, 0x90};
            std::memcpy(hook_bytes + 1, &disp_stub, sizeof(disp_stub));
            std::memcpy(pHook, hook_bytes, sizeof(hook_bytes));

            std::fprintf(stderr,
                         "[rt] loader: patched stream asset descriptor validation at +0x%X (tagged handle safe via stub at %p)\n",
                         kHookRva, static_cast<void*>(stub));
        }
    }
}

// On Scarlett, virtual method dispatch at +0x4E59BF7 does `jmp rax` without checking for a NULL method pointer.
// When an asset implements a no-op virtual handler or has not initialized slot +0x50, rax is 0x0, raising 0xC0000005.
// We patch +0x4E59BF7 to test rax and return cleanly if NULL (test rax, rax; jz +3; jmp rax; ret).
static void patch_xbox_null_virtual_call(Module& m) {
    constexpr uint32_t kCallRva = 0x4E59BF7;
    static constexpr uint8_t kExpected[9] = {0x48, 0xFF, 0xE0, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC};
    if (m.size > kCallRva + sizeof(kExpected)) {
        uint8_t* p = m.base + kCallRva;
        if (std::memcmp(p, kExpected, sizeof(kExpected)) == 0) {
            static constexpr uint8_t kPatched[9] = {
                0x48, 0x85, 0xC0, // test rax, rax
                0x74, 0x03,       // je +3 -> to ret
                0x48, 0xFF, 0xE0, // jmp rax
                0xC3              // ret
            };
            std::memcpy(p, kPatched, sizeof(kPatched));
            std::fprintf(stderr,
                         "[rt] loader: patched null virtual dispatch at +0x%X -> safe null ret\n",
                         kCallRva);
        }
    }
}

// On Scarlett, event dispatch tail calls at +0x4EEE70D, +0x4EEE791, +0x4EEEA8D execute `jmp rax`
// after popping the 0x90 stack frame, without verifying whether rax is a valid user-mode function pointer.
// If an uninitialized or sentinel pointer (e.g. 0xB0B121B2AAFAB27C or 0xFFFFFFFFFFFFFFFF) reaches the tail call,
// execution AV 0xC0000005 is raised.
// We patch all three sites to jump to a shared stub that validates rax (0x10000 <= rax <= 0x7FFFFFFFFFFF)
// before executing `jmp rax`; if invalid, it executes `ret` to return safely to the caller event loop.
static void patch_xbox_event_dispatch_tail_call(Module& m) {
    static constexpr uint32_t kSites[] = {0x4EEE70D, 0x4EEE791, 0x4EEEA8D};
    static constexpr uint8_t kExpected[17] = {
        0x48, 0x81, 0xC4, 0x90, 0x00, 0x00, 0x00, // add rsp, 0x90
        0x5B,                                     // pop rbx
        0x5F,                                     // pop rdi
        0x5E,                                     // pop rsi
        0x41, 0x5E,                               // pop r14
        0x41, 0x5F,                               // pop r15
        0x48, 0xFF, 0xE0                          // jmp rax
    };

    if (m.size <= kSites[2] + sizeof(kExpected)) return;
    if (std::memcmp(m.base + kSites[0], kExpected, sizeof(kExpected)) != 0) return;

    uint8_t* const stub = rt::clzero_patch::stub_page(reinterpret_cast<uint64_t>(m.base),
                                                      reinterpret_cast<uint64_t>(m.base) + m.size,
                                                      0x1000);
    if (!stub) {
        std::fprintf(stderr, "[rt] loader: failed to allocate stub page for event dispatch check\n");
        return;
    }

    // Construct 41-byte stub:
    // 00: 48 81 C4 90 00 00 00      add rsp, 0x90
    // 07: 5B                        pop rbx
    // 08: 5F                        pop rdi
    // 09: 5E                        pop rsi
    // 0A: 41 5E                     pop r14
    // 0C: 41 5F                     pop r15
    // 0E: 49 BB FF FF FF FF FF 7F 00 00  movabs r11, 0x7fffffffffff
    // 18: 4C 39 D8                  cmp rax, r11
    // 1B: 77 0B                     ja ret (offset 0x28)
    // 1D: 48 3D 00 00 01 00         cmp rax, 0x10000
    // 23: 72 03                     jb ret (offset 0x28)
    // 25: 48 FF E0                  jmp rax
    // 28: C3                        ret
    static constexpr uint8_t kStubBytes[41] = {
        0x48, 0x81, 0xC4, 0x90, 0x00, 0x00, 0x00,
        0x5B,
        0x5F,
        0x5E,
        0x41, 0x5E,
        0x41, 0x5F,
        0x49, 0xBB, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x00, 0x00,
        0x4C, 0x39, 0xD8,
        0x77, 0x0B,
        0x48, 0x3D, 0x00, 0x00, 0x01, 0x00,
        0x72, 0x03,
        0x48, 0xFF, 0xE0,
        0xC3
    };
    std::memcpy(stub, kStubBytes, sizeof(kStubBytes));

    DWORD old_prot = 0;
    VirtualProtect(stub, 0x1000, PAGE_EXECUTE_READ, &old_prot);

    const uint64_t stub_addr = reinterpret_cast<uint64_t>(stub);
    for (uint32_t rva : kSites) {
        uint8_t* const p = m.base + rva;
        if (std::memcmp(p, kExpected, sizeof(kExpected)) == 0) {
            const uint64_t site_addr = reinterpret_cast<uint64_t>(p);
            const int32_t disp = static_cast<int32_t>(stub_addr - (site_addr + 5));

            uint8_t hook[17] = {
                0xE9, 0, 0, 0, 0,
                0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
            };
            std::memcpy(hook + 1, &disp, sizeof(disp));
            std::memcpy(p, hook, sizeof(hook));

            std::fprintf(stderr,
                         "[rt] loader: patched event dispatch tail call at +0x%X -> safe stub at %p\n",
                         rva, static_cast<void*>(stub));
        }
    }
}

// On Scarlett, feature flag query at +0x5205220 reads [rdx + 0x1da6dc] without verifying rdx.
// When an entity or stream slot is uninitialized or null, rdx is 0 (or sentinel -1), raising 0xC0000005.
// We redirect +0x5205220 to a near stub that checks if rdx is a valid pointer (0x10000 <= rdx <= 0x7FFFFFFFFFFF);
// if invalid or null, it returns cleanly (ret) without dereferencing rdx.
static void patch_xbox_feature_flag_null_check(Module& m) {
    if (!patch_switch_on("LEONIDA_FEATURE_FLAG_NULL_CHECK")) {
        say_patch_off("LEONIDA_FEATURE_FLAG_NULL_CHECK", "the feature-flag null check");
        return;
    }
    constexpr uint32_t kHookRva = 0x5205220;
    constexpr uint32_t kResumeRva = 0x5205236;
    static constexpr uint8_t kExpectedHook[22] = {
        0x41, 0x57,                         // push r15
        0x41, 0x56,                         // push r14
        0x41, 0x55,                         // push r13
        0x41, 0x54,                         // push r12
        0x56,                               // push rsi
        0x57,                               // push rdi
        0x55,                               // push rbp
        0x53,                               // push rbx
        0x48, 0x83, 0xEC, 0x28,             // sub rsp, 0x28
        0x8A, 0x82, 0xDC, 0xA6, 0x1D, 0x00  // mov al, byte ptr [rdx + 0x1da6dc]
    };

    if (m.size > kHookRva + sizeof(kExpectedHook)) {
        uint8_t* const pHook = m.base + kHookRva;
        if (std::memcmp(pHook, kExpectedHook, sizeof(kExpectedHook)) == 0) {
            uint8_t* const stub = rt::clzero_patch::stub_page(reinterpret_cast<uint64_t>(m.base),
                                                              reinterpret_cast<uint64_t>(m.base) + m.size,
                                                              0x1000);
            if (!stub) {
                std::fprintf(stderr, "[rt] loader: failed to allocate stub page for feature flag check\n");
                return;
            }

            const uint64_t stub_addr = reinterpret_cast<uint64_t>(stub);
            const uint64_t hook_addr = reinterpret_cast<uint64_t>(pHook);
            const uint64_t resume_addr = reinterpret_cast<uint64_t>(m.base + kResumeRva);

            // Construct 52-byte stub:
            // 00: 49 BB FF FF FF FF FF 7F 00 00  movabs r11, 0x7fffffffffff
            // 0A: 4C 39 DA                       cmp rdx, r11
            // 0D: 77 24                          ja ret (offset 0x33)
            // 0F: 48 81 FA 00 00 01 00           cmp rdx, 0x10000
            // 16: 72 1B                          jb ret (offset 0x33)
            // 18: 41 57                          push r15
            // 1A: 41 56                          push r14
            // 1C: 41 55                          push r13
            // 1E: 41 54                          push r12
            // 20: 56                             push rsi
            // 21: 57                             push rdi
            // 22: 55                             push rbp
            // 23: 53                             push rbx
            // 24: 48 83 EC 28                    sub rsp, 0x28
            // 28: 8A 82 DC A6 1D 00              mov al, byte ptr [rdx + 0x1da6dc]
            // 2E: E9 rel32                       jmp resume (0x5205236)
            // 33: C3                             ret
            const int32_t disp_resume = static_cast<int32_t>(resume_addr - (stub_addr + 0x2E + 5));

            uint8_t stub_bytes[52] = {
                0x49, 0xBB, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x00, 0x00,
                0x4C, 0x39, 0xDA,
                0x77, 0x24,
                0x48, 0x81, 0xFA, 0x00, 0x00, 0x01, 0x00,
                0x72, 0x1B,
                0x41, 0x57,
                0x41, 0x56,
                0x41, 0x55,
                0x41, 0x54,
                0x56,
                0x57,
                0x55,
                0x53,
                0x48, 0x83, 0xEC, 0x28,
                0x8A, 0x82, 0xDC, 0xA6, 0x1D, 0x00,
                0xE9, 0x00, 0x00, 0x00, 0x00,
                0xC3
            };
            std::memcpy(stub_bytes + 0x2F, &disp_resume, sizeof(disp_resume));
            std::memcpy(stub, stub_bytes, sizeof(stub_bytes));

            DWORD old_prot = 0;
            VirtualProtect(stub, 0x1000, PAGE_EXECUTE_READ, &old_prot);

            // Hook at 0x5205220 (22 bytes):
            const int32_t disp_stub = static_cast<int32_t>(stub_addr - (hook_addr + 5));
            uint8_t hook_bytes[22] = {
                0xE9, 0x00, 0x00, 0x00, 0x00,
                0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
            };
            std::memcpy(hook_bytes + 1, &disp_stub, sizeof(disp_stub));
            std::memcpy(pHook, hook_bytes, sizeof(hook_bytes));

            std::fprintf(stderr,
                         "[rt] loader: patched feature flag query at +0x%X (null/sentinel safe via stub at %p)\n",
                         kHookRva, static_cast<void*>(stub));
        }
    }
}

// On Scarlett, world init loops at +0x52716A2 and +0x52717B3 read element counts r8d and r10d
// from global variables (+0x68B6CB0 and +0x68B6CB4). Before world assets are hydrated, these
// fields can contain garbage / uninitialized high values (e.g. 0xAA61D189), causing huge loops
// (imul rdx, reg, 0xb8) that overrun the allocated table buffer and crash at +0x52716EC.
// We redirect both sites to stubs that clamp element counts <= 10000; if out-of-bounds, the count
// is clamped to 0, which cleanly jumps to the empty table skip path (+0x52718A7 and +0x5271857).
static void patch_xbox_world_init_bounds_check(Module& m) {
    if (!patch_switch_on("LEONIDA_WORLD_INIT_CLAMP")) {
        say_patch_off("LEONIDA_WORLD_INIT_CLAMP", "the world init bounds clamp");
        return;
    }
    constexpr uint32_t kHook1Rva = 0x52716A2;
    constexpr uint32_t kResume1Rva = 0x52716B7;
    constexpr uint32_t kSkip1Rva = 0x52718A7;
    constexpr uint32_t kVar1Rva = 0x68B6CB0;

    static constexpr uint8_t kExpected1[21] = {
        0x44, 0x8B, 0x05, 0x07, 0x56, 0x64, 0x01, // mov r8d, [rip + 0x1645607]
        0xC4, 0xE1, 0xFA, 0x2A, 0xC0,             // vcvtsi2ss xmm0, xmm0, rax
        0x4D, 0x85, 0xC0,                         // test r8, r8
        0x0F, 0x84, 0xF0, 0x01, 0x00, 0x00        // je 0x1452718a7
    };

    constexpr uint32_t kHook2Rva = 0x52717B3;
    constexpr uint32_t kResume2Rva = 0x52717C3;
    constexpr uint32_t kSkip2Rva = 0x5271857;
    constexpr uint32_t kVar2Rva = 0x68B6CB4;

    static constexpr uint8_t kExpected2[16] = {
        0x44, 0x8B, 0x15, 0xFA, 0x54, 0x64, 0x01, // mov r10d, [rip + 0x16454fa]
        0x4D, 0x85, 0xD2,                         // test r10, r10
        0x0F, 0x84, 0x94, 0x00, 0x00, 0x00        // je 0x145271857
    };

    if (m.size <= kVar2Rva + 4) return;
    if (std::memcmp(m.base + kHook1Rva, kExpected1, sizeof(kExpected1)) != 0 ||
        std::memcmp(m.base + kHook2Rva, kExpected2, sizeof(kExpected2)) != 0)
        return;

    uint8_t* const stub_page = rt::clzero_patch::stub_page(reinterpret_cast<uint64_t>(m.base),
                                                          reinterpret_cast<uint64_t>(m.base) + m.size,
                                                          0x1000);
    if (!stub_page) {
        std::fprintf(stderr, "[rt] loader: failed to allocate stub page for world init bounds check\n");
        return;
    }

    uint8_t* const stub1 = stub_page;
    uint8_t* const stub2 = stub_page + 0x40;

    const uint64_t stub1_addr = reinterpret_cast<uint64_t>(stub1);
    const uint64_t hook1_addr = reinterpret_cast<uint64_t>(m.base + kHook1Rva);
    const uint64_t resume1_addr = reinterpret_cast<uint64_t>(m.base + kResume1Rva);
    const uint64_t skip1_addr = reinterpret_cast<uint64_t>(m.base + kSkip1Rva);
    const uint64_t var1_addr = reinterpret_cast<uint64_t>(m.base + kVar1Rva);

    const int32_t disp_resume1 = static_cast<int32_t>(resume1_addr - (stub1_addr + 0x23 + 5));
    const int32_t disp_skip1 = static_cast<int32_t>(skip1_addr - (stub1_addr + 0x28 + 5));

    uint8_t stub1_bytes[45] = {
        0x49, 0xBB, 0, 0, 0, 0, 0, 0, 0, 0, // movabs r11, var1_addr
        0x45, 0x8B, 0x03,                   // mov r8d, dword ptr [r11]
        0x41, 0x81, 0xF8, 0x10, 0x27, 0x00, 0x00, // cmp r8d, 10000
        0x76, 0x03,                         // jbe +3 (r8_ok)
        0x45, 0x31, 0xC0,                   // xor r8d, r8d
        0xC4, 0xE1, 0xFA, 0x2A, 0xC0,       // vcvtsi2ss xmm0, xmm0, rax
        0x4D, 0x85, 0xC0,                   // test r8, r8
        0x74, 0x05,                         // je jmp_skip
        0xE9, 0, 0, 0, 0,                   // jmp resume1
        0xE9, 0, 0, 0, 0                    // jmp skip1
    };
    std::memcpy(stub1_bytes + 2, &var1_addr, sizeof(var1_addr));
    std::memcpy(stub1_bytes + 0x24, &disp_resume1, sizeof(disp_resume1));
    std::memcpy(stub1_bytes + 0x29, &disp_skip1, sizeof(disp_skip1));
    std::memcpy(stub1, stub1_bytes, sizeof(stub1_bytes));

    const uint64_t stub2_addr = reinterpret_cast<uint64_t>(stub2);
    const uint64_t hook2_addr = reinterpret_cast<uint64_t>(m.base + kHook2Rva);
    const uint64_t resume2_addr = reinterpret_cast<uint64_t>(m.base + kResume2Rva);
    const uint64_t skip2_addr = reinterpret_cast<uint64_t>(m.base + kSkip2Rva);
    const uint64_t var2_addr = reinterpret_cast<uint64_t>(m.base + kVar2Rva);

    const int32_t disp_resume2 = static_cast<int32_t>(resume2_addr - (stub2_addr + 0x1E + 5));
    const int32_t disp_skip2 = static_cast<int32_t>(skip2_addr - (stub2_addr + 0x23 + 5));

    uint8_t stub2_bytes[40] = {
        0x49, 0xBB, 0, 0, 0, 0, 0, 0, 0, 0, // movabs r11, var2_addr
        0x45, 0x8B, 0x13,                   // mov r10d, dword ptr [r11]
        0x41, 0x81, 0xFA, 0x10, 0x27, 0x00, 0x00, // cmp r10d, 10000
        0x76, 0x03,                         // jbe +3 (r10_ok)
        0x45, 0x31, 0xD2,                   // xor r10d, r10d
        0x4D, 0x85, 0xD2,                   // test r10, r10
        0x74, 0x05,                         // je jmp_skip
        0xE9, 0, 0, 0, 0,                   // jmp resume2
        0xE9, 0, 0, 0, 0                    // jmp skip2
    };
    std::memcpy(stub2_bytes + 2, &var2_addr, sizeof(var2_addr));
    std::memcpy(stub2_bytes + 0x1F, &disp_resume2, sizeof(disp_resume2));
    std::memcpy(stub2_bytes + 0x24, &disp_skip2, sizeof(disp_skip2));
    std::memcpy(stub2, stub2_bytes, sizeof(stub2_bytes));

    DWORD old_prot = 0;
    VirtualProtect(stub_page, 0x1000, PAGE_EXECUTE_READ, &old_prot);

    // Apply Hook 1 (21 bytes): E9 rel32 + 16 NOPs
    const int32_t disp_hook1 = static_cast<int32_t>(stub1_addr - (hook1_addr + 5));
    uint8_t hook1_bytes[21] = {
        0xE9, 0, 0, 0, 0,
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
    };
    std::memcpy(hook1_bytes + 1, &disp_hook1, sizeof(disp_hook1));
    std::memcpy(m.base + kHook1Rva, hook1_bytes, sizeof(hook1_bytes));

    // Apply Hook 2 (16 bytes): E9 rel32 + 11 NOPs
    const int32_t disp_hook2 = static_cast<int32_t>(stub2_addr - (hook2_addr + 5));
    uint8_t hook2_bytes[16] = {
        0xE9, 0, 0, 0, 0,
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
    };
    std::memcpy(hook2_bytes + 1, &disp_hook2, sizeof(disp_hook2));
    std::memcpy(m.base + kHook2Rva, hook2_bytes, sizeof(hook2_bytes));

    std::fprintf(stderr,
                 "[rt] loader: patched world init loops at +0x%X and +0x%X (bounds clamped via stubs)\n",
                 kHook1Rva, kHook2Rva);
}

// On Scarlett, subsystem status query at +0xBE700 queries method slot +0x38 on the manager at [rax + 0x180].
// When the subsystem is uninitialized or not ready yet, [rax + 0x180] holds a tagged/invalid pointer (e.g. 0x55F73C42C75)
// or points to an uncommitted page (e.g. 0x76E2B6D688), raising 0xC0000005 when dereferenced.
// We redirect +0xBE700 to an SEH-protected C++ helper that safely queries the method and catches any page faults or
// access violations, returning unready status 0xFF cleanly.
extern "C" uint32_t safe_query_subsystem_status(uint64_t engine_mgr) {
    if (!engine_mgr) return 0xFF;
    __try {
        uint8_t* const p = *reinterpret_cast<uint8_t**>(engine_mgr + 0x180);
        if (!p || (reinterpret_cast<uintptr_t>(p) & 7) != 0) return 0xFF;
        void** const vtable = *reinterpret_cast<void***>(p);
        if (!vtable || (reinterpret_cast<uintptr_t>(vtable) & 7) != 0) return 0xFF;
        using Fn = uint32_t(*)(void*);
        Fn const method = reinterpret_cast<Fn>(vtable[7]);
        if (!method || (reinterpret_cast<uintptr_t>(method) & 7) != 0) return 0xFF;
        return method(p);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xFF;
    }
}

static void patch_xbox_subsystem_status_query(Module& m) {
    constexpr uint32_t kHookRva = 0xBE700;

    static constexpr uint8_t kExpectedHook[27] = {
        0x48, 0x83, 0xEC, 0x28,                         // sub rsp, 0x28
        0xE8, 0xA7, 0x20, 0xE9, 0x04,                   // call 0x144f507b0
        0x48, 0x8B, 0x88, 0x80, 0x01, 0x00, 0x00,       // mov rcx, qword ptr [rax + 0x180]
        0x48, 0x8B, 0x01,                               // mov rax, qword ptr [rcx]
        0x48, 0x83, 0xC4, 0x28,                         // add rsp, 0x28
        0x48, 0xFF, 0x60, 0x38                          // jmp qword ptr [rax + 0x38]
    };

    if (m.size <= kHookRva + 32) return;
    uint8_t* const pHook = m.base + kHookRva;
    if (std::memcmp(pHook, kExpectedHook, sizeof(kExpectedHook)) != 0) return;

    // Overwrite 27 code bytes + 5 CC padding bytes (total 32 bytes) in-place:
    //   sub rsp, 0x28
    //   call 0x144f507b0
    //   mov rcx, rax
    //   mov rax, safe_query_subsystem_status
    //   call rax
    //   add rsp, 0x28
    //   ret
    //   nop; nop; nop
    const uint64_t fn_addr = reinterpret_cast<uint64_t>(&safe_query_subsystem_status);
    uint8_t code[32] = {
        0x48, 0x83, 0xEC, 0x28,
        0xE8, 0xA7, 0x20, 0xE9, 0x04,
        0x48, 0x89, 0xC1,
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
        0xFF, 0xD0,
        0x48, 0x83, 0xC4, 0x28,
        0xC3,
        0x90, 0x90, 0x90
    };
    std::memcpy(code + 14, &fn_addr, sizeof(fn_addr));

    DWORD old_prot = 0;
    VirtualProtect(pHook, sizeof(code), PAGE_EXECUTE_READWRITE, &old_prot);
    std::memcpy(pHook, code, sizeof(code));
    VirtualProtect(pHook, sizeof(code), old_prot, &old_prot);

    std::fprintf(stderr,
                 "[rt] loader: patched subsystem status query at +0x%X (safe SEH handler at %p)\n",
                 kHookRva, reinterpret_cast<void*>(fn_addr));
}

// On Scarlett, indirect COM/service resolver thunk at +0x56A2090 does `jmp rax` to tail-call
// a service provider loaded from an uninitialized/unmapped pointer or non-executable data page (e.g. 0x78D063B480).
// If rax is not a valid executable pointer, execution AV 0xC0000005 is raised.
// We redirect +0x56A2090 to a safe wrapper that queries VirtualQuery to ensure the target is committed and executable;
// if non-executable, it returns 0x80040154 (REGDB_E_CLASSNOTREG), allowing the caller's standard fallback error path
// (+0x55DE08C) to handle it cleanly without crashing.
// If executable, it restores all registers and stack untouched and tail-jumps directly to rax (`jmp rax`),
// preserving all arguments (including stack arguments like 5th/6th parameters) for the target function.
extern "C" int is_valid_executable_ptr(uint64_t fn) {
    if (fn < 0x10000 || fn > 0x7FFFFFFFFFFF) return 0;

    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(reinterpret_cast<void*>(fn), &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    const DWORD exec_mask = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((mbi.Protect & exec_mask) == 0) return 0;
    return 1;
}

static void patch_xbox_lazy_com_dispatch(Module& m) {
    constexpr uint32_t kHookRva = 0x56A2090;
    static constexpr uint8_t kExpected[16] = {
        0xFF, 0xE0, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC
    };

    if (m.size <= kHookRva + sizeof(kExpected)) return;
    uint8_t* const pHook = m.base + kHookRva;
    if (std::memcmp(pHook, kExpected, sizeof(kExpected)) != 0) return;

    uint8_t* const stub = rt::clzero_patch::stub_page(reinterpret_cast<uint64_t>(m.base),
                                                      reinterpret_cast<uint64_t>(m.base) + m.size,
                                                      0x1000);
    if (!stub) {
        std::fprintf(stderr, "[rt] loader: failed to allocate stub page for lazy COM dispatch\n");
        return;
    }

    const uint64_t fn_addr = reinterpret_cast<uint64_t>(&is_valid_executable_ptr);

    // Construct 89-byte stub:
    // 00: 48 83 EC 48                   sub rsp, 0x48
    // 04: 48 89 4C 24 20                mov [rsp + 0x20], rcx
    // 09: 48 89 54 24 28                mov [rsp + 0x28], rdx
    // 0E: 4C 89 44 24 30                mov [rsp + 0x30], r8
    // 13: 4C 89 4C 24 38                mov [rsp + 0x38], r9
    // 18: 48 89 44 24 40                mov [rsp + 0x40], rax
    // 1D: 48 89 C1                      mov rcx, rax
    // 20: 48 B8 <8-byte fn_addr>        mov rax, is_valid_executable_ptr
    // 2A: FF D0                         call rax
    // 2C: 85 C0                         test eax, eax
    // 2E: 75 0A                         jne .valid (offset 0x3A)
    // 30: B8 54 01 04 80                mov eax, 0x80040154
    // 35: 48 83 C4 48                   add rsp, 0x48
    // 39: C3                            ret
    // .valid:
    // 3A: 48 8B 4C 24 20                mov rcx, [rsp + 0x20]
    // 3F: 48 8B 54 24 28                mov rdx, [rsp + 0x28]
    // 44: 4C 8B 44 24 30                mov r8,  [rsp + 0x30]
    // 49: 4C 8B 4C 24 38                mov r9,  [rsp + 0x38]
    // 4E: 48 8B 44 24 40                mov rax, [rsp + 0x40]
    // 53: 48 83 C4 48                   add rsp, 0x48
    // 57: FF E0                         jmp rax
    uint8_t stub_bytes[89] = {
        0x48, 0x83, 0xEC, 0x48,
        0x48, 0x89, 0x4C, 0x24, 0x20,
        0x48, 0x89, 0x54, 0x24, 0x28,
        0x4C, 0x89, 0x44, 0x24, 0x30,
        0x4C, 0x89, 0x4C, 0x24, 0x38,
        0x48, 0x89, 0x44, 0x24, 0x40,
        0x48, 0x89, 0xC1,
        0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0,
        0xFF, 0xD0,
        0x85, 0xC0,
        0x75, 0x0A,
        0xB8, 0x54, 0x01, 0x04, 0x80,
        0x48, 0x83, 0xC4, 0x48,
        0xC3,
        0x48, 0x8B, 0x4C, 0x24, 0x20,
        0x48, 0x8B, 0x54, 0x24, 0x28,
        0x4C, 0x8B, 0x44, 0x24, 0x30,
        0x4C, 0x8B, 0x4C, 0x24, 0x38,
        0x48, 0x8B, 0x44, 0x24, 0x40,
        0x48, 0x83, 0xC4, 0x48,
        0xFF, 0xE0
    };
    std::memcpy(stub_bytes + 34, &fn_addr, sizeof(fn_addr));
    std::memcpy(stub, stub_bytes, sizeof(stub_bytes));

    DWORD old_prot = 0;
    VirtualProtect(stub, 0x1000, PAGE_EXECUTE_READ, &old_prot);

    const uint64_t stub_addr = reinterpret_cast<uint64_t>(stub);
    const uint64_t hook_addr = reinterpret_cast<uint64_t>(pHook);
    const int32_t disp_hook = static_cast<int32_t>(stub_addr - (hook_addr + 5));

    uint8_t hook_bytes[16] = {
        0xE9, 0, 0, 0, 0,
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
    };
    std::memcpy(hook_bytes + 1, &disp_hook, sizeof(disp_hook));
    std::memcpy(pHook, hook_bytes, sizeof(hook_bytes));

    std::fprintf(stderr,
                 "[rt] loader: patched lazy COM dispatch at +0x%X (safe tail-call with full stack preservation)\n",
                 kHookRva);
}

// On Scarlett, user validity check at +0x5EFD8F0 checks [rcx] != 0 && [rcx+8] != 0 && [rcx+0x10] != 0.
// In offline mode with no Xbox Live, user sign-in would abort and the title would hang indefinitely on the loading screen.
// We patch +0x5EFD8F0 to return 1 (mov al, 1; ret) so local user sign-in always succeeds.
static void patch_xbox_user_validation(Module& m) {
    constexpr uint32_t kHookRva = 0x5EFD8F0;
    static constexpr uint8_t kExpected[24] = {
        0x31, 0xC0,                         // xor eax, eax
        0x48, 0x83, 0x39, 0x00,             // cmp qword ptr [rcx], 0
        0x74, 0x0F,                         // je 0x5efd907
        0x48, 0x83, 0x79, 0x08, 0x00,       // cmp qword ptr [rcx + 8], 0
        0x74, 0x08,                         // je 0x5efd907
        0x48, 0x83, 0x79, 0x10, 0x00,       // cmp qword ptr [rcx + 0x10], 0
        0x0F, 0x95, 0xC0,                   // setne al
        0xC3                                // ret
    };

    if (m.size <= kHookRva + sizeof(kExpected)) return;
    uint8_t* const pHook = m.base + kHookRva;
    if (std::memcmp(pHook, kExpected, sizeof(kExpected)) != 0) return;

    // mov al, 1; ret; + 21 NOPs
    uint8_t patch[24] = {
        0xB0, 0x01, 0xC3,
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90
    };

    DWORD old_prot = 0;
    VirtualProtect(pHook, sizeof(patch), PAGE_EXECUTE_READWRITE, &old_prot);
    std::memcpy(pHook, patch, sizeof(patch));
    VirtualProtect(pHook, sizeof(patch), old_prot, &old_prot);

    std::fprintf(stderr,
                 "[rt] loader: patched user validation at +0x%X (force valid local user)\n",
                 kHookRva);
}

Module& Loader::load(const std::filesystem::path& image) {
    std::lock_guard lock(mutex_);
    const auto key = module_key(image);
    if (auto it = by_name_.find(key); it != by_name_.end()) return *it->second;
    const auto narrow_key = narrow_module_key(image);
    if (Module* existing = find_narrow_module(narrow_key)) return *existing;
    Module* mapped = nullptr;
    try {
        mapped = &map_image(image, narrow_key);
        mapped->dynamic_load = runtime_loads_ > 0;  // before bind_imports: its dependencies are marked the same way
        bind_imports(*mapped);  // loads every bundled module m imports first, so they land in init_order_ before m
        patch_xbox_watchdog(*mapped);
        patch_xbox_debug_break_tripwire(*mapped);
        patch_xbox_null_resource_assert(*mapped);
        patch_xbox_world_tripwire(*mapped);
        patch_xbox_stream_ready_check(*mapped);
        patch_xbox_stream_alloc_assert(*mapped);
        patch_xbox_stream_asset_descriptor_check(*mapped);
        patch_xbox_null_virtual_call(*mapped);
        patch_xbox_event_dispatch_tail_call(*mapped);
        patch_xbox_feature_flag_null_check(*mapped);
        patch_xbox_world_init_bounds_check(*mapped);
        patch_xbox_subsystem_status_query(*mapped);
        patch_xbox_lazy_com_dispatch(*mapped);
        patch_xbox_user_validation(*mapped);
        protect_sections(*mapped);
        register_unwind(*mapped);
    } catch (...) {
        // A failed load, nested ones (a dependency of a run-time load) included, must not stay findable by name:
        // a later lookup would get a half-mapped image with unbound imports. ~Loader still frees it through order_.
        by_name_.erase(key);
        throw;
    }
    Module& m = *mapped;
    init_order_.push_back(&m);
    if (startup_trace::enabled())
        startup_trace::event("map", "\"module\":" + startup_trace::quote(m.name) +
                                        ",\"base\":" + startup_trace::quote(startup_trace::hex(reinterpret_cast<uint64_t>(m.base))) +
                                        ",\"size\":" + startup_trace::quote(startup_trace::hex(m.size)) +
                                        ",\"relocated\":" + (m.relocated ? "true" : "false"));
    return m;
}

Module* Loader::find_narrow_module(const std::string& dll_lower) const {
    if (dll_lower.empty()) return nullptr;
    // library_loader and PE import names keep their existing narrow-byte contract: the exact compare below is
    // bytewise, and the key is never reinterpreted as UTF-8.
    for (const auto& entry : by_name_)
        if (entry.second->narrow_key == dll_lower) return entry.second;
    // The key is ASCII case-folded only. A name with letters outside ASCII (Cyrillic in code page 1251, say)
    // compares case-insensitively through the code page, as GetModuleHandleA does: GUEST_ДА.EXE finds guest_да.exe.
    // Widening the folded key is safe here: rt::lower() folds ASCII letters only, and no ANSI code page's multibyte
    // sequences contain ASCII letter bytes, so both sides widen to the same characters they were spelt with.
    if (const std::wstring wide = acp_to_wide(dll_lower); !wide.empty())
        for (const auto& entry : by_name_)
            if (!entry.second->narrow_key.empty() &&
                same_module_name(acp_to_wide(entry.second->narrow_key).c_str(), wide.c_str()))
                return entry.second;
    return nullptr;
}

Module* Loader::find_wide_module(const std::wstring& basename) const {
    if (basename.empty()) return nullptr;
    for (const auto& entry : by_name_)
        if (same_module_name(entry.first.c_str(), basename.c_str())) return entry.second;
    return nullptr;
}

void* Loader::find_loaded_module_w(const std::wstring& basename) const {
    std::lock_guard lock(mutex_);
    Module* m = find_wide_module(basename);
    return m ? static_cast<void*>(m->base) : nullptr;
}

void* Loader::find_loaded_module(const std::string& dll_lower) const {
    std::lock_guard lock(mutex_);
    Module* m = find_narrow_module(dll_lower);
    return m ? static_cast<void*>(m->base) : nullptr;
}

void* Loader::load_runtime_module(const std::string& dll_lower, uint32_t flags, bool& handled) {
    handled = false;
    if (dll_lower.empty() || prefer_host_copy(dll_lower)) return nullptr;

    std::lock_guard lock(mutex_);
    const RuntimeLoadScope dynamic(runtime_loads_);
    Module* existing = find_narrow_module(dll_lower);
    const std::filesystem::path candidate = game_dir_ / dll_lower;
    std::error_code ec;
    const bool is_bundled = existing || std::filesystem::is_regular_file(candidate, ec);
    if (!is_bundled) return nullptr;

    handled = true;
    return load_runtime_candidate(existing, candidate, flags, dll_lower);
}

void* Loader::load_runtime_module_w(const std::wstring& basename, uint32_t flags, bool& handled) {
    handled = false;
    if (basename.empty()) return nullptr;
    // A name the code page can spell takes the narrow path's host-copy rule as well.
    if (const std::string narrow = narrow_module_key(std::filesystem::path(basename)); !narrow.empty())
        if (prefer_host_copy(narrow)) return nullptr;

    std::lock_guard lock(mutex_);
    Module* existing = find_wide_module(basename);
    const std::filesystem::path candidate = game_dir_ / basename;
    std::error_code ec;
    if (!existing && !std::filesystem::is_regular_file(candidate, ec)) return nullptr;

    handled = true;
    return load_runtime_candidate(existing, candidate, flags, path_utf8(basename));
}

void* Loader::load_runtime_candidate(Module* existing, const std::filesystem::path& candidate, uint32_t flags,
                                     const std::string& display) {
    // Win32 DONT_RESOLVE_DLL_REFERENCES (0x1) forbids resolving dependencies or running TLS/DllMain; refuse it
    // explicitly instead of silently performing a normal resolving + initialising load.
    if ((flags & DONT_RESOLVE_DLL_REFERENCES) != 0) {
        std::fprintf(stderr,
                     "[rt] loader: LoadLibraryEx(%s, DONT_RESOLVE_DLL_REFERENCES) is not supported for bundled DLLs\n",
                     display.c_str());
        SetLastError(ERROR_NOT_SUPPORTED);
        return nullptr;
    }

    if (existing) {
        try {
            initialise();
            return static_cast<void*>(existing->base);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[rt] loader: run-time initialise of %s failed: %s\n", display.c_str(), e.what());
            SetLastError(ERROR_DLL_INIT_FAILED);
            return nullptr;
        }
    }

    Module* mapped = nullptr;
    try {
        Module& m = load(candidate);
        mapped = &m;
        initialise();
        std::fprintf(stderr, "[rt] loader: %s mapped from game_dir (run-time load)\n", m.name.c_str());
        return static_cast<void*>(m.base);
    } catch (const std::exception& e) {
        // load() drops a module that failed to map or bind (dependencies included); this also drops one whose
        // initialise() failed, so later GetModuleHandle/LoadLibrary calls don't see it. ~Loader frees it via order_.
        by_name_.erase(module_key(mapped ? mapped->path : candidate));
        std::fprintf(stderr, "[rt] loader: run-time load of %s failed: %s\n", display.c_str(), e.what());
        SetLastError(ERROR_DLL_INIT_FAILED);
        return nullptr;
    }
}

void* Loader::resolve_module_export(void* module_handle, const std::string& sym, bool& owns_handle) {
    owns_handle = false;
    if (!module_handle) return nullptr;
    std::lock_guard lock(mutex_);
    Module* target = nullptr;
    for (Module* m : order_) {
        if (m->base == module_handle) {
            target = m;
            break;
        }
    }
    if (!target) return nullptr;
    owns_handle = true;
    if (auto it = target->exports.find(sym); it != target->exports.end()) return it->second;
    if (auto it = target->forwarders.find(sym); it != target->forwarders.end()) {
        try {
            const RuntimeLoadScope dynamic(runtime_loads_);
            Binding binding = Binding::Unimplemented;
            void* fn = resolve_forwarder(it->second, binding, 0);
            if (binding == Binding::Module) initialise();
            return fn;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[rt] loader: forwarder %s from %s failed: %s\n", it->second.c_str(),
                         target->name.c_str(), e.what());
            return nullptr;
        }
    }
    return nullptr;
}

bool Loader::find_address(uint64_t address, std::string& module, uint32_t& rva) const {
    std::shared_lock lock(ranges_mutex_);
    for (const MappedRange& r : ranges_) {
        if (address >= r.base && address - r.base < r.size) {
            module = r.name;
            rva = static_cast<uint32_t>(address - r.base);
            return true;
        }
    }
    return false;
}

bool Loader::owns_module_handle(void* module_handle) const {
    if (!module_handle) return false;
    std::lock_guard lock(mutex_);
    for (const Module* m : order_)
        if (m->base == module_handle) return true;
    return false;
}

// x64 has no frame-pointer chain to fall back on: without its .pdata registered, an exception raised in (or passing
// through) a mapped image can't be dispatched past that frame, and the process dies. Images the system loader maps
// are found through its own module list; ours are VirtualAlloc'd, so they are registered as dynamic function tables.
//
// The table is checked before it is handed over, because RtlLookupFunctionEntry trusts it: it binary-searches the
// entries by BeginAddress and follows UnwindData without bounds checks. Measured over 3,780 x64 images in
// System32 (5,023,071 entries) and the package's FidelityFX DLLs: every table is sorted and non-overlapping and in
// bounds, and one shipped image (symcryptk.dll) has a zero-length entry (BeginAddress == EndAddress), so that is
// allowed. No entry in that set uses the indirect form (UnwindData bit 0).
void Loader::register_unwind(Module& m) {
    auto* nt = at<IMAGE_NT_HEADERS64>(m.base, reinterpret_cast<IMAGE_DOS_HEADER*>(m.base)->e_lfanew);
    const auto dir = directory(nt, IMAGE_DIRECTORY_ENTRY_EXCEPTION);
    // Like the system loader, a trailing partial entry is ignored.
    const uint32_t count = dir.Size / sizeof(RUNTIME_FUNCTION);
    if (!count) return;
    require_range(m.name, dir.VirtualAddress, static_cast<uint64_t>(count) * sizeof(RUNTIME_FUNCTION), m.size,
                  "the exception directory");
    auto* table = at<RUNTIME_FUNCTION>(m.base, dir.VirtualAddress);
    auto reject = [&](uint32_t i, const char* why) {
        throw std::runtime_error(m.name + ": unwind entry " + std::to_string(i) + " " + why);
    };
    uint32_t previous_end = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const RUNTIME_FUNCTION& f = table[i];
        if (f.EndAddress < f.BeginAddress || f.EndAddress > m.size) reject(i, "covers code outside the image");
        if (f.BeginAddress < previous_end) reject(i, "is out of order or overlaps the one before it");
        // UNWIND_INFO starts with a 4-byte header; its codes and handler data are the unwinder's to read.
        const uint32_t info = f.UnwindData & ~1u;
        if (info > m.size || m.size - info < 4) reject(i, "has unwind data outside the image");
        previous_end = f.EndAddress;
    }
    if (!RtlAddFunctionTable(table, count, reinterpret_cast<DWORD64>(m.base)))
        throw std::runtime_error(m.name + ": RtlAddFunctionTable failed");
    m.function_table = table;
    m.function_count = count;
}

void Loader::initialise() {
    // Not order_: map_image pushes a module there before bind_imports maps its dependencies, so order_ lists an
    // importer ahead of the DLLs it imports. init_order_ is filled once a module's imports are all loaded.
    for (auto* m : init_order_) initialise(*m);
}

void Loader::initialise(Module& m) {
    if (m.initialised) return;

    // TLS was installed by load(); initialise() only delivers the events, so no slot is allocated or written twice.
    run_tls_callbacks(m, DLL_PROCESS_ATTACH);

    // Only a DLL gets DllMain. An EXE's entry point has a different signature and is the caller's to run.
    auto* nt = at<IMAGE_NT_HEADERS64>(m.base, reinterpret_cast<IMAGE_DOS_HEADER*>(m.base)->e_lfanew);
    if ((nt->FileHeader.Characteristics & IMAGE_FILE_DLL) && m.entry_rva) {
        auto entry = reinterpret_cast<BOOL(NTAPI*)(void*, DWORD, void*)>(m.base + m.entry_rva);
        // lpvReserved "is NULL for dynamic loads and non-NULL for static loads" (DllMain docs). What Windows points
        // it at is its own; a static load gets a CONTEXT captured here, so a DLL that reads through it reads memory.
        CONTEXT context{};
        if (!m.dynamic_load) RtlCaptureContext(&context);
        const BOOL ok = entry(m.base, DLL_PROCESS_ATTACH, m.dynamic_load ? nullptr : &context);
        if (startup_trace::enabled())
            startup_trace::event("dllmain", "\"module\":" + startup_trace::quote(m.name) +
                                                ",\"result\":" + std::to_string(ok));
        if (!ok) {
            set_initialised(m, true);  // it ran: a second DLL_PROCESS_ATTACH would be wrong
            throw std::runtime_error(m.name + ": DllMain returned FALSE");
        }
    }
    set_initialised(m, true);
}

// Under the TLS lock, which orders it against the thread events: they run a module's TLS callbacks only while this is
// true (from initialise() until ~Loader starts the process detach).
void Loader::set_initialised(Module& m, bool value) {
    ExclusiveTlsLock lock;
    m.initialised = value;
}

Module& Loader::map_image(const std::filesystem::path& path, const std::string& narrow_key) {
    auto file = read_file(path);
    const std::string name = path_utf8(path.filename());
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(file.data());
    if (file.size() < sizeof(IMAGE_DOS_HEADER) || dos->e_magic != IMAGE_DOS_SIGNATURE)
        throw std::runtime_error(path_utf8(path) + ": not an MZ image");
    if (dos->e_lfanew < 0 || static_cast<uint64_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > file.size())
        throw std::runtime_error(name + ": the NT headers are outside the file");
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
        throw std::runtime_error(name + ": not an x86-64 PE");
    if (nt->FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        throw std::runtime_error(name + ": not a PE32+ image");
    auto& oh = nt->OptionalHeader;
    const uint64_t sections = static_cast<uint64_t>(dos->e_lfanew) + 4 + sizeof(IMAGE_FILE_HEADER) +
                              nt->FileHeader.SizeOfOptionalHeader;
    const uint64_t headers_end =
        sections + static_cast<uint64_t>(nt->FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
    if (headers_end > file.size())
        throw std::runtime_error(name + ": the section table runs past the end of the file");
    if (!oh.SizeOfImage || oh.SizeOfHeaders < headers_end || oh.SizeOfHeaders > oh.SizeOfImage ||
        oh.SizeOfHeaders > file.size())
        throw std::runtime_error(name + ": SizeOfHeaders and SizeOfImage don't fit the headers and file");
    if (oh.AddressOfEntryPoint && oh.AddressOfEntryPoint >= oh.SizeOfImage)
        throw std::runtime_error(name + ": the entry point is outside the image");

    auto* m = new Module;
    m->name = name;
    m->narrow_key = narrow_key;
    m->path = path;
    m->preferred_base = oh.ImageBase;
    m->size = oh.SizeOfImage;
    m->entry_rva = oh.AddressOfEntryPoint;
    by_name_[module_key(path)] = m;
    order_.push_back(m);

    // Inside the range gta6rt reserved for this image before the process started (base_reservation.h), else as usual.
    m->base = static_cast<uint8_t*>(base_reservation::commit_reserved(oh.ImageBase, oh.SizeOfImage));
    if (!m->base)
        m->base = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(oh.ImageBase), oh.SizeOfImage,
                                                     MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!m->base) {  // whether the image may move is decided at the relocation step below
        m->base = static_cast<uint8_t*>(VirtualAlloc(nullptr, oh.SizeOfImage, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!m->base) throw std::runtime_error(m->name + ": VirtualAlloc failed");
    }
    {
        // From here an address inside the image names it. The memory stays until ~Loader, which unregisters the
        // lookup first.
        std::unique_lock lock(ranges_mutex_);
        ranges_.push_back({reinterpret_cast<uint64_t>(m->base), m->size, m->name});
    }

    std::memcpy(m->base, file.data(), oh.SizeOfHeaders);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (sec->VirtualAddress < oh.SizeOfHeaders)
            throw std::runtime_error(m->name + ": section " + std::to_string(i) + " overlaps the headers");
        uint32_t n = std::min(sec->SizeOfRawData, sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData);
        if (!n) continue;
        if (sec->PointerToRawData > file.size() || n > file.size() - sec->PointerToRawData)
            throw std::runtime_error(m->name + ": section " + std::to_string(i) + " reads past the end of the file");
        require_range(m->name, sec->VirtualAddress, n, m->size, "a section's data");
        std::memcpy(m->base + sec->VirtualAddress, file.data() + sec->PointerToRawData, n);
    }

    int64_t delta = reinterpret_cast<int64_t>(m->base) - static_cast<int64_t>(oh.ImageBase);
    // A moved image with a relocation table is fixed up, whatever IMAGE_FILE_RELOCS_STRIPPED says. Without a table,
    // the flag refuses the move; with the flag clear the image is moved and nothing is fixed up, so it is trusted to
    // hold no absolute addresses. The Windows 11 loader does all three (runtime/README.md); that move is logged so
    // it shows in traces. base_reservation::needed still keeps the main image's range free when it has no
    // relocation directory, so a main image is moved this way only where the range could not be had.
    const auto dir = directory(nt, IMAGE_DIRECTORY_ENTRY_BASERELOC);
    if (delta) m->relocated = true;
    if (delta && !dir.Size && (nt->FileHeader.Characteristics & IMAGE_FILE_RELOCS_STRIPPED))
        throw std::runtime_error(m->name + ": preferred base is taken and the image's relocations are stripped");
    if (delta && !dir.Size)
        std::fprintf(stderr, "[rt] loader: %s: preferred base %#llx is taken, moved to %p with no relocations\n",
                     m->name.c_str(), static_cast<unsigned long long>(oh.ImageBase), static_cast<void*>(m->base));
    if (delta && dir.Size) {
        require_range(m->name, dir.VirtualAddress, dir.Size, m->size, "the relocation table");
        auto* block = at<IMAGE_BASE_RELOCATION>(m->base, dir.VirtualAddress);
        auto* end = reinterpret_cast<uint8_t*>(block) + dir.Size;
        while (reinterpret_cast<uint8_t*>(block) < end) {
            const size_t remaining = static_cast<size_t>(end - reinterpret_cast<uint8_t*>(block));
            if (remaining < sizeof(IMAGE_BASE_RELOCATION) || block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION) ||
                block->SizeOfBlock > remaining)
                throw std::runtime_error(m->name + ": a relocation block doesn't fit the relocation table");
            auto* entries = reinterpret_cast<uint16_t*>(block + 1);
            size_t count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / 2;
            for (size_t i = 0; i < count; ++i) {
                uint16_t type = entries[i] >> 12, off = entries[i] & 0xFFF;
                if (type != IMAGE_REL_BASED_ABSOLUTE &&
                    static_cast<uint64_t>(block->VirtualAddress) + off < oh.SizeOfHeaders)
                    throw std::runtime_error(m->name + ": a relocation lands in the headers");
                if (type == IMAGE_REL_BASED_DIR64) {
                    require_range(m->name, static_cast<uint64_t>(block->VirtualAddress) + off, sizeof(uint64_t), m->size,
                                  "a relocation");
                    *at<uint64_t>(m->base, block->VirtualAddress + off) += delta;
                } else if (type == IMAGE_REL_BASED_HIGHLOW) {
                    // A 32-bit absolute address (code built /LARGEADDRESSAWARE:NO), which the x64 Windows loader
                    // also applies. Where the new address no longer fits in 32 bits, Windows would truncate it; the
                    // image would then jump or read through a wrong pointer, so the load is refused instead.
                    require_range(m->name, static_cast<uint64_t>(block->VirtualAddress) + off, sizeof(uint32_t),
                                  m->size, "a relocation");
                    auto* slot = at<uint32_t>(m->base, block->VirtualAddress + off);
                    const int64_t moved = static_cast<int64_t>(*slot) + delta;
                    if (moved < 0 || moved > 0xFFFFFFFFll)
                        throw std::runtime_error(m->name + ": a 32-bit (HIGHLOW) relocation does not fit at this base");
                    *slot = static_cast<uint32_t>(moved);
                } else if (type != IMAGE_REL_BASED_ABSOLUTE) {
                    throw std::runtime_error(m->name + ": unsupported relocation type " + std::to_string(type));
                }
            }
            block = reinterpret_cast<IMAGE_BASE_RELOCATION*>(reinterpret_cast<uint8_t*>(block) + block->SizeOfBlock);
        }
    }
    // The TLS directory and the exception directory hold virtual addresses, so both are read after relocation. TLS is
    // installed here, before bind_imports can return control to the caller and before any image code can run.
    initialize_tls(*m);
    parse_exports(*m);
    if (auto it = m->exports.find("__XMemAllocationHooks__"); it != m->exports.end()) {
        const auto offset = static_cast<uint64_t>(static_cast<uint8_t*>(it->second) - m->base);
        if (offset <= m->size && m->size - offset >= sizeof(shims::XMemAllocationHooks)) {
            shims::add_xmem_allocation_hooks(this, static_cast<const shims::XMemAllocationHooks*>(it->second));
        }
    }
    return *m;
}

// Static TLS for the thread that calls load(). The PE TLS directory only carries the template (Start..End) and the
// zero-fill size; it has no field naming the TLS block. The Windows loader allocates a block per module per thread and
// puts it in TEB->ThreadLocalStoragePointer[module index], which is what __declspec(thread) code reads via _tls_index.
// Only the OS loader can allocate that module index, so the loader reuses the index the OS assigned to
// gta6rt_tls_host.dll and maps a fresh clone of the helper for every additional simultaneous TLS module, each clone
// getting its own index. All kMaxMappedTlsModules slots can be live at once; a further module is rejected rather than
// silently sharing (and corrupting) a slot. An executable takes index 0 instead (kMainImageSlot), as on Windows.
void Loader::initialize_tls(Module& m) {
    auto* tls = tls_directory(m);
    if (!tls) return;
    // A present but all-zero directory is treated like an absent one.
    if (!tls->StartAddressOfRawData && !tls->EndAddressOfRawData && !tls->AddressOfIndex && !tls->AddressOfCallBacks &&
        !tls->SizeOfZeroFill)
        return;
    if (tls->EndAddressOfRawData < tls->StartAddressOfRawData)
        throw std::runtime_error(m.name + ": TLS template end precedes its start");
    uint64_t raw = tls->EndAddressOfRawData - tls->StartAddressOfRawData;
    uint64_t total = raw + tls->SizeOfZeroFill;
    if (total > 0xFFFFFFFFull) throw std::runtime_error(m.name + ": TLS block is too large");
    const uint32_t raw_rva =
        raw ? static_cast<uint32_t>(rva_in(m, tls->StartAddressOfRawData, "the TLS template")) : 0;
    if (raw) require_range(m.name, raw_rva, raw, m.size, "the TLS template");
    uint64_t index_rva = 0;
    if (tls->AddressOfIndex) {
        index_rva = rva_in(m, tls->AddressOfIndex, "the TLS index");
        require_range(m.name, index_rva, sizeof(uint32_t), m.size, "the TLS index");
        // The index is written below, and every later step reads the headers back from the mapping. An index at
        // RVA 0 would not be written at all (index_rva doubles as "no index").
        const auto* nt = at<IMAGE_NT_HEADERS64>(m.base, reinterpret_cast<IMAGE_DOS_HEADER*>(m.base)->e_lfanew);
        if (index_rva < nt->OptionalHeader.SizeOfHeaders)
            throw std::runtime_error(m.name + ": the TLS index lands in the headers");
    }

    if (!module_tls_slots()) throw std::runtime_error(m.name + ": this thread has no TLS array");

    auto* nt = at<IMAGE_NT_HEADERS64>(m.base, reinterpret_cast<IMAGE_DOS_HEADER*>(m.base)->e_lfanew);
    const bool executable = (nt->FileHeader.Characteristics & IMAGE_FILE_DLL) == 0;

    size_t slot = kTlsSlotCount;
    DWORD index = TLS_OUT_OF_INDEXES;
    {
        std::lock_guard alloc_lock(g_tls_alloc_mutex);
        if (executable) {
            if (g_index0.moved && g_tls_slot_owners[kMainImageSlot].load() == nullptr) {
                slot = kMainImageSlot;
                index = 0;
                if (!g_index0.traced && startup_trace::enabled()) {
                    g_index0.traced = true;
                    startup_trace::event("tls_index0", "\"owner\":" + startup_trace::quote(g_index0.owner) +
                                                           ",\"moved_to\":" + std::to_string(g_index0.home.index));
                }
            } else {
                // Mapped on as a DLL is: its code that reads _tls_index works, but its /GA code would read and write
                // the block in slot 0, which is the runtime's own TLS or the other executable's. gta6rt refuses to
                // run such an image (main.cpp); a Loader user must not run its code either.
                std::fprintf(stderr,
                             "[rt] loader: %s does not get module TLS index 0 (%s); its code built with /GA would use "
                             "another module's TLS\n",
                             m.name.c_str(), g_index0.moved ? "another mapped executable holds it" : g_index0.why);
            }
        }
        if (slot == kTlsSlotCount) {
            for (size_t i = 0; i < kMaxMappedTlsModules; ++i) {
                if (g_tls_slot_owners[i].load() == nullptr) {
                    slot = i;
                    break;
                }
            }
            if (slot >= kMaxMappedTlsModules)
                throw std::runtime_error(m.name + ": all " + std::to_string(kMaxMappedTlsModules) +
                                         " manually mapped static TLS slots are in use");

            // Load/clone gta6rt_tls_host.dll OUTSIDE ExclusiveTlsLock so LoadLibraryExW cannot deadlock with
            // on_host_thread_event (which runs under the Windows loader lock and takes SharedTlsLock).
            index = ensure_tls_host(slot).index;
            if (index == TLS_OUT_OF_INDEXES)
                throw std::runtime_error(m.name + ": gta6rt_tls_host.dll is missing or has no module TLS index");
            if (index >= kMaxModuleTlsSlots)
                throw std::runtime_error(m.name + ": reserved TLS index is out of range");
        }

        // The fields are complete before a thread event can see the owner.
        ExclusiveTlsLock lock;
        Module* expected = nullptr;
        if (!g_tls_slot_owners[slot].compare_exchange_strong(expected, &m))
            throw std::runtime_error(m.name + ": static TLS slot was claimed concurrently");

        m.tls_slot = static_cast<uint32_t>(slot);
        m.tls_generation = g_next_tls_generation.fetch_add(1, std::memory_order_relaxed);
        g_tls_slot_generations[slot].store(m.tls_generation);
        m.tls_index = index;
        m.tls_template = raw ? m.base + raw_rva : nullptr;
        m.tls_raw_size = static_cast<uint32_t>(raw);
        m.tls_zero_fill = tls->SizeOfZeroFill;
        m.tls_block_size = static_cast<uint32_t>(total);
        // The index lives in a writable section and protect_sections() has not run yet, so the write needs no
        // VirtualProtect dance.
        if (index_rva) *reinterpret_cast<uint32_t*>(m.base + index_rva) = index;
    }
    {
        // An EXE's code may read its TLS at index 0 directly (/GA); index 0 is gta6rt.exe's here (pe_loader.h,
        // tls_ga_slot). Patched before any of the image's code runs, TLS callbacks included.
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(m.base + dos->e_lfanew);
        if (!(nt->FileHeader.Characteristics & IMAGE_FILE_DLL) && total && index != 0 && ga_tls_patch_enabled()) {
            static const DWORD ga_slot = TlsAlloc();
            if (ga_slot == TLS_OUT_OF_INDEXES || ga_slot >= 64) {
                std::fprintf(stderr, "[rt] loader: %s: no TlsAlloc slot below 64 for its index-0 TLS reads\n",
                             m.name.c_str());
            } else {
                m.tls_ga_sites = patch_ga_tls_reads(m, ga_slot);
                if (m.tls_ga_sites) m.tls_ga_slot = ga_slot;
                std::fprintf(stderr,
                             "[rt] loader: %s: %zu index-0 TLS reads (gs:[58h]) now read its own TLS (TlsSlots[%lu]); "
                             "its module TLS index is %u\n",
                             m.name.c_str(), m.tls_ga_sites, static_cast<unsigned long>(ga_slot), index);
            }
        }
    }
    try {
        install_thread_block(m);
    } catch (...) {
        ExclusiveTlsLock lock;
        m.tls_index = kNoTlsIndex;
        m.tls_slot = kNoTlsIndex;
        m.tls_generation = 0;
        g_tls_slot_generations[slot].store(0);
        g_tls_slot_owners[slot].store(nullptr);
        throw;
    }
    // The loading thread's copy is the one the Module records (and ~Loader frees on that thread).
    if (g_thread_tls_slots[slot].module == &m && g_thread_tls_slots[slot].generation == m.tls_generation) {
        m.tls_block = g_thread_tls_slots[slot].block;
        m.tls_saved_block = g_thread_tls_slots[slot].saved;
    }
    // Threads started from here on get their copy from the helper DLL's DLL_THREAD_ATTACH; the ones already running
    // get it now.
    install_on_existing_threads(m);
}

void Loader::install_tls_for_thread(Module& m) { install_thread_block(m); }

void Loader::uninstall_tls_for_thread(Module& m) { uninstall_thread_block(m); }

void Loader::run_tls_callbacks(Module& m, uint32_t reason) { run_callbacks(m, reason, /*trace=*/true); }

void Loader::parse_exports(Module& m) {
    auto* nt = at<IMAGE_NT_HEADERS64>(m.base, reinterpret_cast<IMAGE_DOS_HEADER*>(m.base)->e_lfanew);
    const auto dir = directory(nt, IMAGE_DIRECTORY_ENTRY_EXPORT);
    if (!dir.Size) return;
    require_range(m.name, dir.VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY), m.size, "the export directory");
    auto* ed = at<IMAGE_EXPORT_DIRECTORY>(m.base, dir.VirtualAddress);
    require_range(m.name, ed->AddressOfFunctions, static_cast<uint64_t>(ed->NumberOfFunctions) * sizeof(uint32_t), m.size,
                  "the export address table");
    require_range(m.name, ed->AddressOfNames, static_cast<uint64_t>(ed->NumberOfNames) * sizeof(uint32_t), m.size,
                  "the export name table");
    require_range(m.name, ed->AddressOfNameOrdinals, static_cast<uint64_t>(ed->NumberOfNames) * sizeof(uint16_t), m.size,
                  "the export ordinal table");
    auto* funcs = at<uint32_t>(m.base, ed->AddressOfFunctions);
    auto* names = at<uint32_t>(m.base, ed->AddressOfNames);
    auto* ords = at<uint16_t>(m.base, ed->AddressOfNameOrdinals);
    // An export whose RVA lands inside the export directory is a forwarder string ("KERNEL32.Sleep",
    // "api-ms-win-core-file-l1-1-0.CreateFileW"), not code. Storing base + that RVA would hand callers a pointer
    // into the string, so forwarders are kept apart and followed when an import asks for one.
    const uint64_t dir_begin = dir.VirtualAddress, dir_end = dir_begin + dir.Size;
    auto add_export = [&](const std::string& key, uint32_t rva) {
        if (!rva) return;  // An unused address-table slot is absent even when a name refers to it.
        if (rva >= dir_begin && rva < dir_end)
            m.forwarders[key] = read_string(m.name, m.base, m.size, rva, "a forwarded export");
        else if (rva >= m.size)  // base + rva would point past the mapping, and a caller would jump there
            throw std::runtime_error(m.name + ": export " + key + " points outside the image");
        else
            m.exports[key] = m.base + rva;
    };
    for (uint32_t i = 0; i < ed->NumberOfFunctions; ++i)
        if (funcs[i]) add_export("#" + std::to_string(ed->Base + i), funcs[i]);
    for (uint32_t i = 0; i < ed->NumberOfNames; ++i) {
        if (ords[i] >= ed->NumberOfFunctions)
            throw std::runtime_error(m.name + ": an export name points past the address table");
        add_export(read_string(m.name, m.base, m.size, names[i], "an export name"), funcs[ords[i]]);
    }
}

// Follows a forwarder string to the module and export it names. Bundled modules are searched first, with or without
// the extension, because a forwarder may point at another mapped module; system DLLs go through LoadLibraryEx, whose
// own GetProcAddress already follows its forwarders. Errors are "not found": the caller decides what to bind.
void* Loader::resolve_forwarder(const std::string& spec, Binding& binding, int depth) {
    if (depth >= 8) return nullptr;  // a longer chain is a loop between modules, not a re-export
    auto dot = spec.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= spec.size()) return nullptr;
    const std::string module = spec.substr(0, dot), symbol = spec.substr(dot + 1);

    // A forwarder can name a module the loader shims. Shims take precedence over a bundled or host copy, exactly as
    // they do for a direct import in resolve(); without this, a forwarded xmem or d3d12_xs call becomes a failure thunk.
    std::string shim_key = lower(module);
    if (shim_key.size() < 4 || shim_key.compare(shim_key.size() - 4, 4, ".dll") != 0) shim_key += ".dll";
    if (void* p = shims::find(shim_key, symbol)) {
        binding = Binding::Shim;
        return p;
    }

    const bool by_ordinal = symbol.size() > 1 && symbol[0] == '#';
    uint32_t ordinal = 0;
    if (by_ordinal) {
        for (size_t i = 1; i < symbol.size(); ++i) {
            if (symbol[i] < '0' || symbol[i] > '9') return nullptr;
            ordinal = ordinal * 10 + static_cast<uint32_t>(symbol[i] - '0');
            if (ordinal > 0xFFFF) return nullptr;  // checked after the digit, so "#65537" can't truncate to 1
        }
    }

    // shim_key, not the bare module name: forwarder strings usually leave out ".dll" ("MSVCP140.?foo"), and the host
    // CRT copies are named with it, so a forwarded CRT symbol must skip the bundled Xbox build as a direct import does.
    const std::filesystem::path candidates[] = {game_dir_ / module, game_dir_ / (module + ".dll")};
    for (const auto& candidate : candidates) {
        std::error_code ec;
        if (prefer_host_copy(shim_key) || !std::filesystem::exists(candidate, ec)) continue;
        Module& dep = load(candidate);
        if (auto it = dep.exports.find(symbol); it != dep.exports.end()) {
            binding = Binding::Module;
            return it->second;
        }
        if (auto it = dep.forwarders.find(symbol); it != dep.forwarders.end())
            return resolve_forwarder(it->second, binding, depth + 1);
        return nullptr;  // the bundled module exists and does not export it; don't look for a host namesake
    }

    HMODULE h = LoadLibraryExA(module.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!h) return nullptr;
    if (prefer_host_copy(shim_key)) redirect_pc_to_file_header(h);
    FARPROC p = GetProcAddress(h, by_ordinal ? MAKEINTRESOURCEA(static_cast<WORD>(ordinal)) : symbol.c_str());
    if (!p) return nullptr;
    if (prefer_host_copy(shim_key)) redirect_pc_to_file_header_at(reinterpret_cast<const void*>(p));
    binding = Binding::Host;
    return reinterpret_cast<void*>(p);
}

bool Loader::resolve_compat(const std::string& key, const std::string& dll, const std::string& name, uint16_t ordinal,
                            bool by_ordinal, void*& out) {
    out = nullptr;
    if (compat_dir_.empty() || key.empty() || key.find_first_of("/\\:") != std::string::npos) return false;
    HMODULE h = nullptr;
    if (auto it = compat_modules_.find(key); it != compat_modules_.end()) {
        h = static_cast<HMODULE>(it->second);
    } else {
        const std::filesystem::path candidate = compat_dir_ / std::filesystem::path(dll);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(candidate, ec)) return false;
        // The full path, with the compat folder on the DLL's own search path for its dependencies; never the game
        // folder. Windows maps it, so its CRT, TLS and SEH work as for any host DLL.
        // A file Windows cannot map (truncated, wrong machine, not a PE image) otherwise raises a modal "Bad Image"
        // box and LoadLibraryExW does not return until someone clicks it: a headless run would hang on a failure
        // that is reported right below anyway. The mode is this thread's, and goes back as it was.
        DWORD previous_mode = 0;
        const bool mode_set = SetThreadErrorMode(GetThreadErrorMode() | SEM_FAILCRITICALERRORS, &previous_mode) != 0;
        h = LoadLibraryExW(candidate.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        const DWORD load_error = h ? ERROR_SUCCESS : GetLastError();
        if (mode_set) SetThreadErrorMode(previous_mode, nullptr);
        if (!h)
            throw std::runtime_error("LEONIDA_COMPAT_DIR: " + path_utf8(candidate) + " exists but Windows could not " +
                                     "load it (error " + std::to_string(load_error) + ")");
        compat_modules_.emplace(key, h);
        std::fprintf(stderr, "[rt] compat: %s -> %s\n", dll.c_str(), path_utf8(candidate).c_str());
    }
    out = reinterpret_cast<void*>(GetProcAddress(h, by_ordinal ? MAKEINTRESOURCEA(ordinal) : name.c_str()));
    return true;
}

void* Loader::resolve(Module& m, const std::string& dll, const std::string& name, uint16_t ordinal, bool by_ordinal) {
    std::string key = lower(dll);
    std::string sym = by_ordinal ? "#" + std::to_string(ordinal) : name;

    // Opt-in compat modules come first: a DLL the player put in LEONIDA_COMPAT_DIR owns all of its imports, so one
    // console module is never served half by a shim and half by the drop-in (they would not share state).
    if (void* p = nullptr; resolve_compat(key, dll, name, ordinal, by_ordinal, p)) {
        m.imports.push_back({dll, sym, p ? Binding::Host : Binding::Unimplemented});
        return p ? p : shims::make_unimplemented_thunk(dll, sym);
    }

    if (void* p = shims::find(key, sym)) {
        m.imports.push_back({dll, sym, Binding::Shim});
        return p;
    }
    // Bundled game module (an Xbox build that ships beside the game)?
    auto bundled = game_dir_ / dll;
    if (!prefer_host_copy(key) && std::filesystem::exists(bundled)) {
        Module& dep = load(bundled);
        if (auto it = dep.exports.find(sym); it != dep.exports.end()) {
            m.imports.push_back({dll, sym, Binding::Module});
            return it->second;
        }
        if (auto it = dep.forwarders.find(sym); it != dep.forwarders.end()) {
            Binding binding = Binding::Unimplemented;
            if (void* p = resolve_forwarder(it->second, binding, 0)) {
                m.imports.push_back({dll, sym, binding});
                return p;
            }
        }
    }
    // Host Windows: system DLLs and API sets. LOAD_LIBRARY_SEARCH_SYSTEM32 keeps the game folder out of the search.
    if (HMODULE h = LoadLibraryExA(dll.c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (prefer_host_copy(key)) redirect_pc_to_file_header(h);  // before any mapped code can throw through it
        FARPROC p = GetProcAddress(h, by_ordinal ? MAKEINTRESOURCEA(ordinal) : name.c_str());
        if (p) {
            if (prefer_host_copy(key)) redirect_pc_to_file_header_at(reinterpret_cast<const void*>(p));
            m.imports.push_back({dll, sym, Binding::Host});
            return reinterpret_cast<void*>(p);
        }
    }
    m.imports.push_back({dll, sym, Binding::Unimplemented});
    return shims::make_unimplemented_thunk(dll, sym);
}

void Loader::bind_imports(Module& m) {
    auto* nt = at<IMAGE_NT_HEADERS64>(m.base, reinterpret_cast<IMAGE_DOS_HEADER*>(m.base)->e_lfanew);
    // Read before the first address is written: an entry in the headers would replace what the steps after this
    // one read back from the mapping (e_lfanew, the section table), so it is refused like a section there.
    const uint64_t headers_end = nt->OptionalHeader.SizeOfHeaders;

    // Thunk tables have no length field: they end at a zero entry, so each step is checked against the image.
    // `delayed`: a delay import that binds to an unimplemented thunk is traced. Windows' delay-load helper would raise
    // 0xC06D007E (module) or 0xC06D007F (procedure) at its first call, which a title can catch for a fallback; the
    // thunk returns 0 instead, so the trace names each such import before any image code runs.
    auto bind_thunks = [&](const std::string& dll, uint64_t lookup_rva, uint64_t iat_rva, bool delayed) {
        for (;; lookup_rva += sizeof(IMAGE_THUNK_DATA64), iat_rva += sizeof(IMAGE_THUNK_DATA64)) {
            require_range(m.name, lookup_rva, sizeof(IMAGE_THUNK_DATA64), m.size, "an import lookup table");
            require_range(m.name, iat_rva, sizeof(IMAGE_THUNK_DATA64), m.size, "an import address table");
            auto* lookup = at<IMAGE_THUNK_DATA64>(m.base, static_cast<uint32_t>(lookup_rva));
            if (!lookup->u1.AddressOfData) return;
            bool by_ord = IMAGE_SNAP_BY_ORDINAL64(lookup->u1.Ordinal);
            uint16_t ord = by_ord ? (uint16_t)IMAGE_ORDINAL64(lookup->u1.Ordinal) : 0;
            std::string name;
            if (!by_ord) {
                require_range(m.name, lookup->u1.AddressOfData, sizeof(uint16_t), m.size, "an import name");
                uint32_t hint = static_cast<uint32_t>(lookup->u1.AddressOfData);
                name = read_string(m.name, m.base, m.size, hint + sizeof(uint16_t), "an import name");
            }
            if (iat_rva < headers_end)
                throw std::runtime_error(m.name + ": an import address table lands in the headers");
            at<IMAGE_THUNK_DATA64>(m.base, static_cast<uint32_t>(iat_rva))->u1.Function =
                reinterpret_cast<uint64_t>(resolve(m, dll, name, ord, by_ord));
            if (delayed && startup_trace::enabled() && m.imports.back().binding == Binding::Unimplemented)
                startup_trace::event("delay_import_unimplemented",
                                     "\"module\":" + startup_trace::quote(m.name) + ",\"import\":" +
                                         startup_trace::quote(dll + "!" + m.imports.back().name));
        }
    };

    const auto dir = directory(nt, IMAGE_DIRECTORY_ENTRY_IMPORT);
    if (dir.Size) {
        for (uint64_t i = 0;; ++i) {
            uint64_t rva = dir.VirtualAddress + i * sizeof(IMAGE_IMPORT_DESCRIPTOR);
            require_range(m.name, rva, sizeof(IMAGE_IMPORT_DESCRIPTOR), m.size, "the import table");
            auto* d = at<IMAGE_IMPORT_DESCRIPTOR>(m.base, static_cast<uint32_t>(rva));
            if (!d->Name) break;
            std::string dll = read_string(m.name, m.base, m.size, d->Name, "an imported DLL name");
            uint32_t lookup = d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk;
            bind_thunks(dll, lookup, d->FirstThunk, false);
        }
    }
    // Delay imports are bound eagerly: simpler, and the full import surface shows up in --dry-run.
    const auto ddir = directory(nt, IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT);
    if (ddir.Size) {
        for (uint64_t i = 0;; ++i) {
            uint64_t rva = ddir.VirtualAddress + i * sizeof(IMAGE_DELAYLOAD_DESCRIPTOR);
            require_range(m.name, rva, sizeof(IMAGE_DELAYLOAD_DESCRIPTOR), m.size, "the delay-import table");
            auto* d = at<IMAGE_DELAYLOAD_DESCRIPTOR>(m.base, static_cast<uint32_t>(rva));
            if (!d->DllNameRVA) break;
            // x64 descriptors always carry dlattrRva (0x1). Without it the fields would be 32-bit virtual
            // addresses, which cannot express a 64-bit image base, so the descriptor is not usable as written.
            if (!d->Attributes.RvaBased)
                throw std::runtime_error(m.name + ": a delay-import descriptor without the RVA attribute");
            if (!d->ImportAddressTableRVA)
                throw std::runtime_error(m.name + ": a delay-import descriptor has no address table");
            std::string dll = read_string(m.name, m.base, m.size, d->DllNameRVA, "a delay-imported DLL name");
            // Without a name table the IAT still holds delay thunks, not import names, so there is no symbol to bind
            // eagerly. Every MSVC delay descriptor and every System32 DLL has one; reject the form instead of reading
            // thunk addresses as name RVAs.
            if (!d->ImportNameTableRVA)
                throw std::runtime_error(m.name + ": a delay-import descriptor has no name table");
            bind_thunks(dll, d->ImportNameTableRVA, d->ImportAddressTableRVA, true);
        }
    }
}

void Loader::protect_sections(Module& m) {
    auto* nt = at<IMAGE_NT_HEADERS64>(m.base, reinterpret_cast<IMAGE_DOS_HEADER*>(m.base)->e_lfanew);
    DWORD old;
    // On a CPU without CLZERO, the image's CLZERO loops run from stubs instead of taking a #UD per cache line
    // (clzero_patch.h). The sections are still writable here, and none of the image's code has run.
    static const bool clzero_missing = !rt::cpu_emulation::detect_host().clzero;
    if (clzero_missing && rt::clzero_patch::enabled()) {
        std::vector<std::pair<uint8_t*, size_t>> code;
        const IMAGE_SECTION_HEADER* cs = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++cs) {
            if (!(cs->Characteristics & IMAGE_SCN_MEM_EXECUTE) || cs->VirtualAddress >= m.size) continue;
            const uint64_t size = std::min<uint64_t>(std::min(cs->Misc.VirtualSize, cs->SizeOfRawData),
                                                     m.size - cs->VirtualAddress);
            if (size) code.push_back({m.base + cs->VirtualAddress, static_cast<size_t>(size)});
        }
        const auto patched = rt::clzero_patch::patch(code, uint64_t(m.base), uint64_t(m.base) + m.size);
        if (patched.sites)
            std::fprintf(stderr,
                         "[rt] loader: %s: %zu CLZERO loops run from stubs at %p (%zu bytes), not as a #UD per line\n",
                         m.name.c_str(), patched.sites, static_cast<void*>(patched.stubs), patched.stub_bytes);
    }
    // Milestone O7: Apply byte-checked in-memory patch table if patches.tbl is present
    const auto patches_path = std::filesystem::path("patches.tbl");
    leonida::offline::apply_patch_table(patches_path, m.base, m.size, nullptr);
    VirtualProtect(m.base, nt->OptionalHeader.SizeOfHeaders, PAGE_READONLY, &old);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        bool x = sec->Characteristics & IMAGE_SCN_MEM_EXECUTE, w = sec->Characteristics & IMAGE_SCN_MEM_WRITE;
        DWORD prot = x ? (w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ) : (w ? PAGE_READWRITE : PAGE_READONLY);
        uint64_t size = sec->Misc.VirtualSize ? sec->Misc.VirtualSize : sec->SizeOfRawData;
        if (sec->VirtualAddress >= m.size) continue;
        size = std::min<uint64_t>(size, m.size - sec->VirtualAddress);
        if (size) VirtualProtect(m.base + sec->VirtualAddress, static_cast<SIZE_T>(size), prot, &old);
    }
    FlushInstructionCache(GetCurrentProcess(), m.base, m.size);
}

}  // namespace rt
