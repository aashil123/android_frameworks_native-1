/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <iomanip>
#include <sstream>

#include <android/hardware_buffer.h>
#include <bufferhub/BufferHubService.h>
#include <cutils/native_handle.h>
#include <log/log.h>
#include <system/graphics-base.h>

using ::android::BufferHubDefs::MetadataHeader;
using ::android::hardware::Void;

namespace android {
namespace frameworks {
namespace bufferhub {
namespace V1_0 {
namespace implementation {

Return<void> BufferHubService::allocateBuffer(const HardwareBufferDescription& description,
                                              const uint32_t userMetadataSize,
                                              allocateBuffer_cb _hidl_cb) {
    AHardwareBuffer_Desc desc;
    memcpy(&desc, &description, sizeof(AHardwareBuffer_Desc));

    std::shared_ptr<BufferNode> node =
            std::make_shared<BufferNode>(desc.width, desc.height, desc.layers, desc.format,
                                         desc.usage, userMetadataSize,
                                         BufferHubIdGenerator::getInstance().getId());
    if (node == nullptr || !node->IsValid()) {
        ALOGE("%s: creating BufferNode failed.", __FUNCTION__);
        _hidl_cb(/*status=*/BufferHubStatus::ALLOCATION_FAILED, /*bufferClient=*/nullptr,
                 /*bufferTraits=*/{});
        return Void();
    }

    sp<BufferClient> client = BufferClient::create(this, node);
    // Add it to list for bookkeeping and dumpsys.
    std::lock_guard<std::mutex> lock(mClientSetMutex);
    mClientSet.emplace(client);

    BufferTraits bufferTraits = {/*bufferDesc=*/description,
                                 /*bufferHandle=*/hidl_handle(node->buffer_handle()),
                                 // TODO(b/116681016): return real data to client
                                 /*bufferInfo=*/hidl_handle()};

    _hidl_cb(/*status=*/BufferHubStatus::NO_ERROR, /*bufferClient=*/client,
             /*bufferTraits=*/bufferTraits);
    return Void();
}

Return<void> BufferHubService::importBuffer(const hidl_handle& tokenHandle,
                                            importBuffer_cb _hidl_cb) {
    if (!tokenHandle.getNativeHandle() || tokenHandle->numFds != 0 || tokenHandle->numInts != 1) {
        // nullptr handle or wrong format
        _hidl_cb(/*status=*/BufferHubStatus::INVALID_TOKEN, /*bufferClient=*/nullptr,
                 /*bufferTraits=*/{});
        return Void();
    }

    uint32_t token = tokenHandle->data[0];

    wp<BufferClient> originClientWp;
    {
        std::lock_guard<std::mutex> lock(mTokenMapMutex);
        auto iter = mTokenMap.find(token);
        if (iter == mTokenMap.end()) {
            // Invalid token
            _hidl_cb(/*status=*/BufferHubStatus::INVALID_TOKEN, /*bufferClient=*/nullptr,
                     /*bufferTraits=*/{});
            return Void();
        }

        originClientWp = iter->second;
        mTokenMap.erase(iter);
    }

    // Check if original client is dead
    sp<BufferClient> originClient = originClientWp.promote();
    if (!originClient) {
        // Should not happen since token should be removed if already gone
        ALOGE("%s: original client %p gone!", __FUNCTION__, originClientWp.unsafe_get());
        _hidl_cb(/*status=*/BufferHubStatus::BUFFER_FREED, /*bufferClient=*/nullptr,
                 /*bufferTraits=*/{});
        return Void();
    }

    sp<BufferClient> client = new BufferClient(*originClient);
    uint32_t clientStateMask = client->getBufferNode()->AddNewActiveClientsBitToMask();
    if (clientStateMask == 0U) {
        // Reach max client count
        ALOGE("%s: import failed, BufferNode#%u reached maximum clients.", __FUNCTION__,
              client->getBufferNode()->id());
        _hidl_cb(/*status=*/BufferHubStatus::MAX_CLIENT, /*bufferClient=*/nullptr,
                 /*bufferTraits=*/{});
        return Void();
    }

    std::lock_guard<std::mutex> lock(mClientSetMutex);
    mClientSet.emplace(client);

    std::shared_ptr<BufferNode> node = client->getBufferNode();

    HardwareBufferDescription bufferDesc;
    memcpy(&bufferDesc, &node->buffer_desc(), sizeof(HardwareBufferDescription));

    BufferTraits bufferTraits = {/*bufferDesc=*/bufferDesc,
                                 /*bufferHandle=*/hidl_handle(node->buffer_handle()),
                                 // TODO(b/116681016): return real data to client
                                 /*bufferInfo=*/hidl_handle()};

    _hidl_cb(/*status=*/BufferHubStatus::NO_ERROR, /*bufferClient=*/client,
             /*bufferTraits=*/bufferTraits);
    return Void();
}

Return<void> BufferHubService::debug(const hidl_handle& fd, const hidl_vec<hidl_string>& args) {
    if (fd.getNativeHandle() == nullptr || fd->numFds < 1) {
        ALOGE("%s: missing fd for writing.", __FUNCTION__);
        return Void();
    }

    FILE* out = fdopen(dup(fd->data[0]), "w");

    if (args.size() != 0) {
        fprintf(out,
                "Note: lshal bufferhub currently does not support args. Input arguments are "
                "ignored.\n");
    }

    std::ostringstream stream;

    // Get the number of clients of each buffer.
    // Map from bufferId to bufferNode_clientCount pair.
    std::map<int, std::pair<const std::shared_ptr<BufferNode>, uint32_t>> clientCount;
    {
        std::lock_guard<std::mutex> lock(mClientSetMutex);
        for (auto iter = mClientSet.begin(); iter != mClientSet.end(); ++iter) {
            sp<BufferClient> client = iter->promote();
            if (client != nullptr) {
                const std::shared_ptr<BufferNode> node = client->getBufferNode();
                auto mapIter = clientCount.find(node->id());
                if (mapIter != clientCount.end()) {
                    ++mapIter->second.second;
                } else {
                    clientCount.emplace(node->id(),
                                        std::pair<std::shared_ptr<BufferNode>, uint32_t>(node, 1U));
                }
            }
        }
    }

    stream << "Active Buffers:\n";
    stream << std::right;
    stream << std::setw(6) << "Id";
    stream << " ";
    stream << std::setw(9) << "Clients";
    stream << " ";
    stream << std::setw(14) << "Geometry";
    stream << " ";
    stream << std::setw(6) << "Format";
    stream << " ";
    stream << std::setw(10) << "Usage";
    stream << " ";
    stream << std::setw(10) << "State";
    stream << " ";
    stream << std::setw(10) << "Index";
    stream << std::endl;

    for (auto iter = clientCount.begin(); iter != clientCount.end(); ++iter) {
        const std::shared_ptr<BufferNode> node = std::move(iter->second.first);
        const uint32_t clientCount = iter->second.second;
        AHardwareBuffer_Desc desc = node->buffer_desc();

        MetadataHeader* metadataHeader =
                const_cast<BufferHubMetadata*>(&node->metadata())->metadata_header();
        const uint32_t state = metadataHeader->buffer_state.load(std::memory_order_acquire);
        const uint64_t index = metadataHeader->queue_index;

        stream << std::right;
        stream << std::setw(6) << /*Id=*/node->id();
        stream << " ";
        stream << std::setw(9) << /*Clients=*/clientCount;
        stream << " ";
        if (desc.format == HAL_PIXEL_FORMAT_BLOB) {
            std::string size = std::to_string(desc.width) + " B";
            stream << std::setw(14) << /*Geometry=*/size;
        } else {
            std::string dimensions = std::to_string(desc.width) + "x" +
                    std::to_string(desc.height) + "x" + std::to_string(desc.layers);
            stream << std::setw(14) << /*Geometry=*/dimensions;
        }
        stream << " ";
        stream << std::setw(6) << /*Format=*/desc.format;
        stream << " ";
        stream << "0x" << std::hex << std::setfill('0');
        stream << std::setw(8) << /*Usage=*/desc.usage;
        stream << std::dec << std::setfill(' ');
        stream << " ";
        stream << "0x" << std::hex << std::setfill('0');
        stream << std::setw(8) << /*State=*/state;
        stream << " ";
        stream << std::setw(8) << /*Index=*/index;
        stream << std::endl;
    }

    stream << std::endl;

    // Get the number of tokens of each buffer.
    // Map from bufferId to tokenCount
    std::map<int, uint32_t> tokenCount;
    {
        std::lock_guard<std::mutex> lock(mTokenMapMutex);
        for (auto iter = mTokenMap.begin(); iter != mTokenMap.end(); ++iter) {
            sp<BufferClient> client = iter->second.promote();
            if (client != nullptr) {
                const std::shared_ptr<BufferNode> node = client->getBufferNode();
                auto mapIter = tokenCount.find(node->id());
                if (mapIter != tokenCount.end()) {
                    ++mapIter->second;
                } else {
                    tokenCount.emplace(node->id(), 1U);
                }
            }
        }
    }

    stream << "Unused Tokens:\n";
    stream << std::right;
    stream << std::setw(8) << "Buffer Id";
    stream << " ";
    stream << std::setw(6) << "Tokens";
    stream << std::endl;

    for (auto iter = tokenCount.begin(); iter != tokenCount.end(); ++iter) {
        stream << std::right;
        stream << std::setw(8) << /*Buffer Id=*/iter->first;
        stream << " ";
        stream << std::setw(6) << /*Tokens=*/iter->second;
        stream << std::endl;
    }

    fprintf(out, "%s", stream.str().c_str());

    fclose(out);
    return Void();
}

hidl_handle BufferHubService::registerToken(const wp<BufferClient>& client) {
    uint32_t token;
    std::lock_guard<std::mutex> lock(mTokenMapMutex);
    do {
        token = mTokenEngine();
    } while (mTokenMap.find(token) != mTokenMap.end());

    // native_handle_t use int[], so here need one slots to fit in uint32_t
    native_handle_t* handle = native_handle_create(/*numFds=*/0, /*numInts=*/1);
    handle->data[0] = token;

    // returnToken owns the native_handle_t* thus doing lifecycle management
    hidl_handle returnToken;
    returnToken.setTo(handle, /*shoudOwn=*/true);

    mTokenMap.emplace(token, client);
    return returnToken;
}

void BufferHubService::onClientClosed(const BufferClient* client) {
    removeTokenByClient(client);

    std::lock_guard<std::mutex> lock(mClientSetMutex);
    auto iter = std::find(mClientSet.begin(), mClientSet.end(), client);
    if (iter != mClientSet.end()) {
        mClientSet.erase(iter);
    }
}

void BufferHubService::removeTokenByClient(const BufferClient* client) {
    std::lock_guard<std::mutex> lock(mTokenMapMutex);
    auto iter = mTokenMap.begin();
    while (iter != mTokenMap.end()) {
        if (iter->second == client) {
            auto oldIter = iter;
            ++iter;
            mTokenMap.erase(oldIter);
        } else {
            ++iter;
        }
    }
}

} // namespace implementation
} // namespace V1_0
} // namespace bufferhub
} // namespace frameworks
} // namespace android
