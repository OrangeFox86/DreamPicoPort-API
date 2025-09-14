#pragma once

#include <memory>
#include <string>
#include <cstdint>
#include <vector>
#include <list>
#include <array>
#include <unordered_map>
#include <map>
#include <mutex>
#include <functional>
#include <chrono>
#include <condition_variable>
#include <thread>

namespace dpp_api
{

//! Contains controller state data
struct ControllerState
{
    //! Enumerates hat state
    enum DpadButtons : std::uint8_t
    {
        GAMEPAD_HAT_CENTERED   = 0,  //!< DPAD_CENTERED
        GAMEPAD_HAT_UP         = 1,  //!< DPAD_UP
        GAMEPAD_HAT_UP_RIGHT   = 2,  //!< DPAD_UP_RIGHT
        GAMEPAD_HAT_RIGHT      = 3,  //!< DPAD_RIGHT
        GAMEPAD_HAT_DOWN_RIGHT = 4,  //!< DPAD_DOWN_RIGHT
        GAMEPAD_HAT_DOWN       = 5,  //!< DPAD_DOWN
        GAMEPAD_HAT_DOWN_LEFT  = 6,  //!< DPAD_DOWN_LEFT
        GAMEPAD_HAT_LEFT       = 7,  //!< DPAD_LEFT
        GAMEPAD_HAT_UP_LEFT    = 8,  //!< DPAD_UP_LEFT
    };

    //! Enumerates buttons
    enum GamepadButton : uint8_t
    {
        GAMEPAD_BUTTON_A = 0,
        GAMEPAD_BUTTON_B = 1,
        GAMEPAD_BUTTON_C = 2,
        GAMEPAD_BUTTON_X = 3,
        GAMEPAD_BUTTON_Y = 4,
        GAMEPAD_BUTTON_Z = 5,
        GAMEPAD_BUTTON_DPAD_B_R = 6,
        GAMEPAD_BUTTON_DPAD_B_L = 7,
        GAMEPAD_BUTTON_DPAD_B_D = 8,
        GAMEPAD_BUTTON_DPAD_B_U = 9,
        GAMEPAD_BUTTON_D = 10,
        GAMEPAD_BUTTON_START = 11,
        GAMEPAD_VMU1_A = 12,
        GAMEPAD_VMU1_B = 15,
        GAMEPAD_VMU1_U = 16,
        GAMEPAD_VMU1_D = 17,
        GAMEPAD_VMU1_L = 18,
        GAMEPAD_VMU1_R = 19,
        GAMEPAD_CHANGE_DETECT = 20
    };

    std::int8_t  x = 0; //!< Delta x  movement of left analog-stick
    std::int8_t  y = 0; //!< Delta y  movement of left analog-stick
    std::int8_t  z = 0; //!< Delta z  movement of left trigger
    std::int8_t  rz = 0; //!< Delta Rz movement of right tirgger
    std::int8_t  rx = 0; //!< Delta Rx movement of analog right analog-stick
    std::int8_t  ry = 0; //!< Delta Ry movement of analog right analog-stick
    DpadButtons hat = GAMEPAD_HAT_CENTERED; //!< Buttons mask for currently pressed buttons in the DPad/hat
    std::uint32_t buttons = 0; //!< Buttons mask for currently pressed buttons @see isPressed
    std::uint8_t pad = 0; //!< Vendor data (padding, set to player index)

    //! Checks if a button is pressed in this state
    //! @param[in] btn Button enumeration value to check
    //! @return true iff \p btn is pressed
    inline bool isPressed(GamepadButton btn) const
    {
        return ((buttons & (1 << btn)) != 0);
    }
};

//! Enumerates gamepad connection states
enum class GamepadConnectionState : std::uint8_t
{
    UNAVAILABLE = 0, //!< No gamepad available at this index
    NOT_CONNECTED = 1, //!< Gamepad available but not connected
    CONNECTED = 2 //!< Gamepad available and connected
};

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
    //! @note connection may fail with "Access denied" if attempt is made to connect immediately after disconnect
    //! @param[in] fn When true is returned, this is the function to execute when receive completes
    //! @return false on failure and getLastErrorStr() will return error description
    //! @return true if connection succeeded
    bool connect(const std::function<void(const char* errStr)>& fn = nullptr);

    //! Disconnect from the previously connected device and stop all threads
    //! @return false on failure and getLastErrorStr() will return error description
    //! @return true if disconnection succeeded or was already disconnected
    bool disconnect();

    //! Send a raw command to DreamPicoPort
    //! @param[in] cmd Raw DreamPicoPort command
    //! @param[in] payload The payload for the command
    //! @param[in] respFn The function to call on received response or timeout with the following arguments
    //!                   cmd: one of the kCmd* values
    //!                   payload: the returned payload
    //! @param[in] timeoutMs Duration to wait before timeout
    //! @return 0 if send failed and getLastErrorStr() will return error description
    //! @return the ID of the sent data
    std::uint64_t send(
        std::uint8_t cmd,
        const std::vector<std::uint8_t>& payload,
        const std::function<void(std::int16_t cmd, const std::vector<std::uint8_t>& payload)>& respFn,
        std::uint32_t timeoutMs = 1000
    );

    //! Send maple packet as 32-bit words
    //! @param[in] payload The maple payload which contains at least 1 word (MSB is command)
    //! @param[in] respFn The function to call on received response or timeout with the following arguments
    //!                   cmd: one of the kCmd* values
    //!                   payload: when cmd is kCmdSuccess, the returned 32-bit maple payload
    //! @param[in] timeoutMs Duration to wait before timeout
    //! @return 0 if send failed and getLastErrorStr() will return error description
    //! @return the ID of the sent data
    std::uint64_t sendMaple(
        const std::vector<std::uint32_t>& payload,
        const std::function<void(std::int16_t cmd, const std::vector<std::uint32_t>& payload)>& respFn,
        std::uint32_t timeoutMs = 1000
    );

    //! Send maple packet as 8-bit payload
    //! @param[in] payload The maple payload which contains at least 4 bytes (first byte is command)
    //! @param[in] respFn The function to call on received response or timeout with the following arguments
    //!                   cmd: one of the kCmd* values
    //!                   payload: when cmd is kCmdSuccess, the returned 8-bit maple payload
    //! @param[in] timeoutMs Duration to wait before timeout
    //! @return 0 if send failed and getLastErrorStr() will return error description
    //! @return the ID of the sent data
    std::uint64_t sendMaple(
        const std::vector<std::uint8_t>& payload,
        const std::function<void(std::int16_t cmd, const std::vector<std::uint8_t>& payload)>& respFn,
        std::uint32_t timeoutMs = 1000
    );

    //! Send player reset command
    //! @param[in] idx Player index [0,3] or -1 for all players
    //! @param[in] respFn The function to call on received response or timeout with the following arguments
    //!                   cmd: one of the kCmd* values
    //!                   numReset: number of players that have been reset
    //! @param[in] timeoutMs Duration to wait before timeout
    //! @return 0 if send failed and getLastErrorStr() will return error description
    //! @return the ID of the sent data
    std::uint64_t sendPlayerReset(
        std::int8_t idx,
        const std::function<void(std::int16_t cmd, std::uint8_t numReset)>& respFn,
        std::uint32_t timeoutMs = 1000
    );

    //! Change the player display on the upper VMU screen
    //! @param[in] idx Player index [0,3] of the target controller
    //! @param[in] toIdx Player index [0,3] to change the display to
    //! @param[in] respFn The function to call on received response or timeout with the following arguments
    //!                   cmd: one of the kCmd* values
    //! @param[in] timeoutMs Duration to wait before timeout
    //! @return 0 if send failed and getLastErrorStr() will return error description
    //! @return the ID of the sent data
    std::uint64_t sendChangePlayerDisplay(
        std::uint8_t idx,
        std::uint8_t toIdx,
        const std::function<void(std::int16_t cmd)>& respFn,
        std::uint32_t timeoutMs = 1000
    );

    //! Request Dreamcast peripheral summary
    //! @param[in] idx Player index [0,3] of the target controller
    //! @param[in] respFn The function to call on received response or timeout with the following arguments
    //!                   cmd: one of the kCmd* values
    //!                   summary: when cmd is kCmdSuccess, this contains peripheral summary data
    //!                            - Each element in the outer list represents a peripheral in order (first is main)
    //!                            - Each element in the inner list represents function definition (max of 3)
    //!                            - First array element is function code, second is function definition word
    //! @param[in] timeoutMs Duration to wait before timeout
    //! @return 0 if send failed and getLastErrorStr() will return error description
    //! @return the ID of the sent data
    std::uint64_t sendGetDcSummary(
        std::uint8_t idx,
        const std::function<void(std::int16_t cmd, const std::list<std::list<std::array<uint32_t, 2>>>& summary)>& respFn,
        std::uint32_t timeoutMs = 1000
    );

    //! Request emulator command processing interface version
    //! @param[in] respFn The function to call on received response or timeout with the following arguments
    //!                   cmd: one of the kCmd* values
    //!                   verMajor: major version number
    //!                   verMinor: minor version number
    //! @param[in] timeoutMs Duration to wait before timeout
    //! @return 0 if send failed and getLastErrorStr() will return error description
    //! @return the ID of the sent data
    std::uint64_t sendGetInterfaceVersion(
        const std::function<void(std::int16_t cmd, std::uint8_t verMajor, std::uint8_t verMinor)>& respFn,
        std::uint32_t timeoutMs = 1000
    );

    //! Request controller state
    //! @param[in] idx Player index [0,3] of the target controller
    //! @param[in] respFn The function to call on received response or timeout with the following arguments
    //!                   cmd: one of the kCmd* values
    //!                   controllerState: when cmd is kCmdSuccess, the current controller state
    //! @param[in] timeoutMs Duration to wait before timeout
    //! @return 0 if send failed and getLastErrorStr() will return error description
    //! @return the ID of the sent data
    std::uint64_t sendGetControllerState(
        std::uint8_t idx,
        const std::function<void(std::int16_t cmd, const ControllerState& controllerState)>& respFn,
        std::uint32_t timeoutMs = 1000
    );

    //! Requests that the DreamPicoPort refreshes its gamepad state over the HID interface
    //! @param[in] idx Player index [0,3] of the target controller
    //! @param[in] respFn The function to call on received response or timeout with the following arguments
    //!                   cmd: one of the kCmd* values
    //! @param[in] timeoutMs Duration to wait before timeout
    //! @return 0 if send failed and getLastErrorStr() will return error description
    //! @return the ID of the sent data
    std::uint64_t sendRefreshGamepad(
        std::uint8_t idx,
        const std::function<void(std::int16_t cmd)>& respFn,
        std::uint32_t timeoutMs = 1000
    );

    //! Requests controller connection state for each controller
    //! @param[in] respFn The function to call on received response or timeout with the following arguments
    //!                   cmd: one of the kCmd* values
    //!                   gamepadConnectionStates: controller connection state for each controller
    //! @param[in] timeoutMs Duration to wait before timeout
    //! @return 0 if send failed and getLastErrorStr() will return error description
    //! @return the ID of the sent data
    std::uint64_t sendGetConnectedGamepads(
        const std::function<void(std::int16_t cmd, const std::array<GamepadConnectionState, 4>& gamepadConnectionStates)>& respFn,
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
    //! The cmd response for successful execution
    static constexpr const std::int16_t kCmdSuccess = 0x0A;
    //! The cmd response for execution complete with warning
    static constexpr const std::int16_t kCmdAttention = 0x0B;
    //! The cmd response for execution failure
    static constexpr const std::int16_t kCmdFailure = 0x0F;
    //! The cmd response for invalid command or missing data
    static constexpr const std::int16_t kCmdInvalid = 0xFE;
    //! The cmd response value set in the callback when timeout occurred before response received
    static constexpr const std::int16_t kCmdTimeout = -1;
    //! The cmd response value set in the callback when device disconnected before response received
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
    //! The maximum value for mNextAddr (essentially, 4 byte max for address length at 7 bits of data per byte)
    static const std::uint64_t kMaxAddr = 0x0FFFFFFF;
    //! Next available return address
    std::uint64_t mNextAddr = kMinAddr;
    //! Thread which executes response timeouts
    std::unique_ptr<std::thread> mTimeoutThread;
    //! Condition variable signaled when data is added to one of the lookups, waited on within mTimeoutThread
    std::condition_variable mTimeoutCv;
    //! Mutex used to serialize access to mFnLookup, mTimeoutLookup, and mTimeoutCv
    std::mutex mTimeoutMutex;
    //! Mutex used to serialize access to class data
    std::recursive_mutex mMutex;
};

}
