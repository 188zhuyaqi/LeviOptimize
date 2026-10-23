// Copyright (c) 2024, The Endstone Project. (https://endstone.dev) All Rights Reserved.
// Licensed under the Apache License, Version 2.0; see LICENSES/Apache-2.0.txt.
// Adapted from Endstone 4624070dd, e41e349ad, 42b472028 and fe86ade3f.
// Modifications: LL named fields, runtime symbol adapter, reusable native batch
// streams, reentrant TLS buffers, checked framing and native disable fallback.

#include "features/features.h"
#include "NativePeerAdapter.h"
#include "LeviOptimize.h"
#include "NetworkBuffers.h"
#include "NetworkCodec.h"
#include "NetworkLifetime.h"
#include "NetworkRuntime.h"
#include "RuntimeSymbols.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ll/api/memory/Hook.h"
#include "ll/api/thread/GlobalThreadPauser.h"
#include "mc/network/BatchedNetworkPeer.h"
#include "mc/network/NetworkConnection.h"
#include "mc/network/NetworkSystem.h"
#include "mc/network/PacketTraceNetworkPeer.h"

namespace lo::network_opt {
namespace {

static_assert(sizeof(CompressedNetworkPeer) == 128);
static_assert(static_cast<unsigned>(PacketCompressionAlgorithm::ZLib) == 0);
static_assert(static_cast<unsigned>(PacketCompressionAlgorithm::Snappy) == 1);

class CompressedPeer;
class BatchedPeer;
struct NetworkOwner {
    struct Peers {
        std::weak_ptr<CompressedPeer> compressed;
        std::weak_ptr<BatchedPeer>    batched;
    };
    std::recursive_mutex mutex;
    NetworkSystem*       network{};
    std::vector<Peers>   peers;
};

struct NetworkState {
    detail::RuntimeSymbols                                            symbols;
    std::atomic_bool                                                  enabled{true};
    std::mutex                                                        ownersMutex;
    std::unordered_map<NetworkSystem*, std::shared_ptr<NetworkOwner>> owners;

    std::shared_ptr<NetworkOwner> owner(NetworkSystem* network, bool create) {
        std::lock_guard lock(ownersMutex);
        if (auto found = owners.find(network); found != owners.end()) return found->second;
        if (!create) return {};
        auto result     = std::make_shared<NetworkOwner>();
        result->network = network;
        owners.emplace(network, result);
        return result;
    }
    void eraseOwner(NetworkSystem* network) {
        std::lock_guard lock(ownersMutex);
        owners.erase(network);
    }
    void restore();
};

std::atomic<std::shared_ptr<NetworkState>> activeState;
std::uintptr_t                             incomingTarget{};
std::uintptr_t                             destructorTarget{};
detail::PeerLifetimeCounter                retainedPeers;
std::atomic_size_t                         activeCallbacks{};

struct CallbackLifetime {
    CallbackLifetime() { activeCallbacks.fetch_add(1, std::memory_order_acq_rel); }
    ~CallbackLifetime() { activeCallbacks.fetch_sub(1, std::memory_order_acq_rel); }
};

class CompressedPeer final : public CompressedNetworkPeer {
    std::shared_ptr<CompressedNetworkPeer> mOriginal;
    std::shared_ptr<NetworkState>          mState;
    detail::NetworkCodec                   mCodec;
    mutable std::recursive_mutex           mFallbackMutex;
    bool                                   mRestored{};

    detail::CompressionSettings settings() const {
        return {
            mCompressionEnabled,
            mHybridCompressionEnabled,
            mNetworkSettings->mCompressionThreshold,
            static_cast<detail::CompressionAlgorithm>(mNetworkSettings->mCompressionAlgorithm),
            static_cast<std::size_t>(mMaxDecompressedSize)
        };
    }

public:
    void copySettingsToOriginal() const {
        mOriginal->mCompressionEnabled       = mCompressionEnabled;
        mOriginal->mHybridCompressionEnabled = mHybridCompressionEnabled;
        mOriginal->mMaxDecompressedSize      = mMaxDecompressedSize;
        mOriginal->mNetworkSettings          = mNetworkSettings.get();
    }
    void handoffToOriginal() {
        std::lock_guard lock(mFallbackMutex);
        copySettingsToOriginal();
        mRestored = true;
    }

    std::shared_ptr<CompressedNetworkPeer> const& original() const { return mOriginal; }

    CompressedPeer(std::shared_ptr<CompressedNetworkPeer> original, std::shared_ptr<NetworkState> state)
    : mOriginal(std::move(original)),
      mState(std::move(state)) {
        mPeer                     = mOriginal->mPeer;
        mCompressionEnabled       = mOriginal->mCompressionEnabled;
        mHybridCompressionEnabled = mOriginal->mHybridCompressionEnabled;
        mMaxDecompressedSize      = mOriginal->mMaxDecompressedSize;
        mNetworkSettings          = mOriginal->mNetworkSettings.get();
    }

    void sendPacket(std::string const& data, Reliability reliability, Compressibility compressible) override {
        if (!mState->enabled.load(std::memory_order_acquire)) {
            std::lock_guard lock(mFallbackMutex);
            if (!mRestored) copySettingsToOriginal();
            mOriginal->sendPacket(data, reliability, compressible);
            return;
        }
        if (!mCompressionEnabled) {
            mPeer->sendPacket(data, reliability, compressible);
            return;
        }
        // The lower peer consumes/copies synchronously, as required by the
        // NetworkPeer interface and native BatchedNetworkPeer's temporary data.
        // A nested send receives another stable TLS slot, preserving this one.
        detail::SendScratch scratch;
        if (!mCodec.encode(data, settings(), compressible == Compressibility::Compressible, scratch.buffer())) {
            getLogger().warn("optNetwork: unable to encode outgoing batch; batch was not sent.");
            return;
        }
        mPeer->sendPacket(scratch.buffer(), reliability, compressible);
    }

    DataStatus _receivePacket(std::string& outData, PacketRecvTimepointPtr const& timepoint) override {
        if (!mState->enabled.load(std::memory_order_acquire)) {
            std::lock_guard lock(mFallbackMutex);
            if (!mRestored) copySettingsToOriginal();
            return mOriginal->_receivePacket(outData, timepoint);
        }
        auto status = mPeer->_receivePacket(mReceiveBuffer.get(), timepoint);
        if (status != DataStatus::HasData) return status;
        return mCodec.decode(mReceiveBuffer.get(), settings(), outData) ? DataStatus::HasData : DataStatus::BrokenData;
    }

    NetworkStatus getNetworkStatus() const override { return mPeer->getNetworkStatus(); }
    void          update() override { mPeer->update(); }
    void          flush(std::function<void()>&& callback) override { mPeer->flush(std::move(callback)); }
    bool          isLocal() const override { return mPeer->isLocal(); }
    bool          isEncrypted() const override { return mPeer->isEncrypted(); }
    bool          isLan() const override { return mPeer->isLan(); }
};

// Retain the BDS-created BatchedNetworkPeer and its TaskGroup/SPSC queue. Both
// sync and async sends write this same stream, so toggling native async mode
// cannot strand a second outgoing buffer or duplicate its compressible count.
class BatchedPeer final : public NetworkPeer {
    std::shared_ptr<BatchedNetworkPeer> mNativeBatch;
    std::shared_ptr<NetworkPeer>        mOriginalTop;
    std::shared_ptr<NetworkState>       mState;

public:
    std::shared_ptr<NetworkPeer> const&        originalTop() const { return mOriginalTop; }
    std::shared_ptr<BatchedNetworkPeer> const& nativeBatch() const { return mNativeBatch; }
    BatchedPeer(
        std::shared_ptr<BatchedNetworkPeer> nativeBatch,
        std::shared_ptr<NetworkPeer>        compressed,
        std::shared_ptr<NetworkPeer>        originalTop,
        std::shared_ptr<NetworkState>       state
    )
    : mNativeBatch(std::move(nativeBatch)),
      mOriginalTop(std::move(originalTop)),
      mState(std::move(state)) {
        mPeer = std::move(compressed);
    }

    void sendPacket(std::string const& data, Reliability reliability, Compressibility compressible) override {
        if (!mState->enabled.load(std::memory_order_acquire)) {
            mOriginalTop->sendPacket(data, reliability, compressible);
            return;
        }
        auto& stream = mNativeBatch->mOutgoingData.get();
        if (!detail::appendBatchPacket(stream.mBuffer, data)) {
            getLogger().warn("optNetwork: outgoing packet exceeds the batch length field; packet was not sent.");
            return;
        }
        stream.mView = stream.mBuffer;
        if (compressible == Compressibility::Compressible) mNativeBatch->mCompressibleBytes += data.size();
        if (!detail::batchIsCompressible(mNativeBatch->mCompressibleBytes, stream.mBuffer.size())) flush({});
    }

    void flush(std::function<void()>&& callback) override {
        if (!mState->enabled.load(std::memory_order_acquire)) {
            mOriginalTop->flush(std::move(callback));
            return;
        }
        mState->symbols.flush(mNativeBatch.get(), std::move(callback));
    }
    void update() override {
        if (!mState->enabled.load(std::memory_order_acquire)) {
            mOriginalTop->update();
            return;
        }
        flush({});
        mPeer->update();
    }

    DataStatus _receivePacket(std::string& outData, PacketRecvTimepointPtr const& timepoint) override {
        if (!mState->enabled.load(std::memory_order_acquire)) return mOriginalTop->_receivePacket(outData, timepoint);
        auto& incoming = mNativeBatch->mIncomingData.get();
        auto& buffer   = mNativeBatch->mIncomingDataBuffer.get();
        if (incoming && incoming->mReadPointer > incoming->mView.size()) {
            incoming.reset();
            buffer.clear();
            return DataStatus::BrokenData;
        }
        if (!incoming || incoming->mReadPointer == incoming->mView.size()) {
            auto status = mPeer->_receivePacket(buffer, timepoint);
            if (status != DataStatus::HasData) return status;
            incoming.emplace(buffer, false);
            if (buffer.empty()) return DataStatus::BrokenData;
        }
        auto position = static_cast<std::size_t>(incoming->mReadPointer);
        auto packet   = detail::readBatchPacket(incoming->mView, position);
        if (!packet) {
            incoming.reset();
            buffer.clear();
            outData.clear();
            return DataStatus::BrokenData;
        }
        outData.assign(*packet);
        incoming->mReadPointer = position;
        return DataStatus::HasData;
    }

    NetworkStatus getNetworkStatus() const override { return mPeer->getNetworkStatus(); }
    bool          isLocal() const override { return mPeer->isLocal(); }
    bool          isEncrypted() const override { return mPeer->isEncrypted(); }
    bool          isLan() const override { return mPeer->isLan(); }
};

void NetworkState::restore() {
    std::vector<std::shared_ptr<NetworkOwner>> snapshot;
    {
        std::lock_guard lock(ownersMutex);
        for (auto const& [network, entry] : owners) snapshot.push_back(entry);
    }
    for (auto const& entry : snapshot) {
        std::lock_guard ownerLock(entry->mutex);
        if (!entry->network) continue;
        // Do not wait on a native lock while holding the lifetime guard: a
        // callback may already own it. Unrestored wrappers remain disabled and
        // counted, so DLL unload stays blocked until that connection releases.
        std::unique_lock nativeLock(entry->network->mConnectionsMutex.get(), std::try_to_lock);
        if (!nativeLock.owns_lock()) {
            getLogger().warn("optNetwork: busy connection list; disabled wrappers remain until connections close.");
            continue;
        }
        for (auto const& record : entry->peers) {
            auto batched    = record.batched.lock();
            auto compressed = record.compressed.lock();
            if (!batched || !compressed) continue;
            for (auto const& connection : entry->network->mConnections.get()) {
                if (connection->mPeer != batched) continue;
                compressed->handoffToOriginal();
                // Never overwrite a chain inserted after ours by another mod.
                if (batched->nativeBatch()->mPeer == compressed) {
                    batched->nativeBatch()->mPeer = compressed->original();
                }
                if (connection->mCompressedPeer.lock() == compressed) {
                    connection->mCompressedPeer = compressed->original();
                }
                connection->mPeer = batched->originalTop();
                break;
            }
        }
    }
}

} // namespace

LL_TYPE_INSTANCE_HOOK(
    NewNetworkConnectionHook,
    ll::memory::HookPriority::Normal,
    NetworkSystem,
    incomingTarget,
    bool,
    NetworkIdentifier const&       id,
    std::shared_ptr<NetworkPeer>&& peer
) {
    CallbackLifetime callbackLifetime;
    auto             state = activeState.load(std::memory_order_acquire);
    if (!state) return origin(id, std::move(peer));
    auto            owner = state->owner(this, true);
    std::lock_guard ownerLock(owner->mutex);
    // The recursive native mutex covers append, validation and publication.
    std::lock_guard lock(mConnectionsMutex.get());
    auto            before = mConnections->size();
    if (!origin(id, std::move(peer))) return false;
    if (!state || !state->enabled.load(std::memory_order_acquire)) return true;
    if (mConnections->size() != before + 1) {
        getLogger().warn("optNetwork: unexpected connection append; preserving the native peer chain.");
        return true;
    }
    auto& connection = *mConnections->back();
    auto  compressed = connection.mCompressedPeer.lock();
    auto  batched    = connection.mBatchedPeer.lock();
    auto  top        = connection.mPeer;
    auto  trace      = connection.mPacketTracePeer.lock();
    if (!compressed || !batched || !top) return true;
    if (connection.mId.get() != id || batched->mPeer != compressed
        || (top != batched && (top != trace || top->mPeer != batched))) {
        getLogger().warn("optNetwork: unknown or modified peer chain; preserving the native connection.");
        return true;
    }
    auto newCompressed = retainedPeers.make<CompressedPeer>(compressed, state);
    auto newBatched    = retainedPeers.make<BatchedPeer>(batched, newCompressed, top, state);
    std::erase_if(owner->peers, [](auto const& record) {
        return record.compressed.expired() && record.batched.expired();
    });
    owner->peers.push_back({newCompressed, newBatched});
    // Publish only after all allocations and shape checks succeed. The weak
    // native batch handle is deliberately kept: BDS owns its async switch.
    batched->mPeer             = newCompressed;
    connection.mCompressedPeer = newCompressed;
    connection.mPeer           = std::move(newBatched);
    return true;
}

LL_TYPE_INSTANCE_HOOK(
    NetworkSystemDestructorHook,
    ll::memory::HookPriority::Normal,
    NetworkSystem,
    destructorTarget,
    void
) {
    CallbackLifetime callbackLifetime;
    auto             state = activeState.load(std::memory_order_acquire);
    auto             owner = state ? state->owner(this, false) : nullptr;
    if (owner) {
        std::lock_guard lock(owner->mutex);
        owner->network = nullptr;
        state->eraseOwner(this);
        origin();
        return;
    }
    origin();
}

namespace {
struct ResidentHooks {
    bool incoming{};
    bool destructor{};
};
std::mutex                     hooksMutex;
std::unique_ptr<ResidentHooks> residentHooks;

bool ensureResidentHooks(detail::RuntimeSymbols const& symbols, int& result) {
    std::lock_guard lock(hooksMutex);
    if (!residentHooks) residentHooks = std::make_unique<ResidentHooks>();
    incomingTarget   = symbols.incoming;
    destructorTarget = symbols.destructor;
    if (!residentHooks->destructor) {
        result                    = NetworkSystemDestructorHook::hook();
        residentHooks->destructor = result >= 0;
    }
    if (residentHooks->destructor && !residentHooks->incoming) {
        result                  = NewNetworkConnectionHook::hook();
        residentHooks->incoming = result >= 0;
    }
    return residentHooks->incoming && residentHooks->destructor;
}
} // namespace

struct NetworkOpt::Impl {
    std::shared_ptr<NetworkState> state = std::make_shared<NetworkState>();

    Impl() {
        if (!state->symbols.resolve()) {
            if (!state->symbols.incoming)
                getLogger().warn("optNetwork: missing NetworkSystem::onNewIncomingConnection; optimization skipped.");
            if (!state->symbols.destructor)
                getLogger().warn("optNetwork: missing NetworkSystem destructor; optimization skipped.");
            if (!state->symbols.flush)
                getLogger().warn("optNetwork: missing BatchedNetworkPeer::flush; optimization skipped.");
            return;
        }
        activeState.store(state, std::memory_order_release);
        int result{};
        if (!ensureResidentHooks(state->symbols, result)) {
            activeState.store({}, std::memory_order_release);
            state->enabled.store(false, std::memory_order_release);
            getLogger().warn("optNetwork: unable to hook new connections ({}); optimization was skipped.", result);
        }
    }
    ~Impl() {
        state->enabled.store(false, std::memory_order_release);
        state->restore();
        auto expected = state;
        activeState.compare_exchange_strong(expected, {}, std::memory_order_acq_rel);
        // Existing wrappers retain the original chain and share this disabled
        // state. Hooks remain resident as well: disable can be reentered from a
        // native callback, which must not release its own active trampoline.
    }
};

std::size_t liveWrapperCount() noexcept { return retainedPeers.count(); }
bool        canUnload() noexcept {
    try {
        auto pending = [] {
            return activeState.load(std::memory_order_acquire) || liveWrapperCount() != 0
                || activeCallbacks.load(std::memory_order_acquire) != 0;
        };
        if (pending()) return false;
        std::lock_guard lock(hooksMutex);
        if (pending()) return false;
        std::optional<ll::thread::GlobalThreadPauser> paused;
        if (ll::memory::shouldHookSuspendThreads()) paused.emplace();
        if (pending()) return false;
        if (residentHooks) {
            if (residentHooks->incoming) {
                if (!NewNetworkConnectionHook::unhook(false)) return false;
                residentHooks->incoming = false;
            }
            if (residentHooks->destructor) {
                if (!NetworkSystemDestructorHook::unhook(false)) return false;
                residentHooks->destructor = false;
            }
            residentHooks.reset();
        }
        return true;
    } catch (...) {
        return false;
    }
}

void NetworkOpt::call(bool enable) {
    if (enable) {
        if (!impl) impl = std::make_unique<Impl>();
    } else {
        impl.reset();
    }
}

NetworkOpt::NetworkOpt()                                 = default;
NetworkOpt::~NetworkOpt()                                = default;
NetworkOpt::NetworkOpt(NetworkOpt&&) noexcept            = default;
NetworkOpt& NetworkOpt::operator=(NetworkOpt&&) noexcept = default;

} // namespace lo::network_opt
