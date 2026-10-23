#include "ps2_iop_host.h"

#include "ps2_host_input.h"
#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "Kernel/Stubs/SIF.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_cd_image.h"
#include "Kernel/Stubs/MemoryCard.h"
#include "Kernel/Syscalls/Common.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <utility>

#if !defined(_WIN32)
#include <sys/types.h>
#endif

PS2IopHostAdapter::CallScope::CallScope(PS2IopHostAdapter &owner, R5900Context *context, uint8_t *rdram)
    : m_lock(owner.m_callMutex),
      m_owner(&owner),
      m_previousContext(owner.m_activeContext),
      m_previousRdram(owner.m_activeRdram),
      m_previousToken(owner.m_activeToken)
{
    m_token = owner.m_nextToken++;
    if (m_token == 0)
    {
        m_token = owner.m_nextToken++;
    }
    owner.m_activeContext = context;
    owner.m_activeRdram = rdram;
    owner.m_activeToken = m_token;
}

PS2IopHostAdapter::CallScope::~CallScope()
{
    release();
}

PS2IopHostAdapter::CallScope::CallScope(CallScope &&other) noexcept
    : m_lock(std::move(other.m_lock)),
      m_owner(std::exchange(other.m_owner, nullptr)),
      m_previousContext(other.m_previousContext),
      m_previousRdram(other.m_previousRdram),
      m_previousToken(other.m_previousToken),
      m_token(other.m_token)
{
}

PS2IopHostAdapter::CallScope &PS2IopHostAdapter::CallScope::operator=(CallScope &&other) noexcept
{
    if (this != &other)
    {
        release();
        m_lock = std::move(other.m_lock);
        m_owner = std::exchange(other.m_owner, nullptr);
        m_previousContext = other.m_previousContext;
        m_previousRdram = other.m_previousRdram;
        m_previousToken = other.m_previousToken;
        m_token = other.m_token;
    }
    return *this;
}

void PS2IopHostAdapter::CallScope::release()
{
    if (m_owner)
    {
        m_owner->m_activeContext = m_previousContext;
        m_owner->m_activeRdram = m_previousRdram;
        m_owner->m_activeToken = m_previousToken;
        m_owner = nullptr;
    }
}

PS2IopHostAdapter::PS2IopHostAdapter(PS2Runtime &runtime)
    : m_runtime(runtime)
{
}

PS2IopHostAdapter::~PS2IopHostAdapter()
{
    std::lock_guard<std::mutex> lock(m_hostFileMutex);
    for (auto &[handle, file] : m_hostFiles)
    {
        (void)handle;
        if (file.stream)
        {
            std::fclose(file.stream);
        }
    }
    m_hostFiles.clear();
}

PS2IopHostAdapter::CallScope PS2IopHostAdapter::enterCall(R5900Context *context, uint8_t *rdram)
{
    return CallScope(*this, context, rdram);
}

bool PS2IopHostAdapter::guestRange(uint32_t address, size_t size, uint8_t *&begin) const
{
    begin = nullptr;
    uint8_t *const rdram = m_activeRdram
                               ? m_activeRdram
                               : m_runtime.memory().getRDRAM();
    if (!rdram)
    {
        return false;
    }
    if (size == 0)
    {
        begin = getMemPtr(rdram, address);
        return begin != nullptr;
    }
    if (size - 1 > std::numeric_limits<uint32_t>::max() - address)
    {
        return false;
    }
    uint8_t *const first = getMemPtr(rdram, address);
    uint8_t *const last = getMemPtr(rdram, address + static_cast<uint32_t>(size - 1));
    if (!first || !last || last < first || static_cast<size_t>(last - first) != size - 1)
    {
        return false;
    }
    begin = first;
    return true;
}

bool PS2IopHostAdapter::readGuest(uint32_t address, void *destination, size_t size) const
{
    if (!destination && size != 0)
    {
        return false;
    }
    uint8_t *source = nullptr;
    if (!guestRange(address, size, source))
    {
        return false;
    }
    if (size != 0)
    {
        std::memcpy(destination, source, size);
    }
    return true;
}

bool PS2IopHostAdapter::writeGuest(uint32_t address, const void *source, size_t size)
{
    if (!source && size != 0)
    {
        return false;
    }
    uint8_t *destination = nullptr;
    if (!guestRange(address, size, destination))
    {
        return false;
    }
    if (size != 0)
    {
        uint8_t *const rdram = m_activeRdram ? m_activeRdram : m_runtime.memory().getRDRAM();
        ps2TraceGuestRangeWrite(rdram, address, static_cast<uint32_t>(size), "IopHost::writeGuest", nullptr);
        std::memcpy(destination, source, size);
    }
    return true;
}

bool PS2IopHostAdapter::zeroGuest(uint32_t address, size_t size)
{
    uint8_t *destination = nullptr;
    if (!guestRange(address, size, destination))
    {
        return false;
    }
    if (size != 0)
    {
        uint8_t *const rdram = m_activeRdram ? m_activeRdram : m_runtime.memory().getRDRAM();
        ps2TraceGuestRangeWrite(rdram, address, static_cast<uint32_t>(size), "IopHost::zeroGuest", nullptr);
        std::memset(destination, 0, size);
    }
    return true;
}

bool PS2IopHostAdapter::normalizeGuestAddress(uint32_t address, uint32_t &normalized) const
{
    bool scratchpad = false;
    if (!ps2ResolveGuestPointer(address, normalized, scratchpad) || scratchpad)
    {
        normalized = 0;
        return false;
    }
    return true;
}

bool PS2IopHostAdapter::readIopMemory(uint32_t address, void *destination, size_t size) const
{
    return m_runtime.readIopMemory(address, destination, size);
}

bool PS2IopHostAdapter::writeIopMemory(uint32_t address, const void *source, size_t size)
{
    return m_runtime.writeIopMemory(address, source, size);
}

bool PS2IopHostAdapter::zeroIopMemory(uint32_t address, size_t size)
{
    return m_runtime.zeroIopMemory(address, size);
}

bool PS2IopHostAdapter::normalizeIopAddress(uint32_t address, uint32_t &normalized) const
{
    if (!m_runtime.isIopMemoryRange(address, 0u))
    {
        normalized = 0u;
        return false;
    }
    normalized = address & 0x1FFFFFFFu;
    return true;
}

uint32_t PS2IopHostAdapter::allocateIopHandle(ps2x::iop::IopHandleKind kind)
{
    uint8_t *const rdram = m_activeRdram
                               ? m_activeRdram
                               : m_runtime.memory().getRDRAM();
    if (!rdram)
    {
        return 0;
    }
    return kind == ps2x::iop::IopHandleKind::RpcPacket
               ? rpcAllocPacketAddr(rdram)
               : rpcAllocServerAddr(rdram);
}

uint32_t PS2IopHostAdapter::allocateGuest(uint32_t size, uint32_t alignment)
{
    return m_runtime.guestMalloc(size, alignment);
}

void PS2IopHostAdapter::freeGuest(uint32_t address)
{
    m_runtime.guestFree(address);
}

void PS2IopHostAdapter::audioCommand(uint32_t sid,
                                     uint32_t function,
                                     ps2x::iop::GuestBuffer send,
                                     ps2x::iop::GuestBuffer receive)
{
    uint8_t *sendPointer = nullptr;
    uint8_t *receivePointer = nullptr;
    if (send.address && !guestRange(send.address, send.size, sendPointer))
    {
        sendPointer = nullptr;
    }
    if (receive.address && !guestRange(receive.address, receive.size, receivePointer))
    {
        receivePointer = nullptr;
    }
    m_runtime.audioBackend().onSoundCommand(sid,
                                            function,
                                            sendPointer,
                                            send.size,
                                            receivePointer,
                                            receive.size);
}

void PS2IopHostAdapter::submitAudio(std::span<const int16_t> samples)
{
    m_runtime.audioBackend().submitPcm(samples);
}

void PS2IopHostAdapter::resetAudio()
{
    m_runtime.audioBackend().resetPcm();
}

std::string PS2IopHostAdapter::hostPath(ps2x::iop::HostPathKind kind) const
{
    const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
    switch (kind)
    {
    case ps2x::iop::HostPathKind::CdRoot:
        return paths.cdRoot.string();
    case ps2x::iop::HostPathKind::CdImage:
        return paths.cdImage.string();
    case ps2x::iop::HostPathKind::HostRoot:
        return paths.hostRoot.string();
    case ps2x::iop::HostPathKind::MemoryCardRoot:
        return paths.mcRoot.string();
    case ps2x::iop::HostPathKind::ElfDirectory:
    default:
        return paths.elfDirectory.string();
    }
}

namespace
{
    constexpr std::string_view kCdImagePathPrefix = "<cdimage>:";
}

std::string PS2IopHostAdapter::translateGuestPath(std::string_view path) const
{
    const ps2x::iop::ParsedPs2Path parsed = ps2x::iop::parsePs2Path(path);
    if (PS2CdImage::Entry entry{}; parsed && parsed.device == ps2x::iop::Ps2PathDevice::Cdrom &&
                                   PS2CdImage::instance().ensureOpenFromIoPaths() &&
                                   PS2CdImage::instance().find(parsed.path, entry) && !entry.directory)
    {
        return std::string(kCdImagePathPrefix) + std::to_string(entry.lba) + ":" + std::to_string(entry.size);
    }

    const PS2Runtime::IoPaths &paths = PS2Runtime::getIoPaths();
    const PS2VfsMounts mounts{paths.hostRoot, paths.cdRoot, paths.mcRoot};
    std::filesystem::path hostPath;
    if (!m_runtime.vfs().resolveHostPath(path, mounts, hostPath))
        return {};
    return hostPath.string();
}

uint64_t PS2IopHostAdapter::openHostFile(std::string_view path)
{
    if (path.empty())
    {
        return 0u;
    }

    if (path.substr(0, kCdImagePathPrefix.size()) == kCdImagePathPrefix)
    {
        const std::string spec(path.substr(kCdImagePathPrefix.size()));
        const size_t colon = spec.find(':');
        if (colon == std::string::npos)
        {
            return 0u;
        }
        HostFile file;
        file.cdImage = true;
        file.cdImageOffset = std::stoull(spec.substr(0, colon)) * PS2CdImage::kSectorSize;
        file.size = std::stoull(spec.substr(colon + 1));
        std::lock_guard<std::mutex> lock(m_hostFileMutex);
        uint64_t handle = m_nextHostFileHandle++;
        if (handle == 0u)
        {
            handle = m_nextHostFileHandle++;
        }
        m_hostFiles.emplace(handle, file);
        return handle;
    }

    const std::filesystem::path hostPath{std::string(path)};
#if defined(_WIN32)
    std::FILE *stream = ::_wfopen(hostPath.c_str(), L"rb");
#else
    std::FILE *stream = std::fopen(hostPath.string().c_str(), "rb");
#endif
    if (!stream)
    {
        return 0u;
    }

    std::error_code error;
    const uint64_t size = std::filesystem::file_size(hostPath, error);
    if (error)
    {
        std::fclose(stream);
        return 0u;
    }

    HostFile opened{stream, size};
    {
        const std::filesystem::path &image = PS2Runtime::getIoPaths().cdImage;
        std::error_code sameError;
        opened.discImageStream = !image.empty() && std::filesystem::equivalent(hostPath, image, sameError) && !sameError;
    }
    std::lock_guard<std::mutex> lock(m_hostFileMutex);
    uint64_t handle = m_nextHostFileHandle++;
    if (handle == 0u)
    {
        handle = m_nextHostFileHandle++;
    }
    m_hostFiles.emplace(handle, opened);
    return handle;
}

bool PS2IopHostAdapter::hostFileSize(uint64_t handle, uint64_t &size) const
{
    size = 0u;
    std::lock_guard<std::mutex> lock(m_hostFileMutex);
    const auto it = m_hostFiles.find(handle);
    if (it == m_hostFiles.end() || (!it->second.stream && !it->second.cdImage))
    {
        return false;
    }
    size = it->second.size;
    return true;
}

bool PS2IopHostAdapter::readHostFile(uint64_t handle,
                                     uint64_t offset,
                                     void *destination,
                                     size_t size,
                                     size_t &bytesRead)
{
    bytesRead = 0u;
    if (!destination && size != 0u)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_hostFileMutex);
    const auto it = m_hostFiles.find(handle);
    if (it != m_hostFiles.end() && it->second.cdImage)
    {
        const uint64_t available = offset < it->second.size ? it->second.size - offset : 0u;
        const size_t count = static_cast<size_t>(std::min<uint64_t>(size, available));
        if (count != 0u && !PS2CdImage::instance().read(it->second.cdImageOffset + offset, destination, count))
        {
            return false;
        }
        bytesRead = count;
        return true;
    }
    if (it == m_hostFiles.end() || !it->second.stream)
    {
        return false;
    }

#if defined(_WIN32)
    if (offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) ||
        _fseeki64(it->second.stream, static_cast<int64_t>(offset), SEEK_SET) != 0)
#else
    if (offset > static_cast<uint64_t>(std::numeric_limits<off_t>::max()) ||
        fseeko(it->second.stream, static_cast<off_t>(offset), SEEK_SET) != 0)
#endif
    {
        return false;
    }

    bytesRead = size == 0u
                    ? 0u
                    : std::fread(destination, 1u, size, it->second.stream);
    if (bytesRead < size && std::ferror(it->second.stream))
    {
        std::clearerr(it->second.stream);
        return false;
    }
    if (it->second.discImageStream)
        PS2DiscOverlay::apply(offset, destination, bytesRead);
    return true;
}

void PS2IopHostAdapter::closeHostFile(uint64_t handle)
{
    std::FILE *stream = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_hostFileMutex);
        const auto it = m_hostFiles.find(handle);
        if (it == m_hostFiles.end())
        {
            return;
        }
        stream = it->second.stream;
        m_hostFiles.erase(it);
    }
    if (stream)
    {
        std::fclose(stream);
    }
}

int32_t PS2IopHostAdapter::memoryCard(const ps2x::iop::MemoryCardRequest &request)
{
    using Handler = void (*)(uint8_t *, R5900Context *, PS2Runtime *);
    Handler handler = nullptr;
    switch (request.operation)
    {
    case ps2x::iop::MemoryCardOperation::Init:
        handler = ps2_stubs::sceMcInit;
        break;
    case ps2x::iop::MemoryCardOperation::GetInfo:
        handler = ps2_stubs::sceMcGetInfo;
        break;
    case ps2x::iop::MemoryCardOperation::Open:
        handler = ps2_stubs::sceMcOpen;
        break;
    case ps2x::iop::MemoryCardOperation::Close:
        handler = ps2_stubs::sceMcClose;
        break;
    case ps2x::iop::MemoryCardOperation::Seek:
        handler = ps2_stubs::sceMcSeek;
        break;
    case ps2x::iop::MemoryCardOperation::Read:
        handler = ps2_stubs::sceMcRead;
        break;
    case ps2x::iop::MemoryCardOperation::Write:
        handler = ps2_stubs::sceMcWrite;
        break;
    case ps2x::iop::MemoryCardOperation::Flush:
        handler = ps2_stubs::sceMcFlush;
        break;
    case ps2x::iop::MemoryCardOperation::Chdir:
        handler = ps2_stubs::sceMcChdir;
        break;
    case ps2x::iop::MemoryCardOperation::GetDir:
        handler = ps2_stubs::sceMcGetDir;
        break;
    case ps2x::iop::MemoryCardOperation::SetFileInfo:
        handler = ps2_stubs::sceMcSetFileInfo;
        break;
    case ps2x::iop::MemoryCardOperation::Delete:
        handler = ps2_stubs::sceMcDelete;
        break;
    case ps2x::iop::MemoryCardOperation::Format:
        handler = ps2_stubs::sceMcFormat;
        break;
    case ps2x::iop::MemoryCardOperation::Unformat:
        handler = ps2_stubs::sceMcUnformat;
        break;
    case ps2x::iop::MemoryCardOperation::Mkdir:
        handler = ps2_stubs::sceMcMkdir;
        break;
    }
    if (!handler)
    {
        return -1;
    }

    R5900Context context{};
    for (size_t i = 0; i < 4u; ++i)
    {
        setRegU32(&context, static_cast<int>(4 + i), request.arguments[i]);
    }

    // EE n32 ABI: the fifth argument travels in $t0, matching the sceMc* stubs.
    setRegU32(&context, 8, request.arguments[4]);

    handler(m_activeRdram ? m_activeRdram : m_runtime.memory().getRDRAM(),
            &context,
            &m_runtime);
    return ps2_stubs::getMemoryCardDebugSnapshot().lastResult;
}

bool PS2IopHostAdapter::hasGuestFunction(uint32_t address) const
{
    return m_runtime.hasFunction(address);
}

bool PS2IopHostAdapter::invokeGuestFunction(uint64_t callToken,
                                            uint32_t address,
                                            uint32_t a0,
                                            uint32_t a1,
                                            uint32_t a2,
                                            uint32_t a3,
                                            uint32_t *resultAddress)
{
    (void)callToken;
    (void)address;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    if (resultAddress)
    {
        *resultAddress = 0u;
    }
    return false;
}

bool PS2IopHostAdapter::sendSifCommand(uint32_t commandId,
                                       const void *packet,
                                       size_t packetSize)
{
    uint8_t *const rdram = m_activeRdram
                               ? m_activeRdram
                               : m_runtime.memory().getRDRAM();
    return ps2_stubs::dispatchSifCommand(rdram,
                                         &m_runtime,
                                         commandId,
                                         packet,
                                         packetSize);
}

namespace ps2_syscalls
{
    void completeAsyncIopRpc(PS2Runtime *runtime, uint64_t callToken);
}

void PS2IopHostAdapter::completeRpc(uint64_t callToken)
{
    ps2_syscalls::completeAsyncIopRpc(&m_runtime, callToken);
}

namespace ps2_syscalls
{
    bool findEeRegisteredRpcServer(uint32_t sid, uint32_t &serverData);
}

// IOP sceSifBindRpc to an EE server (Kernel/Syscalls/RPC.cpp keeps the EE registrations apart from the EE clients'
// IOP bindings). t_SifRpcServerData: sid 0x00, func 0x04, buf 0x08, size 0x0C, cfunc 0x10, cbuf 0x14.
bool PS2IopHostAdapter::bindEeRpcServer(uint32_t sid, uint32_t &serverData, uint32_t &buffer, uint32_t &callbackBuffer)
{
    serverData = buffer = callbackBuffer = 0u;
    uint32_t sd = 0u;
    if (!ps2_syscalls::findEeRegisteredRpcServer(sid, sd))
        return false;
    const auto *server = reinterpret_cast<const t_SifRpcServerData *>(getConstMemPtr(m_runtime.memory().getRDRAM(), sd));
    if (!server)
        return false;
    serverData = sd;
    buffer = server->buf;
    callbackBuffer = server->cbuf;
    return true;
}

// IOP sceSifCallRpc to an EE server, on the EE thread: like the EE RPC loop (sceSifRpcLoop -> sceSifExecRequest) the
// send data lands in the server's buffer, func(fno, buf, size) runs, and rsize bytes from its return pointer (the
// buffer when it returns 0) go back to the IOP. The function runs as a queued guest invocation (McPort's 0x80000400
// handler func_0111F018 only copies memory; it does not block).
bool PS2IopHostAdapter::callEeRpcServer(uint32_t serverData,
                                        uint32_t function,
                                        std::vector<uint8_t> send,
                                        uint32_t receiveSize,
                                        std::function<void(std::vector<uint8_t>, bool)> done)
{
    uint8_t *const rdram = m_runtime.memory().getRDRAM();
    auto *server = reinterpret_cast<t_SifRpcServerData *>(getMemPtr(rdram, serverData));
    if (!server || server->func == 0u || !m_runtime.hasFunction(server->func))
        return false;
    const uint32_t buffer = server->buf;
    if (!send.empty())
    {
        if (buffer == 0u || !getMemPtr(rdram, buffer) || !getMemPtr(rdram, buffer + static_cast<uint32_t>(send.size()) - 1u))
            return false;
        std::memcpy(getMemPtr(rdram, buffer), send.data(), send.size());
    }
    server->rpc_number = static_cast<int>(function);
    server->size = static_cast<int>(send.size());
    server->rsize = static_cast<int>(receiveSize);
    GuestInvocation invocation{};
    invocation.kind = GuestInvocationKind::RpcCallback;
    invocation.context = m_runtime.cpu();
    invocation.context.pc = server->func;
    SET_GPR_U32(&invocation.context, 4, function);
    SET_GPR_U32(&invocation.context, 5, buffer);
    SET_GPR_U32(&invocation.context, 6, static_cast<uint32_t>(send.size()));
    SET_GPR_U32(&invocation.context, 29, 0u);
    SET_GPR_U32(&invocation.context, 31, 0u);
    invocation.onComplete = [rdram, buffer, receiveSize, done = std::move(done)](const R5900Context &result, R5900Context &)
    {
        uint32_t source = getRegU32(&result, 2);
        if (source == 0u)
            source = buffer;
        std::vector<uint8_t> receive(receiveSize);
        bool ok = true;
        if (receiveSize != 0u)
        {
            const uint8_t *first = getConstMemPtr(rdram, source);
            const uint8_t *last = getConstMemPtr(rdram, source + receiveSize - 1u);
            ok = first && last && last >= first && static_cast<uint32_t>(last - first) == receiveSize - 1u;
            if (ok)
                std::memcpy(receive.data(), first, receiveSize);
        }
        done(std::move(receive), ok);
    };
    m_runtime.eeScheduler().queueInvocation(std::move(invocation));
    return true;
}

bool PS2IopHostAdapter::readPad(uint32_t port, ps2x::iop::PadInputState &state)
{
    state = ps2_host_input::padState(port);
    return state.connected;
}

bool PS2IopHostAdapter::readPadSlot(uint32_t port, uint32_t slot, ps2x::iop::PadInputState &state)
{
    state = ps2_host_input::padState(port, slot);
    return state.connected;
}

void PS2IopHostAdapter::padVibrationSlot(uint32_t port, uint32_t slot, uint8_t smallMotor, uint8_t largeMotor)
{
    ps2_host_input::setVibration(port, slot, smallMotor, largeMotor);
}

void PS2IopHostAdapter::log(ps2x::iop::LogLevel level, std::string_view message)
{
    const char *prefix = "[ps2xIOP]";
    if (level == ps2x::iop::LogLevel::Warning)
    {
        prefix = "[ps2xIOP:warning]";
    }
    else if (level == ps2x::iop::LogLevel::Error)
    {
        prefix = "[ps2xIOP:error]";
    }
    std::cerr << prefix << ' ' << message << std::endl;
}
