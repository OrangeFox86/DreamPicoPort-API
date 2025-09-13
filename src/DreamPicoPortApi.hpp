#pragma once

#include <memory>
#include <string>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <map>
#include <mutex>
#include <functional>
#include <chrono>
#include <condition_variable>
#include <thread>

namespace dpp_api
{

class DppDevice
{
private:
    //! Cpnstructor
    //! @param dev Pointer to internal implementation
    DppDevice(std::unique_ptr<class DppDeviceImp>&& dev);

public:
    //! Destructor
    virtual ~DppDevice();

    //! Find a device by serial
    //! @param[in] serial The requested device's serial
    //! @return pointer to the located device
    //! @return nullptr otherwise
    static std::unique_ptr<DppDevice> find(const std::string& serial);

    //! Find a device by index
    //! @param[in] idx 0-based index of device
    //! @return pointer to the located device
    //! @return nullptr otherwise
    static std::unique_ptr<DppDevice> findAtIndex(std::size_t idx);

    //! @return the number of DreamPicoPort devices
    static std::size_t getCount();

    //! Gets the serial at a device index
    //! @param[in] idx 0-based index of the device
    //! @return serial at the device index if found
    //! @return empty string otherwise
    static std::string getSerialAt(std::size_t idx);

    //! @return the serial of this device
    const std::string& getSerial() const;

    //! @return string representation of last error
    const char* getLastErrorStr();

    //! Connect to the device and start operation threads
    //! @param[in] fn When true is returned, this is the function to execute when receive completes
    //! @return false on failure and getLastErrorStr() will return error description
    //! @return true if connection succeeded
    bool connect(const std::function<void(const char* errStr)>& fn = nullptr);

    //! Disconnect from the previously connected device, stop all threads, and wait for them to join
    //! @return false on failure and getLastErrorStr() will return error description
    //! @return true if disconnection succeeded or was already disconnected
    bool disconnect();

    //! Send a raw command to DreamPicoPort
    //! @param[in] cmd Raw DreamPicoPort command
    //! @param[in] payload The payload for the command
    //! @param[in] respFn The function to call on received response or timeout
    //!                   (response cmd is kCmdTimeout on timeout or kCmdDisconnect on disconnect)
    //! @param[in] timeoutMs Duration to wait before timeout
    //! @return 0 if send failed and getLastErrorStr() will return error description
    //! @return the ID of the sent data
    std::uint64_t send(
        std::uint8_t cmd,
        const std::vector<std::uint8_t>& payload,
        const std::function<void(std::int16_t cmd, const std::vector<std::uint8_t>& payload)>& respFn,
        std::uint32_t timeoutMs = 1000
    );

    //! @return true iff currently connected
    bool isConnected();

    //! @return number of waiting responses
    std::size_t getNumWaiting();

private:
    //! Handle received data
    //! @param[in] addr The return address of the received data
    //! @param[in] cmd The command of the received data
    //! @param[in] payload The payload of the received data
    void handleReceive(std::uint64_t addr, std::uint8_t cmd, const std::vector<std::uint8_t>& payload);

public:
    //! The cmd value set in the callback when timeout occurred before response received
    static constexpr const std::int16_t kCmdTimeout = -1;
    //! The cmd value set in the callback when device disconnected before response received
    static constexpr const std::int16_t kCmdDisconnect = -2;

private:
    //! Forward declared pointer to internal implementation class
    std::unique_ptr<class DppDeviceImp> mImp;

    //! The map entry for callback lookup
    struct FunctionLookupMapEntry
    {
        //! The callback to use when this message is received
        std::function<void(std::int16_t cmd, const std::vector<std::uint8_t>& payload)> callback;
        //! Iterator into the timeout map which should be removed once the message is received
        std::multimap<std::chrono::system_clock::time_point, std::uint64_t>::iterator timeoutMapIter;
    };

    //! The map type which links return address to FunctionLookupMapEntry
    using FunctionLookupMap = std::unordered_map<std::uint64_t, FunctionLookupMapEntry>;

    //! True when connected, false when disconnected
    bool mConnected = false;
    //! Maps return address to FunctionLookupMapEntry
    FunctionLookupMap mFnLookup;
    //! This is used to organize chronologically the timeout values for each key in the above mFnLookup
    std::multimap<std::chrono::system_clock::time_point, std::uint64_t> mTimeoutLookup;
    //! The minimum value for mNextAddr
    static const std::uint64_t kMinAddr = 1;
    //! The maximum value for mNextAddr (essentially, 4 byte max for address length)
    static const std::uint64_t kMaxAddr = 0xFFFFFFF;
    //! Next available return address
    std::uint64_t mNextAddr = kMinAddr;
    //! Thread which executes response timeouts
    std::unique_ptr<std::thread> mTimeoutThread;
    //! Condition variable signaled when data is added to one of the lookups, waited on within mTimeoutThread
    std::condition_variable mTimeoutCv;
    //! Mutex used to serialize access to class data and mTimeoutCv
    std::mutex mMutex;
};

}
