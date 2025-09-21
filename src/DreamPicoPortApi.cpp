// MIT License
//
// Copyright (c) 2025 James Smith of OrangeFox86
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "DreamPicoPortApi.hpp"
#include "LibusbWrappers.hpp"

#include <cstdint>
#include <cstdlib>
#include <vector>
#include <thread>
#include <mutex>
#include <functional>
#include <algorithm>

namespace dpp_api
{

//! The magic sequence which starts each packet
static constexpr const std::uint8_t kMagicSequence[] = {0xDB, 0x8B, 0xAF, 0xD5};
//! The number of bytes in the magic sequence
static constexpr const std::int8_t kSizeMagic = sizeof(kMagicSequence);
//! The number of packet size bytes (2 for size and 2 for inverse size)
static constexpr const std::int8_t kSizeSize = 4;
//! Minimum number of bytes used for return address in packet
static constexpr const std::int8_t kMinSizeAddress = 1;
//! Maximum number of bytes used for return address in packet
static constexpr const std::int8_t kMaxSizeAddress = 9;
//! The number of bytes used for command in packet
static constexpr const std::int8_t kSizeCommand = 1;
//! The number of bytes used for CRC at the end of the packet
static constexpr const std::int8_t kSizeCrc = 2;
//! Minimum number of bytes of a packet
static constexpr const std::int8_t kMinPacketSize =
    kSizeMagic + kSizeSize + kMinSizeAddress + kSizeCommand + kSizeCrc;


//! Converts 2 bytes in network order to a uint16 value in host order
//! @param[in] payload Pointer to the beginning of the 2-byte sequence
//! @return the converted uint16 value
static std::uint16_t bytesToUint16(const void* payload)
{
    const std::uint8_t* p8 = reinterpret_cast<const std::uint8_t*>(payload);
    return (static_cast<std::uint16_t>(p8[0]) << 8 | p8[1]);
}

//! Converts a uint16 value from host order int a byte buffer in network order
//! @param[out] out The buffer to write the next 2 bytes to
//! @param[in] data The uint16 value to convert
static void uint16ToBytes(void* out, std::uint16_t data)
{
    std::uint8_t* p8 = reinterpret_cast<std::uint8_t*>(out);
    *p8++ = data >> 8;
    *p8 = data & 0xFF;
}

//! Converts 4 bytes in network order to a uint16 value in host order
//! @param[in] payload Pointer to the beginning of the 4-byte sequence
//! @return the converted uint32 value
static std::uint32_t bytesToUint32(const void* payload)
{
    const std::uint8_t* p8 = reinterpret_cast<const std::uint8_t*>(payload);
    return (
        static_cast<std::uint32_t>(p8[0]) << 24 |
        static_cast<std::uint32_t>(p8[1]) << 16 |
        static_cast<std::uint32_t>(p8[2]) << 8 |
        p8[3]
    );
}

//! Converts a uint32 value from host order int a byte buffer in network order
//! @param[out] out The buffer to write the next 4 bytes to
//! @param[in] data The uint32 value to convert
static void uint32ToBytes(void* out, std::uint32_t data)
{
    std::uint8_t* p8 = reinterpret_cast<std::uint8_t*>(out);
    *p8++ = (data >> 24) & 0xFF;
    *p8++ = (data >> 16) & 0xFF;
    *p8++ = (data >> 8) & 0xFF;
    *p8 = data & 0xFF;
}

//! Compute CRC16 over a buffer using a seed value
//! @param[in] seed The seed value to start with
//! @param[in] buffer Pointer to byte array
//! @param[in] bufLen Number of bytes to read from buffer
//! @return CRC16 value
static std::uint16_t computeCrc16(std::uint16_t seed, const void* buffer, std::uint16_t bufLen)
{
    std::uint16_t crc = seed;
    const std::uint8_t* b8 = reinterpret_cast<const std::uint8_t*>(buffer);

    for (std::uint16_t i = 0; i < bufLen; ++i)
    {
        crc ^= static_cast<uint8_t>(*b8++) << 8;
        for (int j = 0; j < 8; ++j)
        {
            if (crc & 0x8000)
            {
                crc = (crc << 1) ^ 0x1021;
            }
            else
            {
                crc <<= 1;
            }
        }
    }

    return crc;
}

//! Compute CRC16 over a buffer
//! @param[in] buffer Pointer to byte array
//! @param[in] bufLen Number of bytes to read from buffer
//! @return CRC16 value
static std::uint16_t computeCrc16(const void* buffer, std::uint16_t bufLen)
{
    return computeCrc16(0xFFFFU, buffer, bufLen);
}


//! This class hides implementation details to simplify the public interface.
class DppDeviceImp
{
public:
    DppDeviceImp(std::unique_ptr<LibusbDevice>&& dev) : mLibusbDevice(std::move(dev)) {}
    ~DppDeviceImp()
    {
        disconnect();
    }

    //! Packs a packet structure into a vector
    //! @param[in] addr The return address
    //! @param[in] cmd The command to set
    //! @param[in] payload The payload for the command
    //! @return the packed data
    static std::vector<std::uint8_t> pack(
        std::uint64_t addr,
        std::uint8_t cmd,
        const std::vector<std::uint8_t>& payload
    )
    {
        // Create address bytes
        std::vector<std::uint8_t> addrBytes;
        addrBytes.reserve(kMaxSizeAddress);
        std::int8_t idx = 0;
        while (addr > 0 || idx == 0)
        {
            const uint8_t orMask = (idx < (kMaxSizeAddress - 1)) ? ((addr > 0x7F) ? 0x80 : 0x00) : 0x00;
            const uint8_t shift = (idx < (kMaxSizeAddress - 1)) ? 7 : 8;
            addrBytes.push_back(static_cast<std::uint8_t>(addr & 0xFF) | orMask);
            addr >>= shift;
            ++idx;
        }

        // Create size bytes
        const std::uint16_t size =
            static_cast<std::uint16_t>(addrBytes.size() + kSizeCommand + payload.size() + kSizeCrc);
        const std::uint16_t invSize = 0xFFFF ^ size;
        std::uint8_t sizeBytes[kSizeSize];
        uint16ToBytes(&sizeBytes[0], size);
        uint16ToBytes(&sizeBytes[2], invSize);

        // Pack the data
        std::vector<std::uint8_t> data;
        data.reserve(kSizeMagic + kSizeSize + addrBytes.size() + kSizeCommand + payload.size() + kSizeCrc);
        data.insert(data.end(), kMagicSequence, kMagicSequence + kSizeMagic);
        data.insert(data.end(), sizeBytes, sizeBytes + kSizeSize);
        data.insert(data.end(), addrBytes.begin(), addrBytes.end());
        data.push_back(cmd);
        data.insert(data.end(), payload.begin(), payload.end());
        const std::uint16_t crc = computeCrc16(&data[kSizeMagic + kSizeSize], data.size() - kSizeMagic - kSizeSize);
        std::uint8_t crcBytes[kSizeCrc];
        uint16ToBytes(crcBytes, crc);
        data.insert(data.end(), crcBytes, crcBytes + kSizeCrc);

        return data;
    }

    //! Sends data on the vendor interface
    //! @param[in] addr The return address
    //! @param[in] cmd The command to set
    //! @param[in] payload The payload for the command
    //! @param[in] timeoutMs Send timeout in milliseconds
    //! @return true if data was successfully sent
    bool send(
        std::uint64_t addr,
        std::uint8_t cmd,
        const std::vector<std::uint8_t>& payload,
        unsigned int timeoutMs = 1000
    )
    {
        std::vector<std::uint8_t> data = pack(addr, cmd, payload);
        return mLibusbDevice->send(&data[0], static_cast<int>(data.size()), timeoutMs);
    }

    //! Connect to the device and start operation threads. If already connected, disconnect before reconnecting.
    //! @param[in] fn When true is returned, this is the function that will execute when the device is disconnected
    //!               errStr: the reason for disconnection or empty string if disconnect() was called
    //!               NOTICE: Any attempt to call connect() within any callback function will always fail
    //! @return false on failure and getLastErrorStr() will return error description
    //! @return true if connection succeeded
    bool connect(const std::function<void(std::string& errStr)>& fn)
    {
        // This function may not be called from any thread context (would cause deadlock)
        {
            // (never lock mConnectionMutex while mProcessThreadMutex is locked)
            std::lock_guard<std::mutex> lock(mProcessMutex);
            if (
                (mProcessThread && mProcessThread->get_id() == std::this_thread::get_id()) ||
                (mReadThread && mReadThread->get_id() == std::this_thread::get_id())
            )
            {
                setExternalError("connect attempted within thread context");
                return false;
            }
        }

        std::lock_guard<std::recursive_mutex> lock(mConnectionMutex);

        // Because of the above checks, calling disconnect() here will ensure all threads are stopped and joined
        if (!disconnect())
        {
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(mDisconnectMutex);
            mDisconnectCallback = fn;
            mDisconnectReason.clear();
            mDisconnectReason.shrink_to_fit();
        }

        if (!mLibusbDevice->openInterface())
        {
            return false;
        }

        std::lock_guard<std::mutex> threadLock(mProcessMutex);

        mProcessing = true;

        mReadThread = std::make_unique<std::thread>(
            [this]()
            {
                mLibusbDevice->run([this](const std::uint8_t* buffer, int len){ handleReceive(buffer, len); });

                // Save the error description at this point
                {
                    std::lock_guard<std::mutex> lock(mDisconnectMutex);
                    mDisconnectReason = mLibusbDevice->getLastErrorStr();
                }

                // Ensure disconnection
                disconnect();
            }
        );

        mProcessThread = std::make_unique<std::thread>(
            [this]()
            {
                processEntrypoint();
            }
        );

        mConnected = true;

        return true;
    }

    //! Disconnect from the previously connected device and stop all threads
    //! @return false on failure and getLastErrorStr() will return error description
    //! @return true if disconnection succeeded or was already disconnected
    bool disconnect()
    {
        // Do not take mConnectionMutex while in thread context. Instead, simply stop processing without joining.
        {
            // (never lock mConnectionMutex while mProcessThreadMutex is locked)
            std::lock_guard<std::mutex> lock(mProcessMutex);
            bool isReadThread = (mReadThread && mReadThread->get_id() == std::this_thread::get_id());
            bool isProcessThread = (mProcessThread && mProcessThread->get_id() == std::this_thread::get_id());
            if (isReadThread || isProcessThread)
            {
                // Stop processing without joining
                mLibusbDevice->stopRead();
                if (isReadThread)
                {
                    mProcessing = false;
                    mProcessCv.notify_all();
                }
                return true;
            }
        }

        std::lock_guard<std::recursive_mutex> lock(mConnectionMutex);

        if (!mConnected)
        {
            return true;
        }

        mConnected = false;

        // Calling this may cause a call to disconnect() from the read thread
        bool closed = mLibusbDevice->closeInterface();

        if (mReadThread)
        {
            mReadThread->join();
        }

        if (mProcessThread)
        {
            mProcessThread->join();
        }

        // Delete the threads
        {
            std::lock_guard<std::mutex> lock(mProcessMutex);
            mReadThread.reset();
            mProcessThread.reset();
        }

        return closed;
    }

    bool isConnected()
    {
        return mConnected;
    }
    //! Send a raw command to DreamPicoPort
    //! @param[in] cmd Raw DreamPicoPort command
    //! @param[in] payload The payload for the command
    //! @param[in] respFn The function to call on received response, timeout, or disconnect with the following arguments
    //!                   cmd: one of the kCmd* values
    //!                   payload: the returned payload
    //!                   NOTICE: Any attempt to call connect() within any callback function will always fail
    //! @param[in] timeoutMs Duration to wait before timeout
    //! @return 0 if send failed and getLastErrorStr() will return error description
    //! @return the ID of the sent data
    std::uint64_t send(
        std::uint8_t cmd,
        const std::vector<std::uint8_t>& payload,
        const std::function<void(std::int16_t cmd, std::vector<std::uint8_t>& payload)>& respFn,
        std::uint32_t timeoutMs
    )
    {
        std::uint64_t addr = 0;

        {
            std::lock_guard<std::mutex> lock(mNextAddrMutex);

            if (mNextAddr < kMinAddr)
            {
                mNextAddr = kMinAddr;
            }

            addr = mNextAddr++;

            if (mNextAddr > mMaxAddr)
            {
                mNextAddr = kMinAddr;
            }
        }

        std::vector<std::uint8_t> packedData = pack(addr, cmd, payload);

        if (respFn)
        {
            std::lock_guard<std::mutex> lock(mProcessMutex);

            if (!mProcessing)
            {
                mLibusbDevice->setExternalError("send called while disconnected");
                return 0;
            }

            std::chrono::system_clock::time_point expiration =
                std::chrono::system_clock::now() + std::chrono::milliseconds(timeoutMs);

            FunctionLookupMapEntry entry;
            entry.callback = respFn;
            entry.timeoutMapIter = mTimeoutLookup.insert(std::make_pair(expiration, addr));

            mFnLookup[addr] = std::move(entry);

            mOutgoingData.push_back({addr, std::move(packedData)});

            mProcessCv.notify_all();
        }

        return addr;
    }

    //! Sets the maximum return address value used to tag each command
    //! @note the minimum maximum is 0x0FFFFFFF to ensure proper execution
    //! @param[in] maxAddr The maximum address value to set
    static void setMaxAddr(std::uint64_t maxAddr)
    {
        mMaxAddr = (std::max)(maxAddr, static_cast<std::uint64_t>(0x0FFFFFFF));
    }

    //! @return the serial of this device
    const std::string& getSerial() const
    {
        return mLibusbDevice->mSerial;
    }

    //! @return USB version number {major, minor, patch}
    std::array<std::uint8_t, 3> getVersion() const
    {
        return mLibusbDevice->getVersion();
    }

    //! @return string representation of last error
    std::string getLastErrorStr()
    {
        return mLibusbDevice->getLastErrorStr();
    }

    //! @return number of waiting responses
    std::size_t getNumWaiting()
    {
        std::lock_guard<std::mutex> lock(mProcessMutex);
        // Size of both of these maps should be equal
        return (std::max)(mFnLookup.size(), mTimeoutLookup.size());
    }

    //! Set an error which occurs externally
    //! @param[in] where Explanation of where the error occurred
    void setExternalError(const char* where)
    {
        mLibusbDevice->setExternalError(where);
    }

    //! Retrieve the currently connected interface number (first VENDOR interface)
    //! @return the connected interface number
    int getInterfaceNumber()
    {
        return mLibusbDevice->getInterfaceNumber();
    }

    //! @return the currently used IN endpoint
    std::uint8_t getEpIn()
    {
        return mLibusbDevice->getEpIn();
    }

    //! @return the currently used OUT endpoint
    std::uint8_t getEpOut()
    {
        return mLibusbDevice->getEpOut();
    }

private:
    //! Handle received data
    //! @param[in] buffer Buffer received from libusb
    //! @param[in] len Number of bytes in buffer received
    void handleReceive(const std::uint8_t* buffer, int len)
    {
        mReceiveBuffer.insert(mReceiveBuffer.end(), buffer, buffer + len);
        while (mReceiveBuffer.size() >= kMinPacketSize)
        {
            std::size_t magicStart = 0;
            std::size_t magicSize = 0;
            std::size_t idx = 0;
            std::size_t magicIdx = 0;
            while (idx < mReceiveBuffer.size() && magicSize < kSizeMagic)
            {
                if (kMagicSequence[magicIdx] == mReceiveBuffer[idx])
                {
                    ++magicSize;
                    ++magicIdx;
                }
                else
                {
                    magicStart = idx + 1;
                    magicSize = 0;
                    magicIdx = 0;
                }

                ++idx;
            }

            if (magicStart > 0)
            {
                // Remove non-magic bytes
                mReceiveBuffer.erase(mReceiveBuffer.begin(), mReceiveBuffer.begin() + magicStart);
                if (mReceiveBuffer.size() < kMinPacketSize)
                {
                    // Not large enough for a full packet
                    return;
                }
            }

            std::uint16_t size = bytesToUint16(&mReceiveBuffer[kSizeMagic]);
            std::uint16_t sizeInv = bytesToUint16(&mReceiveBuffer[kSizeMagic + 2]);
            if ((size ^ sizeInv) != 0xFFFF || size < (kMinSizeAddress + kSizeCrc))
            {
                // Invalid size inverse, discard first byte and retry
                mReceiveBuffer.erase(mReceiveBuffer.begin(), mReceiveBuffer.begin() + 1);
                continue;
            }

            // Check if full payload is available
            if (mReceiveBuffer.size() < (kSizeMagic + kSizeSize + size))
            {
                // Wait for more data
                return;
            }

            // Check CRC
            std::size_t pktSize = kSizeMagic + kSizeSize + size;
            const std::uint16_t receivedCrc = bytesToUint16(&mReceiveBuffer[pktSize - kSizeCrc]);
            const std::uint16_t computedCrc =
                computeCrc16(&mReceiveBuffer[kSizeMagic + kSizeSize], size - kSizeCrc);

            if (receivedCrc != computedCrc)
            {
                // Invalid CRC, discard first byte and retry
                mReceiveBuffer.erase(mReceiveBuffer.begin(), mReceiveBuffer.begin() + 1);
                continue;
            }

            // Ready to fill the packet
            IncomingData packet;

            // Extract address (variable-length, 7 bits per byte, MSb=1 if more bytes follow)
            std::int8_t addrLen = 0;
            bool lastByteBreak = false;
            std::size_t maxAddrSize = mReceiveBuffer.size() - kSizeMagic - kSizeSize - kSizeCrc;
            if (maxAddrSize > static_cast<std::size_t>(kMaxSizeAddress))
            {
                maxAddrSize = static_cast<std::size_t>(kMaxSizeAddress);
            }
            for (
                std::int8_t i = 0;
                static_cast<std::size_t>(i) < maxAddrSize;
                ++i
            ) {
                const std::uint8_t mask = (i < (kMaxSizeAddress - 1)) ? 0x7f : 0xff;
                const std::uint8_t thisByte = mReceiveBuffer[kSizeMagic + kSizeSize + i];
                packet.addr |= (thisByte & mask) << (7 * i);
                ++addrLen;
                if ((thisByte & 0x80) == 0)
                {
                    lastByteBreak = true;
                    break;
                }
            }
            if (mReceiveBuffer.size() <= (kSizeMagic + kSizeSize + addrLen + kSizeCrc))
            {
                // Missing command byte, discard first byte and retry
                mReceiveBuffer.erase(mReceiveBuffer.begin(), mReceiveBuffer.begin() + 1);
                continue;
            }

            // Extract command
            packet.cmd = mReceiveBuffer[kSizeMagic + kSizeSize + addrLen];

            // Extract payload
            const std::size_t beginIdx = kSizeMagic + kSizeSize + addrLen + kSizeCommand;
            const std::size_t endIdx = kSizeMagic + kSizeSize + size - kSizeCrc;
            packet.payload.assign(
                mReceiveBuffer.begin() + beginIdx,
                mReceiveBuffer.begin() + endIdx
            );

            // Erase this packet from data
            mReceiveBuffer.erase(
                mReceiveBuffer.begin(),
                mReceiveBuffer.begin() + kSizeMagic + kSizeSize + size
            );

            // Process the data
            {
                std::unique_lock<std::mutex> lock(mProcessMutex);
                mIncomingPackets.push_back(std::move(packet));
                mProcessCv.notify_all();
            }
        }
    }

    //! The entrypoint for mProcessThread
    //! All callbacks are executed from this context
    void processEntrypoint()
    {
        while (true)
        {
            std::list<OutgoingData> dataToSend;
            std::list<std::function<void(std::int16_t cmd, std::vector<std::uint8_t>&)>> timeoutFns;
            std::list<std::function<void(std::uint8_t cmd, std::vector<std::uint8_t>& payload)>> respFns;
            std::list<IncomingData> receivedPackets;

            {
                std::unique_lock<std::mutex> lock(mProcessMutex);

                bool waitResult = true;

                if (mTimeoutLookup.empty())
                {
                    mProcessCv.wait(
                        lock,
                        [this]()
                        {
                            return !mProcessing || !mOutgoingData.empty() || !mIncomingPackets.empty();
                        }
                    );
                }
                else
                {
                    std::chrono::system_clock::time_point nextTimePoint = mTimeoutLookup.begin()->first;
                    waitResult = mProcessCv.wait_until(
                        lock,
                        nextTimePoint,
                        [this]()
                        {
                            return !mProcessing || !mOutgoingData.empty() || !mIncomingPackets.empty();
                        }
                    );
                }

                if (!mProcessing)
                {
                    break;
                }
                else if (waitResult)
                {
                    if (!mOutgoingData.empty())
                    {
                        dataToSend = std::move(mOutgoingData);
                        mOutgoingData.clear();
                    }

                    if (!mIncomingPackets.empty())
                    {
                        // Accumulate received packets and response functions
                        receivedPackets = std::move(mIncomingPackets);
                        mIncomingPackets.clear();
                        for (auto iter = receivedPackets.begin(); iter != receivedPackets.end();)
                        {
                            FunctionLookupMap::iterator fnIter = mFnLookup.find(iter->addr);
                            if (fnIter != mFnLookup.end())
                            {
                                respFns.push_back(std::move(fnIter->second.callback));
                                mTimeoutLookup.erase(fnIter->second.timeoutMapIter);
                                mFnLookup.erase(fnIter);
                                ++iter;
                            }
                            else
                            {
                                // Nothing around to process this packet (must have timed out)
                                iter = receivedPackets.erase(iter);
                            }
                        }
                    }
                }
                else
                {
                    // Accumulate timeout functions
                    std::chrono::system_clock::time_point now = std::chrono::system_clock::now();

                    for (auto iter = mTimeoutLookup.begin(); iter != mTimeoutLookup.end();)
                    {
                        if (now < iter->first)
                        {
                            break;
                        }

                        FunctionLookupMap::iterator fnLookupIter = mFnLookup.find(iter->second);
                        if (fnLookupIter != mFnLookup.end())
                        {
                            timeoutFns.push_back(std::move(fnLookupIter->second.callback));
                            mFnLookup.erase(fnLookupIter);
                        }

                        iter = mTimeoutLookup.erase(iter);
                    }
                }
            }

            // Execute send
            std::list<std::uint64_t> sendFailureAddresses;
            for (OutgoingData& data : dataToSend)
            {
                if (!mLibusbDevice->send(&data.packet[0], static_cast<int>(data.packet.size())))
                {
                    sendFailureAddresses.push_back(data.addr);
                }
            }

            if (!sendFailureAddresses.empty())
            {
                std::list<std::function<void(std::int16_t cmd, std::vector<std::uint8_t>&)>> sendFailureFns;

                {
                    std::unique_lock<std::mutex> lock(mProcessMutex);

                    for (const std::uint64_t& addr: sendFailureAddresses)
                    {
                        FunctionLookupMap::iterator fnIter = mFnLookup.find(addr);
                        if (fnIter != mFnLookup.end())
                        {
                            sendFailureFns.push_back(std::move(fnIter->second.callback));
                            mTimeoutLookup.erase(fnIter->second.timeoutMapIter);
                            mFnLookup.erase(fnIter);
                        }
                    }
                }

                // Execute send failure functions
                for (const std::function<void(std::int16_t cmd, const std::vector<std::uint8_t>)>& fn : sendFailureFns)
                {
                    fn(::dpp_api::msg::rx::Msg::kCmdSendFailure, {});
                }
            }

            // Execute for response
            auto respFnIter = respFns.begin();
            auto pktIter = receivedPackets.begin();
            for (; respFnIter != respFns.end() && pktIter != receivedPackets.end(); ++respFnIter, ++pktIter)
            {
                if (*respFnIter)
                {
                    (*respFnIter)(pktIter->cmd, pktIter->payload);
                }
            }

            // Execute for timeout
            for (const std::function<void(std::int16_t cmd, const std::vector<std::uint8_t>)>& fn : timeoutFns)
            {
                fn(::dpp_api::msg::rx::Msg::kCmdTimeout, {});
            }
        }

        // Accumulate all hanging functions
        std::list<std::function<void(std::int16_t cmd, std::vector<std::uint8_t>&)>> disconnectFns;

        {
            std::unique_lock<std::mutex> lock(mProcessMutex);

            for (FunctionLookupMap::reference entry : mFnLookup)
            {
                disconnectFns.push_back(std::move(entry.second.callback));
            }

            mFnLookup.clear();
            mTimeoutLookup.clear();
        }

        // Execute for disconnect
        for (const std::function<void(std::int16_t cmd, const std::vector<std::uint8_t>)>& fn : disconnectFns)
        {
            fn(::dpp_api::msg::rx::Msg::kCmdDisconnect, {});
        }

        // Execute disconnection callback
        std::function<void(std::string& errStr)> disconnectCallback;
        std::string disconnectReason;

        {
            std::lock_guard<std::mutex> lock(mDisconnectMutex);
            disconnectCallback = mDisconnectCallback;
            disconnectReason = std::move(mDisconnectReason);
            mDisconnectReason.clear();
        }

        if (disconnectCallback)
        {
            disconnectCallback(disconnectReason);
        }
    }

private:
    std::unique_ptr<LibusbDevice> mLibusbDevice;

    //! The map entry for callback lookup
    struct FunctionLookupMapEntry
    {
        //! The callback to use when this message is received
        std::function<void(std::int16_t cmd, std::vector<std::uint8_t>& payload)> callback;
        //! Iterator into the timeout map which should be removed once the message is received
        std::multimap<std::chrono::system_clock::time_point, std::uint64_t>::iterator timeoutMapIter;
    };

    //! The map type which links return address to FunctionLookupMapEntry
    using FunctionLookupMap = std::unordered_map<std::uint64_t, FunctionLookupMapEntry>;

    //! True when connected, false when disconnected
    bool mConnected = false;
    //! The callback to execute when processing thread is exiting
    std::function<void(std::string& errStr)> mDisconnectCallback;
    //! The error reason for disconnection
    std::string mDisconnectReason;
    //! Serializes access to mDisconnectCallback and mDisconnectReason
    std::mutex mDisconnectMutex;
    //! True while processing thread should execute
    bool mProcessing = false;
    //! Maps return address to FunctionLookupMapEntry
    FunctionLookupMap mFnLookup;
    //! This is used to organize chronologically the timeout values for each key in the above mFnLookup
    std::multimap<std::chrono::system_clock::time_point, std::uint64_t> mTimeoutLookup;
    //! Condition variable signaled when data is added to one of the lookups, waited on within mProcessThread
    std::condition_variable mProcessCv;
    //! Mutex used to serialize access to mFnLookup, mTimeoutLookup, and mProcessCv
    std::mutex mProcessMutex;
    //! The minimum value for mNextAddr
    static const std::uint64_t kMinAddr = 1;
    //! The maximum value for mNextAddr
    static std::uint64_t mMaxAddr;
    //! Next available return address
    std::uint64_t mNextAddr = kMinAddr;
    //! Mutex serializing access to mNextAddr
    std::mutex mNextAddrMutex;
    //! The read thread created on connect()
    std::unique_ptr<std::thread> mReadThread;
    //! Thread which executes send, receive callback execution, and response timeout callback execution
    std::unique_ptr<std::thread> mProcessThread;
    //! Mutex used to serialize connect() and disconnect() calls
    std::recursive_mutex mConnectionMutex;
    //! Holds received bytes not yet parsed into a packet
    std::vector<std::uint8_t> mReceiveBuffer;

    //! Outgoing data structure
    struct OutgoingData
    {
        //! Address embedded in the packet
        std::uint64_t addr = 0;
        //! Packet to send
        std::vector<std::uint8_t> packet;
    };

    //! Holds data not yet sent
    std::list<OutgoingData> mOutgoingData;

    //! Holds incoming parsed packet data
    struct IncomingData
    {
        std::uint64_t addr = 0;
        std::uint8_t cmd = 0;
        std::vector<std::uint8_t> payload;
    };

    //! Holds data to be passed to processing callbacks
    std::list<IncomingData> mIncomingPackets;
};

// (essentially, 4 byte max for address length at 7 bits of data per byte)
std::uint64_t DppDeviceImp::mMaxAddr = 0x0FFFFFFF;


//
// Message tx and rx definitions
//

std::pair<std::uint8_t, std::vector<std::uint8_t>> msg::tx::Maple32::get() const
{
    std::vector<std::uint8_t> packet8;
    packet8.reserve(packet.size() * 4);
    for (std::uint32_t word : packet)
    {
        std::uint8_t buffer[4];
        uint32ToBytes(buffer, word);
        packet8.insert(packet8.end(), buffer, buffer + 4);
    }

    return std::make_pair('0', std::move(packet8));
}

void msg::rx::Maple32::set(std::int16_t cmd, std::vector<std::uint8_t>& payload)
{
    this->cmd = cmd;

    packet.clear();
    packet.reserve(payload.size() / 4);
    for (std::size_t i = 0; (i + 4) <= payload.size(); i+=4)
    {
        packet.push_back(bytesToUint32(&payload[i]));
    }
}

std::pair<std::uint8_t, std::vector<std::uint8_t>> msg::tx::Maple::get() const
{
    return std::make_pair('0', packet);
}

void msg::rx::Maple::set(std::int16_t cmd, std::vector<std::uint8_t>& payload)
{
    this->cmd = cmd;
    packet = std::move(payload);
}

std::pair<std::uint8_t, std::vector<std::uint8_t>> msg::tx::PlayerReset::get() const
{
    std::vector<std::uint8_t> payload;
    payload.reserve(2);
    payload.push_back('-');
    if (idx >= 0)
    {
        payload.push_back(idx);
    }

    return std::make_pair('X', std::move(payload));
}

void msg::rx::PlayerReset::set(std::int16_t cmd, std::vector<std::uint8_t>& payload)
{
    this->cmd = cmd;

    if (cmd == kCmdSuccess && !payload.empty())
    {
        numReset = payload[0];
    }
}

std::pair<std::uint8_t, std::vector<std::uint8_t>> msg::tx::ChangePlayerDisplay::get() const
{
    std::vector<std::uint8_t> payload;
    payload.reserve(3);
    payload.push_back('P');
    payload.push_back(idx);
    payload.push_back(toIdx);

    return std::make_pair('X', std::move(payload));
}

void msg::rx::ChangePlayerDisplay::set(std::int16_t cmd, std::vector<std::uint8_t>& payload)
{
    this->cmd = cmd;
}

std::pair<std::uint8_t, std::vector<std::uint8_t>> msg::tx::GetDcSummary::get() const
{
    std::vector<std::uint8_t> payload;
    payload.reserve(2);
    payload.push_back('?');
    payload.push_back(idx);

    return std::make_pair('X', std::move(payload));
}

void msg::rx::GetDcSummary::set(std::int16_t cmd, std::vector<std::uint8_t>& payload)
{
    this->cmd = cmd;

    std::size_t pidx = 0;
    while (pidx < payload.size())
    {
        std::list<std::array<uint32_t, 2>> currentPeriph;
        std::array<std::uint32_t, 2> arr;
        std::size_t aidx = 0;
        // Pipe means that 4-byte function data should follow (should be in pairs)
        while (pidx < payload.size() && payload[pidx] == '|')
        {
            ++pidx; // skip past pipe
            if (pidx + 4 <= payload.size())
            {
                arr[aidx++] = bytesToUint32(&payload[pidx]);
                if (aidx >= arr.size())
                {
                    currentPeriph.push_back(std::move(arr));
                    aidx = 0;
                }
                pidx += 4;
            }
            else
            {
                // Not enough data - skip to the end
                pidx = payload.size();
            }
        }

        // Add the accumulated peripheral data
        summary.push_back(std::move(currentPeriph));

        if (pidx < payload.size())
        {
            // This is assumed to be a semicolon which terminates the current peripheral
            ++pidx;
        }
    }
}

std::pair<std::uint8_t, std::vector<std::uint8_t>> msg::tx::GetInterfaceVersion::get() const
{
    std::vector<std::uint8_t> payload(1, 'V');
    return std::make_pair('X', std::move(payload));
}

void msg::rx::GetInterfaceVersion::set(std::int16_t cmd, std::vector<std::uint8_t>& payload)
{
    this->cmd = cmd;

    if (cmd == kCmdSuccess && payload.size() >= 2)
    {
        verMajor = payload[0];
        verMinor = payload[1];
    }
}

std::pair<std::uint8_t, std::vector<std::uint8_t>> msg::tx::GetControllerState::get() const
{
    std::vector<std::uint8_t> payload;
    payload.reserve(2);
    payload.push_back('R');
    payload.push_back(idx);
    return std::make_pair('X', std::move(payload));
}

void msg::rx::GetControllerState::set(std::int16_t cmd, std::vector<std::uint8_t>& payload)
{
    this->cmd = cmd;

    if (cmd == kCmdSuccess)
    {
        if (payload.size() > 0)
        {
            controllerState.x = payload[0];
        }

        if (payload.size() > 1)
        {
            controllerState.y = payload[1];
        }

        if (payload.size() > 2)
        {
            controllerState.z = payload[2];
        }

        if (payload.size() > 3)
        {
            controllerState.rz = payload[3];
        }

        if (payload.size() > 4)
        {
            controllerState.rx = payload[4];
        }

        if (payload.size() > 5)
        {
            controllerState.ry = payload[5];
        }

        if (payload.size() > 6)
        {
            controllerState.hat = static_cast<ControllerState::DpadButtons>(payload[6]);
        }

        if (payload.size() > 10)
        {
            // Button state in little-endian order
            controllerState.buttons = (
                (static_cast<std::uint32_t>(payload[7])) |
                (static_cast<std::uint32_t>(payload[8]) << 8) |
                (static_cast<std::uint32_t>(payload[9]) << 16) |
                (static_cast<std::uint32_t>(payload[10]) << 24)
            );
        }

        if (payload.size() > 11)
        {
            controllerState.pad = payload[11];
        }
    }
}

std::pair<std::uint8_t, std::vector<std::uint8_t>> msg::tx::RefreshGamepad::get() const
{
    std::vector<std::uint8_t> payload;
    payload.reserve(2);
    payload.push_back('G');
    payload.push_back(idx);
    return std::make_pair('X', std::move(payload));
}

void msg::rx::RefreshGamepad::set(std::int16_t cmd, std::vector<std::uint8_t>& payload)
{
    this->cmd = cmd;
}

std::pair<std::uint8_t, std::vector<std::uint8_t>> msg::tx::GetConnectedGamepads::get() const
{
    std::vector<std::uint8_t> payload(1, 'O');
    return std::make_pair('X', std::move(payload));
}

void msg::rx::GetConnectedGamepads::set(std::int16_t cmd, std::vector<std::uint8_t>& payload)
{
    this->cmd = cmd;

    std::size_t idx = 0;
    while (idx < gamepadConnectionStates.size() && idx < payload.size())
    {
        gamepadConnectionStates[idx] = static_cast<GamepadConnectionState>(payload[idx]);
        ++idx;
    }
}

//
// DppDevice definitions
//

DppDevice::DppDevice(std::unique_ptr<DppDeviceImp>&& dev) : mImp(std::move(dev))
{}

DppDevice::~DppDevice()
{}

std::unique_ptr<DppDevice> DppDevice::find(const Filter& filter)
{
    std::unique_ptr<libusb_context, LibusbContextDeleter> libusbContext = make_libusb_context();

    FindResult foundDevice = find_dpp_device(libusbContext, filter);
    if (!foundDevice.dev || !foundDevice.devHandle)
    {
        return nullptr;
    }

    struct DppDeviceFactory : public DppDevice
    {
        DppDeviceFactory(std::unique_ptr<class DppDeviceImp>&& dev) : DppDevice(std::move(dev)) {}
    };

    return std::make_unique<DppDeviceFactory>(
        std::make_unique<DppDeviceImp>(
            std::make_unique<LibusbDevice>(
                foundDevice.serial,
                std::move(foundDevice.dev),
                std::move(libusbContext),
                std::move(foundDevice.devHandle)
            )
        )
    );
}

std::uint32_t DppDevice::getCount(const Filter& filter)
{
    std::unique_ptr<libusb_context, LibusbContextDeleter> libusbContext = make_libusb_context();

    Filter filterCpy = filter;
    filterCpy.idx = (std::numeric_limits<std::int32_t>::max)();
    FindResult foundDevice = find_dpp_device(libusbContext, filterCpy);

    return foundDevice.count;
}

void DppDevice::setMaxAddr(std::uint64_t maxAddr)
{
    DppDeviceImp::setMaxAddr(maxAddr);
}

const std::string& DppDevice::getSerial() const
{
    return mImp->getSerial();
}

std::array<std::uint8_t, 3> DppDevice::getVersion() const
{
    return mImp->getVersion();
}

std::string DppDevice::getLastErrorStr()
{
    return mImp->getLastErrorStr();
}

bool DppDevice::connect(const std::function<void(std::string& errStr)>& fn)
{
    return mImp->connect(fn);
}

bool DppDevice::disconnect()
{
    return mImp->disconnect();
}

std::uint64_t DppDevice::send(
    std::uint8_t cmd,
    const std::vector<std::uint8_t>& payload,
    const std::function<void(std::int16_t cmd, std::vector<std::uint8_t>& payload)>& respFn,
    std::uint32_t timeoutMs
)
{
    return mImp->send(cmd, payload, respFn, timeoutMs);
}

bool DppDevice::isConnected()
{
    return mImp->isConnected();
}

std::size_t DppDevice::getNumWaiting()
{
    return mImp->getNumWaiting();
}

int DppDevice::getInterfaceNumber()
{
    return mImp->getInterfaceNumber();
}

std::uint8_t DppDevice::getEpIn()
{
    return mImp->getEpIn();
}

std::uint8_t DppDevice::getEpOut()
{
    return mImp->getEpOut();
}

} // namespace dpp_api
