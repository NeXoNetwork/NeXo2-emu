#include "fs.hpp"
#include "hle/kernel.hpp"
#include "common/logger.hpp"
#include <algorithm>
#include <cstring>

namespace NeXo2::HLE {
namespace fs = std::filesystem;
using Common::Logger;

namespace {
// Resultados del modulo FS (modulo 2)
constexpr u32 RESULT_PATH_NOT_FOUND      = 0x202;     // desc 1
constexpr u32 RESULT_PATH_ALREADY_EXISTS = 0x402;     // desc 2
constexpr u32 RESULT_INVALID_PATH        = 0x2EE202;  // desc 6001
constexpr u32 RESULT_INVALID_OFFSET      = 0x2F5A02;  // desc 6061
constexpr u32 RESULT_NOT_PERMITTED       = 0x2F5C02;  // desc 6062 (escribir en archivo de solo lectura)

constexpr size_t MAX_PATH = 0x301;

// Entrada de IDirectory::Read (FsDirectoryEntry, 0x310 bytes)
struct DirectoryEntry {
    char name[MAX_PATH];
    u8   pad0[3];
    s8   type;        // 0 = carpeta, 1 = archivo
    u8   pad1[3];
    s64  file_size;
};
static_assert(sizeof(DirectoryEntry) == 0x310);

u32 ErrorFor(const std::error_code& ec) {
    if (ec == std::errc::no_such_file_or_directory) return RESULT_PATH_NOT_FOUND;
    if (ec == std::errc::file_exists)               return RESULT_PATH_ALREADY_EXISTS;
    return RESULT_INVALID_PATH;
}

fs::path FromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }
std::string ToUtf8(const fs::path& p) { auto u = p.u8string(); return std::string(u.begin(), u.end()); }
} // namespace

fs::path ResolveGuestPath(const fs::path& root, const std::string& guest) {
    // Partimos la ruta por '/' y rechazamos cualquier ".." (nunca se sale de la SD)
    fs::path result = root;
    size_t pos = 0;
    while (pos <= guest.size()) {
        size_t next = guest.find('/', pos);
        if (next == std::string::npos) next = guest.size();
        const std::string part = guest.substr(pos, next - pos);
        pos = next + 1;
        if (part.empty() || part == ".") continue;
        if (part == ".." || part.find('\\') != std::string::npos || part.find(':') != std::string::npos) return {};
        result /= FromUtf8(part);
    }
    return result;
}

// ============================================================================
//  fsp-srv
// ============================================================================

FileSystemProxy::FileSystemProxy() : ServiceObject("fsp-srv") {
    // SetCurrentProcess(pid, u64): el programa se presenta
    RegisterStub(1, "SetCurrentProcess");
    // OpenSdCardFileSystem -> IFileSystem de la tarjeta SD ("sdmc:/")
    RegisterCommand(18, "OpenSdCardFileSystem", [](IpcContext& ctx) {
        std::error_code ec;
        fs::create_directories(ctx.GetKernel().GetSdmcRoot(), ec);
        ctx.PushInterface(std::make_shared<FileSystem>("IFileSystem (sdmc)"));
        ctx.SetResult(Result::Success);
    });
    RegisterStub(1003, "DisableAutoSaveDataCreation");
}

// ============================================================================
//  IFileSystem
// ============================================================================

bool FileSystem::HostPath(IpcContext& ctx, size_t index, fs::path& out) {
    const auto raw = ctx.ReadBuffer(index);
    std::string guest(reinterpret_cast<const char*>(raw.data()), raw.size());
    guest = guest.substr(0, guest.find('\0'));
    out = ResolveGuestPath(ctx.GetKernel().GetSdmcRoot(), guest);
    if (out.empty()) {
        Logger::Log(Logger::Level::Warning, "[fs] Ruta rechazada: " + guest);
        ctx.SetResult(RESULT_INVALID_PATH);
        return false;
    }
    return true;
}

std::shared_ptr<const std::vector<u8>> FileSystem::SelfNro(IpcContext& ctx, size_t index) {
    const auto& kernel = ctx.GetKernel();
    if (kernel.GetSelfNroPath().empty()) return nullptr;
    const auto raw = ctx.ReadBuffer(index);
    std::string guest(reinterpret_cast<const char*>(raw.data()), raw.size());
    guest = guest.substr(0, guest.find('\0'));
    if (guest != kernel.GetSelfNroPath()) return nullptr;
    const fs::path host = ResolveGuestPath(kernel.GetSdmcRoot(), guest);
    if (!host.empty() && fs::is_regular_file(host)) return nullptr;   // el de verdad tiene prioridad
    return kernel.GetSelfNro();
}

FileSystem::FileSystem(std::string name) : ServiceObject(std::move(name)) {
    // 0 CreateFile(u32 opcion, s64 tamano, ruta)
    RegisterCommand(0, "CreateFile", [this](IpcContext& ctx) {
        ctx.Pop<u32>(); ctx.Pop<u32>();
        const s64 size = ctx.Pop<s64>();
        fs::path p;
        if (!HostPath(ctx, 0, p)) return;
        if (fs::exists(p)) { ctx.SetResult(RESULT_PATH_ALREADY_EXISTS); return; }
        if (!fs::is_directory(p.parent_path())) { ctx.SetResult(RESULT_PATH_NOT_FOUND); return; }
        { std::ofstream f(p, std::ios::binary); }
        std::error_code ec;
        fs::resize_file(p, static_cast<u64>(std::max<s64>(size, 0)), ec);
        ctx.SetResult(ec ? ErrorFor(ec) : Result::Success);
    });
    // 1 DeleteFile(ruta)
    RegisterCommand(1, "DeleteFile", [this](IpcContext& ctx) {
        fs::path p;
        if (!HostPath(ctx, 0, p)) return;
        if (!fs::is_regular_file(p)) { ctx.SetResult(RESULT_PATH_NOT_FOUND); return; }
        std::error_code ec;
        fs::remove(p, ec);
        ctx.SetResult(ec ? ErrorFor(ec) : Result::Success);
    });
    // 2 CreateDirectory(ruta)
    RegisterCommand(2, "CreateDirectory", [this](IpcContext& ctx) {
        fs::path p;
        if (!HostPath(ctx, 0, p)) return;
        if (fs::exists(p)) { ctx.SetResult(RESULT_PATH_ALREADY_EXISTS); return; }
        if (!fs::is_directory(p.parent_path())) { ctx.SetResult(RESULT_PATH_NOT_FOUND); return; }
        std::error_code ec;
        fs::create_directory(p, ec);
        ctx.SetResult(ec ? ErrorFor(ec) : Result::Success);
    });
    // 3 DeleteDirectory(ruta) (tiene que estar vacia) / 4 DeleteDirectoryRecursively(ruta)
    auto delete_dir = [this](bool recursive) {
        return [this, recursive](IpcContext& ctx) {
            fs::path p;
            if (!HostPath(ctx, 0, p)) return;
            if (!fs::is_directory(p)) { ctx.SetResult(RESULT_PATH_NOT_FOUND); return; }
            std::error_code ec;
            if (recursive) fs::remove_all(p, ec); else fs::remove(p, ec);
            ctx.SetResult(ec ? ErrorFor(ec) : Result::Success);
        };
    };
    RegisterCommand(3, "DeleteDirectory", delete_dir(false));
    RegisterCommand(4, "DeleteDirectoryRecursively", delete_dir(true));
    // 5 RenameFile(origen, destino) / 6 RenameDirectory(origen, destino)
    auto rename = [this](IpcContext& ctx) {
        fs::path from, to;
        if (!HostPath(ctx, 0, from) || !HostPath(ctx, 1, to)) return;
        if (!fs::exists(from)) { ctx.SetResult(RESULT_PATH_NOT_FOUND); return; }
        if (fs::exists(to))    { ctx.SetResult(RESULT_PATH_ALREADY_EXISTS); return; }
        std::error_code ec;
        fs::rename(from, to, ec);
        ctx.SetResult(ec ? ErrorFor(ec) : Result::Success);
    };
    RegisterCommand(5, "RenameFile", rename);
    RegisterCommand(6, "RenameDirectory", rename);
    // 7 GetEntryType(ruta) -> u32 (0 = carpeta, 1 = archivo)
    RegisterCommand(7, "GetEntryType", [this](IpcContext& ctx) {
        if (SelfNro(ctx, 0)) { ctx.Push<u32>(1); ctx.SetResult(Result::Success); return; }
        fs::path p;
        if (!HostPath(ctx, 0, p)) return;
        if (fs::is_directory(p))         { ctx.Push<u32>(0); ctx.SetResult(Result::Success); }
        else if (fs::is_regular_file(p)) { ctx.Push<u32>(1); ctx.SetResult(Result::Success); }
        else ctx.SetResult(RESULT_PATH_NOT_FOUND);
    });
    // 8 OpenFile(u32 modo, ruta) -> IFile
    RegisterCommand(8, "OpenFile", [this](IpcContext& ctx) {
        const u32 mode = ctx.Pop<u32>();
        if (auto self = SelfNro(ctx, 0)) {
            if (mode & 2) { ctx.SetResult(RESULT_NOT_PERMITTED); return; }
            Logger::Log(Logger::Level::Info, "[fs] OpenFile(" + ctx.GetKernel().GetSelfNroPath() + "): el propio .nro");
            ctx.PushInterface(std::make_shared<MemoryFileObject>(self));
            ctx.SetResult(Result::Success);
            return;
        }
        fs::path p;
        if (!HostPath(ctx, 0, p)) return;
        if (!fs::is_regular_file(p)) { ctx.SetResult(RESULT_PATH_NOT_FOUND); return; }
        Logger::Log(Logger::Level::Info, "[fs] OpenFile(" + ToUtf8(p) + ", modo " + std::to_string(mode) + ")");
        ctx.PushInterface(std::make_shared<FileObject>(p, mode));
        ctx.SetResult(Result::Success);
    });
    // 9 OpenDirectory(u32 filtro, ruta) -> IDirectory
    RegisterCommand(9, "OpenDirectory", [this](IpcContext& ctx) {
        const u32 filter = ctx.Pop<u32>();
        fs::path p;
        if (!HostPath(ctx, 0, p)) return;
        if (!fs::is_directory(p)) { ctx.SetResult(RESULT_PATH_NOT_FOUND); return; }
        ctx.PushInterface(std::make_shared<DirectoryObject>(p, filter));
        ctx.SetResult(Result::Success);
    });
    RegisterStub(10, "Commit");
    // 11 GetFreeSpaceSize / 12 GetTotalSpaceSize: una SD de 32 GiB con 16 libres
    RegisterCommand(11, "GetFreeSpaceSize",  [](IpcContext& ctx) { ctx.Push<u64>(16ull << 30); ctx.SetResult(Result::Success); });
    RegisterCommand(12, "GetTotalSpaceSize", [](IpcContext& ctx) { ctx.Push<u64>(32ull << 30); ctx.SetResult(Result::Success); });
    // 13 CleanDirectoryRecursively(ruta): vacia la carpeta pero no la borra
    RegisterCommand(13, "CleanDirectoryRecursively", [this](IpcContext& ctx) {
        fs::path p;
        if (!HostPath(ctx, 0, p)) return;
        if (!fs::is_directory(p)) { ctx.SetResult(RESULT_PATH_NOT_FOUND); return; }
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(p, ec)) fs::remove_all(e.path(), ec);
        ctx.SetResult(ec ? ErrorFor(ec) : Result::Success);
    });
}

// ============================================================================
//  IFile
// ============================================================================

FileObject::FileObject(fs::path path, u32 mode)
    : ServiceObject("IFile"), m_path(std::move(path)), m_mode(mode) {
    auto flags = std::ios::binary | std::ios::in;
    if (m_mode & 2) flags |= std::ios::out;
    m_file.open(m_path, flags);

    // 0 Read(u32 opcion, s64 offset, u64 tamano) -> u64 leidos; datos en el buffer B
    RegisterCommand(0, "Read", [this](IpcContext& ctx) {
        ctx.Pop<u32>(); ctx.Pop<u32>();
        const s64 offset = ctx.Pop<s64>();
        const u64 size = std::min<u64>(ctx.Pop<u64>(), ctx.GetWriteBufferSize(0));
        if (offset < 0) { ctx.SetResult(RESULT_INVALID_OFFSET); return; }
        std::vector<u8> data(static_cast<size_t>(size));
        m_file.clear();
        m_file.seekg(offset);
        m_file.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
        const u64 got = static_cast<u64>(std::max<std::streamsize>(m_file.gcount(), 0));
        ctx.WriteBuffer(data.data(), static_cast<size_t>(got), 0);
        ctx.Push<u64>(got);
        ctx.SetResult(Result::Success);
    });
    // 1 Write(u32 opcion, s64 offset, u64 tamano); datos en el buffer A
    RegisterCommand(1, "Write", [this](IpcContext& ctx) {
        ctx.Pop<u32>(); ctx.Pop<u32>();
        const s64 offset = ctx.Pop<s64>();
        const u64 size = ctx.Pop<u64>();
        if (!(m_mode & 2)) { ctx.SetResult(RESULT_NOT_PERMITTED); return; }
        if (offset < 0)    { ctx.SetResult(RESULT_INVALID_OFFSET); return; }
        auto data = ctx.ReadBuffer(0);
        data.resize(std::min<size_t>(data.size(), static_cast<size_t>(size)));
        m_file.clear();
        m_file.seekp(offset);
        m_file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (m_file.fail()) { ctx.SetResult(RESULT_INVALID_OFFSET); return; }
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(2, "Flush", [this](IpcContext& ctx) { m_file.flush(); ctx.SetResult(Result::Success); });
    // 3 SetSize(s64)
    RegisterCommand(3, "SetSize", [this](IpcContext& ctx) {
        const s64 size = ctx.Pop<s64>();
        if (!(m_mode & 2)) { ctx.SetResult(RESULT_NOT_PERMITTED); return; }
        m_file.flush();
        std::error_code ec;
        fs::resize_file(m_path, static_cast<u64>(std::max<s64>(size, 0)), ec);
        ctx.SetResult(ec ? ErrorFor(ec) : Result::Success);
    });
    // 4 GetSize -> s64
    RegisterCommand(4, "GetSize", [this](IpcContext& ctx) {
        m_file.flush();
        std::error_code ec;
        const auto size = fs::file_size(m_path, ec);
        ctx.Push<s64>(ec ? 0 : static_cast<s64>(size));
        ctx.SetResult(ec ? ErrorFor(ec) : Result::Success);
    });
}

MemoryFileObject::MemoryFileObject(std::shared_ptr<const std::vector<u8>> data)
    : ServiceObject("IFile (nro)"), m_data(std::move(data)) {
    RegisterCommand(0, "Read", [this](IpcContext& ctx) {
        ctx.Pop<u32>(); ctx.Pop<u32>();
        const s64 offset = ctx.Pop<s64>();
        const u64 size = std::min<u64>(ctx.Pop<u64>(), ctx.GetWriteBufferSize(0));
        if (offset < 0) { ctx.SetResult(RESULT_INVALID_OFFSET); return; }
        const u64 start = std::min<u64>(u64(offset), m_data->size());
        const u64 got = std::min<u64>(size, m_data->size() - start);
        if (got) ctx.WriteBuffer(m_data->data() + start, static_cast<size_t>(got), 0);
        ctx.Push<u64>(got);
        ctx.SetResult(Result::Success);
    });
    RegisterCommand(1, "Write", [](IpcContext& ctx) { ctx.SetResult(RESULT_NOT_PERMITTED); });
    RegisterCommand(2, "Flush", [](IpcContext& ctx) { ctx.SetResult(Result::Success); });
    RegisterCommand(3, "SetSize", [](IpcContext& ctx) { ctx.SetResult(RESULT_NOT_PERMITTED); });
    RegisterCommand(4, "GetSize", [this](IpcContext& ctx) {
        ctx.Push<s64>(static_cast<s64>(m_data->size()));
        ctx.SetResult(Result::Success);
    });
}

// ============================================================================
//  IDirectory
// ============================================================================

DirectoryObject::DirectoryObject(const fs::path& path, u32 filter) : ServiceObject("IDirectory") {
    // filtro: bit 0 = carpetas, bit 1 = archivos
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(path, ec)) {
        const bool is_dir = e.is_directory(ec);
        if (is_dir ? !(filter & 1) : !(filter & 2)) continue;
        const std::string name = ToUtf8(e.path().filename());
        if (name.size() >= MAX_PATH) continue;
        m_entries.push_back({name, is_dir, is_dir ? 0 : static_cast<u64>(e.file_size(ec))});
    }
    std::sort(m_entries.begin(), m_entries.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });

    // 0 Read -> s64 cuantas; entradas en el buffer B
    RegisterCommand(0, "Read", [this](IpcContext& ctx) {
        const size_t room = static_cast<size_t>(ctx.GetWriteBufferSize(0) / sizeof(DirectoryEntry));
        std::vector<DirectoryEntry> out;
        while (out.size() < room && m_next < m_entries.size()) {
            const Entry& e = m_entries[m_next++];
            DirectoryEntry d{};
            std::memcpy(d.name, e.name.data(), e.name.size());
            d.type = e.is_dir ? 0 : 1;
            d.file_size = static_cast<s64>(e.size);
            out.push_back(d);
        }
        if (!out.empty()) ctx.WriteBuffer(out.data(), out.size() * sizeof(DirectoryEntry), 0);
        ctx.Push<s64>(static_cast<s64>(out.size()));
        ctx.SetResult(Result::Success);
    });
    // 1 GetEntryCount -> s64
    RegisterCommand(1, "GetEntryCount", [this](IpcContext& ctx) {
        ctx.Push<s64>(static_cast<s64>(m_entries.size()));
        ctx.SetResult(Result::Success);
    });
}

} // namespace NeXo2::HLE
