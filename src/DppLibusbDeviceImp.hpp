#ifndef __DREAM_PICO_PORT_LIBUSB_DEVICE_IMP_HPP__
#define __DREAM_PICO_PORT_LIBUSB_DEVICE_IMP_HPP__

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

#include <string>
#include <list>
#include <vector>
#include <cstdint>
#include <functional>
#include <memory>

#include "DppDeviceImp.hpp"
#include "LibusbWrappers.hpp"

namespace dpp_api
{
//! This class hides implementation details to simplify the public interface.
class DppLibUsbDeviceImp : public DppDeviceImp
{
public:
    DppLibUsbDeviceImp(std::unique_ptr<LibusbDevice>&& dev);
    virtual ~DppLibUsbDeviceImp();

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
    ) override;

    //! Connect to the device and start operation threads. If already connected, disconnect before reconnecting.
    //! @param[in] fn When true is returned, this is the function that will execute when the device is disconnected
    //!               errStr: the reason for disconnection or empty string if disconnect() was called
    //!               NOTICE: Any attempt to call connect() within any callback function will always fail
    //! @return false on failure and getLastErrorStr() will return error description
    //! @return true if connection succeeded
    bool connect(const std::function<void(std::string& errStr)>& fn) override;

    //! Disconnect from the previously connected device and stop all threads
    //! @return false on failure and getLastErrorStr() will return error description
    //! @return true if disconnection succeeded or was already disconnected
    bool disconnect() override;

    //! @return true iff currently connected
    bool isConnected() override;

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
    ) override;

    //! @return the serial of this device
    const std::string& getSerial() const override;

    //! @return USB version number {major, minor, patch}
    std::array<std::uint8_t, 3> getVersion() const override;

    //! @return string representation of last error
    std::string getLastErrorStr() override;

    //! @return number of waiting responses
    std::size_t getNumWaiting() override;

    //! Set an error which occurs externally
    //! @param[in] where Explanation of where the error occurred
    void setExternalError(const char* where) override;

    //! Retrieve the currently connected interface number (first VENDOR interface)
    //! @return the connected interface number
    int getInterfaceNumber() override;

    //! @return the currently used IN endpoint
    std::uint8_t getEpIn() override;

    //! @return the currently used OUT endpoint
    std::uint8_t getEpOut() override;

private:
    //! Packs a packet structure into a vector
    //! @param[in] addr The return address
    //! @param[in] cmd The command to set
    //! @param[in] payload The payload for the command
    //! @return the packed data
    static std::vector<std::uint8_t> pack(
        std::uint64_t addr,
        std::uint8_t cmd,
        const std::vector<std::uint8_t>& payload
    );

    //! Handle received data
    //! @param[in] buffer Buffer received from libusb
    //! @param[in] len Number of bytes in buffer received
    void handleReceive(const std::uint8_t* buffer, int len);

    //! The entrypoint for mProcessThread
    //! All callbacks are executed from this context
    void processEntrypoint();

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
}

#endif // __DREAM_PICO_PORT_LIBUSB_DEVICE_IMP_HPP__
