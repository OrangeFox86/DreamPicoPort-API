#include <libusb.h>

#include "DreamPicoPortApi.hpp"

#include <cstdint>
#include <cstdlib>
#include <vector>
#include <thread>
#include <mutex>
#include <functional>
#include <algorithm>

namespace dpp_api
{

//
// libusb deleters
//

//! Deleter for unique_pointer of a libusb_context
struct LibusbContextDeleter
{
    inline void operator()(libusb_context* p) const
    {
        libusb_exit(p);
    }
};

//! Deleter for unique_pointer of a libusb_device_handle
struct LibusbDeviceHandleDeleter
{
    inline void operator()(libusb_device_handle* handle) const
    {
        libusb_close(handle);
    }
};

//! Deleter for unique_pointer of a libusb_device*
struct LibusbDeviceListDeleter
{
    inline void operator()(libusb_device** devs) const
    {
        libusb_free_device_list(devs, 1);
    }
};

//! Deleter for unique_pointer of a libusb_config_descriptor
struct LibusbConfigDescriptorDeleter
{
    inline void operator()(libusb_config_descriptor* config) const
    {
        libusb_free_config_descriptor(config);
    }
};

//! Deleter for unique_pointer of a libusb_transfer
struct LibusbTransferDeleter
{
    inline void operator()(libusb_transfer* transfer) const
    {
        libusb_free_transfer(transfer);
    }
};

//
// C++ libusb wrappers
//

//! Holds libusb device list
class LibusbDeviceList
{
public:
    LibusbDeviceList();

    LibusbDeviceList(const std::unique_ptr<libusb_context, LibusbContextDeleter>& libusbContext);

    void generate(const std::unique_ptr<libusb_context, LibusbContextDeleter>& libusbContext);

    std::size_t size() const;

    bool empty() const;

    libusb_device* operator[](std::size_t index) const;

    // Iterator support
    class iterator
    {
    public:
        inline iterator(libusb_device** devices, std::size_t index) : mDevices(devices), mIndex(index) {}

        inline libusb_device* operator*() const { return mDevices ? mDevices[mIndex] : nullptr; }
        inline iterator& operator++() { ++mIndex; return *this; }
        inline iterator operator++(int) { iterator tmp = *this; ++mIndex; return tmp; }
        inline bool operator==(const iterator& other) const
        {
            return mDevices == other.mDevices && mIndex == other.mIndex;
        }
        inline bool operator!=(const iterator& other) const
        {
            return mDevices != other.mDevices || mIndex != other.mIndex;
        }

    private:
        libusb_device** mDevices;
        std::size_t mIndex;
    };

    iterator begin() const;

    iterator end() const;

private:
    std::size_t mCount;
    std::unique_ptr<libusb_device*, LibusbDeviceListDeleter> mLibusbDeviceList;
};

//! @return a new unique_pointer to a libusb_context
std::unique_ptr<libusb_context, LibusbContextDeleter> make_libusb_context();

//! @return a new unique_pointer to a libusb_device_handle
std::unique_ptr<libusb_device_handle, LibusbDeviceHandleDeleter> make_libusb_device_handle(libusb_device* dev);

//! Holds libusb error and where it occurred locally
class LibusbError
{
public:
    LibusbError() = default;
    LibusbError(const LibusbError&) = default;
    LibusbError(LibusbError&&) = default;

    //! Save the error data
    //! @param[in] libusbError The libusb error number
    //! @param[in] where Where the error ocurred
    void saveError(int libusbError, const char* where);

    //! Save error only if no error is already set
    //! @param[in] libusbError The libusb error number
    //! @param[in] where Where the error ocurred
    void saveErrorIfNotSet(int libusbError, const char* where);

    //! Clear all error data
    void clearError();

    //! @return error description
    std::string getErrorDesc() const;

    //! @return description of the last experienced error
    static const char* getLibusbErrorStr(int libusbError);

private:
    //! libusb error number
    int mLastLibusbError = LIBUSB_SUCCESS;
    //! Holds a static string where the error ocurred
    const char* mWhere = nullptr;
    //! Mutex which serializes access to above data
    mutable std::mutex mMutex;
};

struct FindResult
{
    std::unique_ptr<libusb_device_descriptor> dev;
    std::unique_ptr<libusb_device_handle, LibusbDeviceHandleDeleter> devHandle;
    std::string serial;
    std::int32_t count;
};

FindResult find_dpp_device(
    const std::unique_ptr<libusb_context, LibusbContextDeleter>& libusbContext,
    const DppDevice::Filter& filter
);

//! Handles generic libusb device, 1 vendor interface
class LibusbDevice
{
public:
    //! Contains transfer data
    struct TransferData
    {
        //! Pointer to the underlying transfer data
        std::unique_ptr<libusb_transfer, LibusbTransferDeleter> tranfer;
        //! Buffer which the transfer points into
        std::vector<std::uint8_t> buffer;
    };

    //! Constructor
    //! @param[in] serial Serial number of this device
    //! @param[in] desc The device descriptor of this device
    //! @param[in] libusbContext The context of libusb
    //! @param[in] libusbDeviceHandle Handle to the device
    LibusbDevice(
        const std::string& serial,
        std::unique_ptr<libusb_device_descriptor>&& desc,
        std::unique_ptr<libusb_context, LibusbContextDeleter>&& libusbContext,
        std::unique_ptr<libusb_device_handle, LibusbDeviceHandleDeleter>&& libusbDeviceHandle
    );

    //! Destructor
    ~LibusbDevice();

    //! Opens the vendor interface of the DreamPicoPort
    //! @return true if interface was successfully claimed or was already claimed
    bool openInterface();

    //! Sends data on the vendor interface
    //! @param[in] data Buffer to send
    //! @param[in] length Number of bytes in \p data
    //! @param[in] timeoutMs Send timeout in milliseconds
    //! @return true if data was successfully sent
    bool send(std::uint8_t* data, int length, unsigned int timeoutMs = 1000);

    //! Starts the asynchronous interface and blocks until disconnect
    //! @param[in] rxFn The function to call when a full packet is received
    //! @return true if interface was open or opened and read thread was started
    bool run(const std::function<void(const std::uint8_t*, int)>& rxFn);

    //! Request stop of the read thread
    void stopRead();

    //! Closes the interface
    //! @return true if interface was closed or was already closed
    bool closeInterface();

    //! @return description of the last experienced error
    std::string getLastErrorStr() const;

    //! @return true iff the interface is currently claimed
    bool isConnected();

    //! @return USB version number {major, minor, patch}
    std::array<std::uint8_t, 3> getVersion() const;

    //! Set an error which occurs externally
    //! @param[in] where Explanation of where the error occurred
    void setExternalError(const char* where);

    //! Retrieve the currently connected interface number (first VENDOR interface)
    //! @return the connected interface number
    int getInterfaceNumber();

    //! @return the currently used IN endpoint
    std::uint8_t getEpIn();

    //! @return the currently used OUT endpoint
    std::uint8_t getEpOut();

private:
    //! Forward declaration of transfer complete callback
    //! @param[in] transfer The transfer which completed
    static void LIBUSB_CALL onLibusbTransferComplete(libusb_transfer *transfer);

    //! Called when a libusb read transfer completed
    //! @param[in] transfer The transfer that completed
    void transferComplete(libusb_transfer* transfer);

    //! Create all libusb transfers
    //! @return true iff all transfers were created
    bool createTransfers();

    //! Cancel all transfers
    void cancelTransfers();

public:
    //! The serial number of this device
    const std::string mSerial;

private:
    //! The size in bytes of each libusb transfer
    static const std::size_t kRxSize = 1100;
    //! The number of libusb transfers to create
    static const std::uint32_t kNumTransfers = 5;
    //! The device descriptor of this device
    std::unique_ptr<libusb_device_descriptor> mDesc;
    //! Pointer to the libusb context
    std::unique_ptr<libusb_context, LibusbContextDeleter> mLibusbContext;
    //! Maps transfer pointers to TransferData
    std::unordered_map<libusb_transfer*, std::unique_ptr<TransferData>> mTransferDataMap;
    //! Serializes access to mTransferDataMap
    std::recursive_mutex mTransferDataMapMutex;
    //! Pointer to the libusb device handle
    std::unique_ptr<libusb_device_handle, LibusbDeviceHandleDeleter> mLibusbDeviceHandle;
    //! True when interface is claimed
    bool mInterfaceClaimed = false;
    //! Set to true when read thread starts, set to false to cause read thread to exit
    bool mExitRequested = false;
    //! Set when RX experienced a STALL and automatic recovery should be attempted
    bool mRxStalled = false;
    //! The interface number of the WinUSB (vendor) interface
    int mInterfaceNumber = 7;
    //! The IN endpoint of mInterfaceNumber where bulk data is read
    std::uint8_t mEpIn = 0;
    //! The IN endpoint of mInterfaceNumber where bulk data is written
    std::uint8_t mEpOut = 0;
    //! The function to call whenever a packet is received
    std::function<void(const std::uint8_t*, int)> mRxFn;
    //! Contains last libusb error data
    LibusbError mLastLibusbError;
    //! Set to true on first connection in order to force reset on subsequent connection
    bool mPreviouslyConnected = false;

}; // class LibusbDevice

} // namespace dpp_api
