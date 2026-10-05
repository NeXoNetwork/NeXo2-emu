#include "fs.hpp"
#include "hle/kernel.hpp"

namespace NeXo2::HLE {

FileSystemProxy::FileSystemProxy() : ServiceObject("fsp-srv") {
    // SetCurrentProcess(pid, u64): el programa se presenta
    RegisterStub(1, "SetCurrentProcess");
    // OpenSdCardFileSystem -> IFileSystem de la tarjeta SD ("sdmc:/")
    RegisterCommand(18, "OpenSdCardFileSystem", [](IpcContext& ctx) {
        ctx.PushInterface(std::make_shared<FileSystem>("IFileSystem (sdmc)"));
        ctx.SetResult(Result::Success);
    });
    RegisterStub(1003, "DisableAutoSaveDataCreation");
}

} // namespace NeXo2::HLE
