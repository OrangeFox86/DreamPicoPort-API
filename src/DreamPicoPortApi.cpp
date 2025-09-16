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

#include "libusb.h"

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
    void operator()(libusb_context* p) const
    {
        libusb_exit(p);
    }
};

//! Deleter for unique_pointer of a libusb_device_handle
struct LibusbDeviceHandleDeleter
{
    void operator()(libusb_device_handle* handle) const
    {
        libusb_close(handle);
    }
};

//! Deleter for unique_pointer of a libusb_device*
struct LibusbDeviceListDeleter
{
    void operator()(libusb_device** devs) const
    {
        libusb_free_device_list(devs, 1);
    }
};

//! Deleter for unique_pointer of a libusb_config_descriptor
struct LibusbConfigDescriptorDeleter
{
    void operator()(libusb_config_descriptor* config) const
    {
        libusb_free_config_descriptor(config);
    }
};

//! Deleter for unique_pointer of a libusb_transfer
struct LibusbTransferDeleter
{
    void operator()(libusb_transfer* transfer) const
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
    LibusbDeviceList() : mCount(0), mLibusbDeviceList() {}

    LibusbDeviceList(const std::unique_ptr<libusb_context, LibusbContextDeleter>& libusbContext) : LibusbDeviceList()
    {
        generate(libusbContext);
    }

    void generate(const std::unique_ptr<libusb_context, LibusbContextDeleter>& libusbContext)
    {
        libusb_device **devs;
        ssize_t cnt = libusb_get_device_list(libusbContext.get(), &devs);
        if (cnt >= 0)
        {
            mLibusbDeviceList.reset(devs);
            mCount = static_cast<std::size_t>(cnt);
        }
        else
        {
            mCount = 0;
            mLibusbDeviceList.reset();
        }
    }

    std::size_t size() const
    {
        return mCount;
    }

    bool empty() const
    {
        return (mCount == 0);
    }

    libusb_device* operator[](std::size_t index) const
    {
        if (index >= mCount)
        {
            return nullptr;
        }
        return mLibusbDeviceList.get()[index];
    }

    // Iterator support
    class iterator
    {
    public:
        iterator(libusb_device** devices, std::size_t index) : mDevices(devices), mIndex(index) {}

        libusb_device* operator*() const { return mDevices ? mDevices[mIndex] : nullptr; }
        iterator& operator++() { ++mIndex; return *this; }
        iterator operator++(int) { iterator tmp = *this; ++mIndex; return tmp; }
        bool operator==(const iterator& other) const { return mDevices == other.mDevices && mIndex == other.mIndex; }
        bool operator!=(const iterator& other) const { return mDevices != other.mDevices || mIndex != other.mIndex; }

    private:
        libusb_device** mDevices;
        std::size_t mIndex;
    };

    iterator begin() const
    {
        return iterator(mLibusbDeviceList.get(), 0);
    }

    iterator end() const
    {
        return iterator(mLibusbDeviceList.get(), mCount);
    }

private:
    std::size_t mCount;
    std::unique_ptr<libusb_device*, LibusbDeviceListDeleter> mLibusbDeviceList;
};

//! @return a new unique_pointer to a libusb_context
static std::unique_ptr<libusb_context, LibusbContextDeleter> make_libusb_context()
{
    std::unique_ptr<libusb_context, LibusbContextDeleter> libusbContext;

    {
        libusb_context *ctx = nullptr;
        int r = libusb_init(&ctx);
        if (r < 0)
        {
            return nullptr;
        }
        libusbContext.reset(ctx);
    }
    return libusbContext;
}

//! @return a new unique_pointer to a libusb_device_handle
static std::unique_ptr<libusb_device_handle, LibusbDeviceHandleDeleter> make_libusb_device_handle(libusb_device* dev)
{
    std::unique_ptr<libusb_device_handle, LibusbDeviceHandleDeleter> deviceHandle;

    {
        libusb_device_handle *handle;
        int r = libusb_open(dev, &handle);
        if (r >= 0)
        {
            deviceHandle.reset(handle);
        }
    }

    return deviceHandle;
}

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
    void saveError(int libusbError, const char* where)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mLastLibusbError = libusbError;
        mWhere = where;
    }

    //! Save error only if no error is already set
    //! @param[in] libusbError The libusb error number
    //! @param[in] where Where the error ocurred
    void saveErrorIfNotSet(int libusbError, const char* where)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mLastLibusbError != LIBUSB_SUCCESS)
        {
            mLastLibusbError = libusbError;
            mWhere = where;
        }
    }

    //! Clear all error data
    void clearError()
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mLastLibusbError = LIBUSB_SUCCESS;
        mWhere = nullptr;
    }

    //! @return error description
    std::string getErrorDesc() const
    {
        int libusbError = 0;
        const char* where = nullptr;

        {
            std::lock_guard<std::mutex> lock(mMutex);
            libusbError = mLastLibusbError;
            where = mWhere;
        }

        const char* libusbErrorStr = getLibusbErrorStr(libusbError);

        if (where && *where != '\0')
        {
            return std::string(libusbErrorStr) + std::string(" @ ") + where;
        }

        return std::string(libusbErrorStr);
    }

    //! @return description of the last experienced error
    static const char* getLibusbErrorStr(int libusbError)
    {
        switch (libusbError)
        {
            case LIBUSB_SUCCESS: return "";
            case LIBUSB_ERROR_IO: return "Input/Output error";
            case LIBUSB_ERROR_INVALID_PARAM: return "Invalid parameter (internal fault)";
#ifdef __linux__
            case LIBUSB_ERROR_ACCESS: return "Access denied (check permissions or udev rules)";
#else
            case LIBUSB_ERROR_ACCESS: return "Access denied";
#endif
            case LIBUSB_ERROR_NO_DEVICE: return "Device not found or disconnected";
            case LIBUSB_ERROR_NOT_FOUND: return "Device, interface, or endpoint not found";
            case LIBUSB_ERROR_BUSY: return "Device is busy";
            case LIBUSB_ERROR_TIMEOUT: return "Timeout occurred";
            case LIBUSB_ERROR_OVERFLOW: return "Overflow occurred";
            case LIBUSB_ERROR_PIPE: return "Pipe error";
            case LIBUSB_ERROR_INTERRUPTED: return "Operation was interrupted";
            case LIBUSB_ERROR_NO_MEM: return "Insufficient memory";
            case LIBUSB_ERROR_NOT_SUPPORTED: return "Operation not supported or unimplemented on this platform";
            case LIBUSB_ERROR_OTHER: return "Undefined error";
            default:
                return libusb_error_name(libusbError);
        }
    }

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
)
{
    LibusbDeviceList deviceList(libusbContext);
    std::unique_ptr<libusb_device_descriptor> desc = std::make_unique<libusb_device_descriptor>();
    std::int32_t currentIndex = 0;

    for (libusb_device* dev : deviceList)
    {
        int r = libusb_get_device_descriptor(dev, desc.get());
        if (r < 0)
        {
            continue;
        }

        if (desc->idVendor != filter.idVendor || desc->idProduct != filter.idProduct)
        {
            continue;
        }

        if (desc->bcdDevice < filter.minBcdDevice || desc->bcdDevice > filter.maxBcdDevice)
        {
            continue;
        }

        std::unique_ptr<libusb_device_handle, LibusbDeviceHandleDeleter> deviceHandle = make_libusb_device_handle(dev);
        if (!deviceHandle)
        {
            continue;
        }

        std::string deviceSerial;
        unsigned char serialString[256] = {};
        if (desc->iSerialNumber > 0)
        {
            r = libusb_get_string_descriptor_ascii(
                deviceHandle.get(),
                desc->iSerialNumber,
                serialString,
                sizeof(serialString)
            );

            if (r >= 0)
            {
                deviceSerial.assign(reinterpret_cast<char*>(serialString));
                if (filter.serial.empty() || deviceSerial == filter.serial)
                {
                    if (filter.idx < 0 || filter.idx == currentIndex)
                    {
                        return FindResult{
                            std::move(desc),
                            std::move(deviceHandle),
                            std::move(deviceSerial),
                            currentIndex + 1
                        };
                    }
                    else
                    {
                        ++currentIndex;
                    }
                }
            }
        }
    }

    return FindResult{
        nullptr,
        nullptr,
        std::string(),
        currentIndex
    };
}

//! Implementation class for DppDevice
class DppDeviceImp
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
    DppDeviceImp(
        const std::string& serial,
        std::unique_ptr<libusb_device_descriptor>&& desc,
        std::unique_ptr<libusb_context, LibusbContextDeleter>&& libusbContext,
        std::unique_ptr<libusb_device_handle, LibusbDeviceHandleDeleter>&& libusbDeviceHandle
    ) :
        mSerial(serial),
        mDesc(std::move(desc)),
        mLibusbContext(std::move(libusbContext)),
        mLibusbDeviceHandle(std::move(libusbDeviceHandle))
    {
    }

    //! Destructor
    ~DppDeviceImp()
    {
        closeInterface();
        joinRead();

        // Reset libusb pointers in the correct order
        mLibusbDeviceHandle.reset();
        mTransferDataMap.clear();
        mLibusbContext.reset();
    }

    //! Opens the vendor interface of the DreamPicoPort
    //! @return true if interface was successfully claimed or was already claimed
    bool openInterface()
    {
        if (mInterfaceClaimed)
        {
            return true;
        }

        mLastLibusbError.clearError();

        if (mPreviouslyConnected || !mLibusbDeviceHandle)
        {
            // Reset and attempt to reconnect
            mLibusbDeviceHandle.reset();

            DppDevice::Filter filter;
            filter.idVendor = mDesc->idVendor;
            filter.idProduct = mDesc->idProduct;
            filter.minBcdDevice = mDesc->bcdDevice;
            filter.maxBcdDevice = mDesc->bcdDevice;
            filter.serial = mSerial;
            FindResult foundDevice = find_dpp_device(mLibusbContext, filter);
            if (!foundDevice.dev || !foundDevice.devHandle)
            {
                mLastLibusbError.saveError(LIBUSB_ERROR_NO_DEVICE, "find_dpp_device");
                return false;
            }

            mDesc = std::move(foundDevice.dev);
            mLibusbDeviceHandle = std::move(foundDevice.devHandle);
        }

        mPreviouslyConnected = true;

        // Dynamically retrieve endpoint addresses for the interface
        std::unique_ptr<libusb_config_descriptor, LibusbConfigDescriptorDeleter> configDescriptor;

        {
            libusb_config_descriptor *config;
            int r = libusb_get_active_config_descriptor(libusb_get_device(mLibusbDeviceHandle.get()), &config);
            if (r < 0)
            {
                mLastLibusbError.saveError(r, "libusb_get_active_config_descriptor");
                return false;
            }
            configDescriptor.reset(config);
        }

        const libusb_interface *selectedInterface = nullptr;
        for (std::uint8_t i = 0; i < configDescriptor->bNumInterfaces; ++i)
        {
            const libusb_interface *itf = &configDescriptor->interface[i];
            if (itf->num_altsetting > 0 && itf->altsetting[0].bInterfaceClass == LIBUSB_CLASS_VENDOR_SPEC)
            {
                selectedInterface = itf;
                break;
            }
        }

        if (!selectedInterface || selectedInterface->num_altsetting <= 0)
        {
            mLastLibusbError.saveError(LIBUSB_ERROR_NOT_FOUND, "find vendor interface");
            return false;
        }

        std::int16_t outEndpoint = -1;
        std::int16_t inEndpoint = -1;

        const libusb_interface_descriptor *altsetting = &selectedInterface->altsetting[0];
        for (int i = 0; i < altsetting->bNumEndpoints; i++)
        {
            const libusb_endpoint_descriptor *endpoint = &altsetting->endpoint[i];
            if ((endpoint->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) == LIBUSB_TRANSFER_TYPE_BULK)
            {
                if (endpoint->bEndpointAddress & LIBUSB_ENDPOINT_IN)
                {
                    inEndpoint = endpoint->bEndpointAddress;
                }
                else
                {
                    outEndpoint = endpoint->bEndpointAddress;
                }
            }
        }

        if (outEndpoint < 0 || inEndpoint < 0)
        {
            mLastLibusbError.saveError(LIBUSB_ERROR_NOT_FOUND, "find endpoints");
            return false;
        }

        mInterfaceNumber = selectedInterface->altsetting->bInterfaceNumber;
        mEpOut = static_cast<std::uint8_t>(outEndpoint);
        mEpIn = static_cast<std::uint8_t>(inEndpoint);
        configDescriptor.reset();

        int r = libusb_claim_interface(mLibusbDeviceHandle.get(), mInterfaceNumber);
        if (r < 0)
        {
            // Handle error - interface claim failed
            mLastLibusbError.saveError(r, "libusb_claim_interface");
            return false;
        }

        // Set up control transfer for connect message (clears buffers)
        r = libusb_control_transfer(
            mLibusbDeviceHandle.get(),
            LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_OUT,
            0x22, // bRequest
            0x01, // wValue (connection)
            mInterfaceNumber, // wIndex
            nullptr, // data buffer
            0,    // wLength
            1000  // timeout in milliseconds
        );

        if (r < 0)
        {
            // Handle control transfer error
            libusb_release_interface(mLibusbDeviceHandle.get(), mInterfaceNumber);
            mLastLibusbError.saveError(r, "libusb_control_transfer on connect");
            return false;
        }

        mInterfaceClaimed = true;
        return true;
    }

    //! Sends data on the vendor interface
    //! @param[in] addr The return address
    //! @param[in] cmd The command to set
    //! @param[in] payload The payload for the command
    //! @return true if data was successfully sent
    bool send(std::uint64_t addr, std::uint8_t cmd, const std::vector<std::uint8_t>& payload)
    {
        if (!openInterface() || !mLibusbDeviceHandle)
        {
            return false;
        }

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

        // Transfer the package
        int transferred;
        int r = libusb_bulk_transfer(
            mLibusbDeviceHandle.get(),
            mEpOut, // OUT endpoint
            const_cast<unsigned char*>(data.data()),
            static_cast<int>(data.size()),
            &transferred,
            1000  // timeout in milliseconds
        );

        if (r < 0)
        {
            mLastLibusbError.saveError(r, "libusb_bulk_transfer on send");
            return false;
        }
        else if (transferred != static_cast<int>(data.size()))
        {
            r = LIBUSB_ERROR_IO;
            return false;
        }

        return true;
    }

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

    //! Process data from mReceiveBuffer into packets
    void processPackets()
    {
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

            // Extract address (variable-length, 7 bits per byte, MSb=1 if more bytes follow)
            std::uint64_t addr = 0;
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
              addr |= (thisByte & mask) << (7 * i);
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
            const std::uint8_t cmd = mReceiveBuffer[kSizeMagic + kSizeSize + addrLen];

            // Extract payload
            const std::size_t beginIdx = kSizeMagic + kSizeSize + addrLen + kSizeCommand;
            const std::size_t endIdx = kSizeMagic + kSizeSize + size - kSizeCrc;
            std::vector<std::uint8_t> payload(
                mReceiveBuffer.begin() + beginIdx,
                mReceiveBuffer.begin() + endIdx
            );

            // Erase this packet from data
            mReceiveBuffer.erase(
                mReceiveBuffer.begin(),
                mReceiveBuffer.begin() + kSizeMagic + kSizeSize + size
            );

            // Process the data
            if (mRxFn)
            {
                mRxFn(addr, cmd, payload);
            }
        }
    }

    //! Forward declaration of transfer complete callback
    //! @param[in] transfer The transfer which completed
    static void LIBUSB_CALL onLibusbTransferComplete(libusb_transfer *transfer)
    {
        DppDeviceImp* dppDeviceImp = static_cast<DppDeviceImp*>(transfer->user_data);
        dppDeviceImp->transferComplete(transfer);
    }

    //! Called when a libusb read transfer completed
    //! @param[in] transfer The transfer that completed
    void transferComplete(libusb_transfer* transfer)
    {
        if (transfer->status == LIBUSB_TRANSFER_COMPLETED && mRxFn && transfer->actual_length > 0)
        {
            mReceiveBuffer.insert(mReceiveBuffer.end(), transfer->buffer, transfer->buffer + transfer->actual_length);
            processPackets();

            transfer->actual_length = 0;
        }

        bool stallDetected = mRxStalled;
        bool allowNewTransfer = false;

        if (mInterfaceClaimed && !mExitRequested)
        {
            switch (transfer->status)
            {
                case LIBUSB_TRANSFER_COMPLETED:
                {
                    allowNewTransfer = true;
                }
                break;

                case LIBUSB_TRANSFER_TIMED_OUT:
                {
                    // retry
                    allowNewTransfer = true;
                }
                break;

                case LIBUSB_TRANSFER_STALL:
                {
                    mLastLibusbError.saveErrorIfNotSet(LIBUSB_ERROR_IO, "transfer in - stall");
                    allowNewTransfer = false;
                    // Set stallDetected which will prevent device from closing unless other errors occur
                    stallDetected = true;
                }
                break;

                case LIBUSB_TRANSFER_ERROR:
                {
                    mLastLibusbError.saveErrorIfNotSet(LIBUSB_ERROR_IO, "transfer in - error");
                    allowNewTransfer = false;
                }
                break;

                case LIBUSB_TRANSFER_CANCELLED:
                {
                    mLastLibusbError.saveErrorIfNotSet(LIBUSB_ERROR_IO, "transfer in - cancelled");
                    allowNewTransfer = false;
                }
                break;

                case LIBUSB_TRANSFER_NO_DEVICE:
                {
                    mLastLibusbError.saveErrorIfNotSet(LIBUSB_ERROR_IO, "transfer in - couldn't find device");
                    allowNewTransfer = false;
                }
                break;

                case LIBUSB_TRANSFER_OVERFLOW:
                {
                    mLastLibusbError.saveErrorIfNotSet(LIBUSB_ERROR_IO, "transfer in - overflow");
                    allowNewTransfer = false;
                }
                break;

                default:
                {
                    mLastLibusbError.saveErrorIfNotSet(LIBUSB_ERROR_IO, "transfer in - unknown error");
                    allowNewTransfer = false;
                }
                break;
            }
        }
        else
        {
            mLastLibusbError.saveErrorIfNotSet(LIBUSB_ERROR_IO, "transfer in - device closing");
            allowNewTransfer = false;
        }

        bool transferSubmitted = false;

        if (allowNewTransfer)
        {
            // Submit new transfer
            int r = libusb_submit_transfer(transfer);
            if (r < 0)
            {
                // Failure
                mLastLibusbError.saveError(r, "libusb_submit_transfer on transfer");
            }
            else
            {
                transferSubmitted = true;
            }
        }

        if (!transferSubmitted)
        {
            std::lock_guard<std::recursive_mutex> lock(mTransferDataMapMutex);

            // Erase the transfer from the map which should automatically free the transfer data
            mTransferDataMap.erase(transfer);

            if (stallDetected)
            {
                if (!mRxStalled)
                {
                    // Only cancel all other transfers without completely stopping read
                    mRxStalled = true;
                    cancelTransfers();
                }
            }
            else
            {
                // Cancel all other transfers
                stopRead();
            }
        }
    }

    //! Create all libusb transfers
    //! @return true iff all transfers were created
    bool createTransfers()
    {
        bool success = true;

        for (std::uint32_t i = 0; i < kNumTransfers; ++i)
        {
            std::unique_ptr<TransferData> transferData;

            {
                libusb_transfer *transfer = libusb_alloc_transfer(0);
                if (!transfer)
                {
                    mLastLibusbError.saveError(LIBUSB_ERROR_NO_MEM, "libusb_alloc_transfer");
                    success = false;
                    break;
                }
                transferData = std::make_unique<TransferData>();
                transferData->tranfer.reset(transfer);
            }

            transferData->buffer.resize(kRxSize);

            libusb_fill_bulk_transfer(
                transferData->tranfer.get(),
                mLibusbDeviceHandle.get(),
                mEpIn,
                &transferData->buffer[0],
                transferData->buffer.size(),
                DppDeviceImp::onLibusbTransferComplete,
                this,
                0
            );

            {
                std::lock_guard<std::recursive_mutex> lock(mTransferDataMapMutex);

                int r = libusb_submit_transfer(transferData->tranfer.get());
                if (r < 0)
                {
                    mLastLibusbError.saveError(r, "libusb_submit_transfer");
                    success = false;
                    break;
                }

                mTransferDataMap.insert(std::make_pair(transferData->tranfer.get(), std::move(transferData)));
            }
        }

        if (!success)
        {
            std::lock_guard<std::recursive_mutex> lock(mTransferDataMapMutex);
            mTransferDataMap.clear();
        }

        return success;
    }

    //! Starts the read thread
    //! @param[in] rxFn The function to call when a full packet is received
    //! @param[in] completeFn The function to call on disconnect
    //! @return true if interface was open or opened and read thread was started
    bool beginRead(
        const std::function<void(uint64_t, uint8_t, std::vector<std::uint8_t>&)>& rxFn,
        const std::function<void(const std::string&)>& completeFn
    )
    {
        if (!openInterface())
        {
            return false;
        }

        if (!createTransfers())
        {
            return false;
        }

        mExitRequested = false;
        mRxStalled = false;
        mRxFn = rxFn;
        mRxCompleteFn = completeFn;

        std::lock_guard<std::recursive_mutex> lock(mReadThreadMutex);
        mReadThread = std::make_unique<std::thread>(
            [this]()
            {
                while (mInterfaceClaimed && !mExitRequested)
                {
                    if (mRxStalled && mTransferDataMap.empty())
                    {
                        if (!mLibusbDeviceHandle)
                        {
                            mLastLibusbError.saveErrorIfNotSet(LIBUSB_ERROR_IO, "Device handle freed");
                            mExitRequested = true;
                            break;
                        }

                        int r = libusb_clear_halt(mLibusbDeviceHandle.get(), mEpIn);
                        if (r < 0)
                        {
                            mLastLibusbError.saveError(r, "libusb_clear_halt");
                            mExitRequested = true;
                            break;
                        }

                        if (!createTransfers())
                        {
                            mExitRequested = true;
                            break;
                        }
                    }

                    int r = libusb_handle_events(mLibusbContext.get());
                    if (r < 0)
                    {
                        mLastLibusbError.saveError(r, "libusb_handle_events");
                        mExitRequested = true;
                        break;
                    }
                }

                if (mRxCompleteFn)
                {
                    mRxCompleteFn(getLastErrorStr());
                }
            }
        );

        return true;
    }

    //! Request stop of the read thread
    void stopRead()
    {
        std::lock_guard<std::recursive_mutex> lock(mTransferDataMapMutex);

        // Flag the thread to exit
        mExitRequested = true;

        // Cancel any transfers in progress in order to wake read thread
        cancelTransfers();
    }

    void cancelTransfers()
    {
        std::lock_guard<std::recursive_mutex> lock(mTransferDataMapMutex);

        for (auto& pair : mTransferDataMap)
        {
            libusb_cancel_transfer(pair.second->tranfer.get());
        }
    }

    //! @return true iff the current thread is the read thread
    bool isThisThreadReading()
    {
        return (mReadThread && mReadThread->get_id() == std::this_thread::get_id());
    }

    // Consumes the read thread so it may be externally joined
    std::unique_ptr<std::thread> consumeReadThread()
    {
        std::unique_ptr<std::thread> readThread;
        std::lock_guard<std::recursive_mutex> lock(mReadThreadMutex);
        if (!isThisThreadReading())
        {
            readThread = std::move(mReadThread);
        }
        return readThread;
    }

    //! Wait for the read thread to complete
    void joinRead()
    {
        std::unique_ptr<std::thread> readThread = consumeReadThread();
        if (readThread)
        {
            readThread->join();
        }
    }

    //! Closes the interface
    //! @return true if interface was closed or was already closed
    bool closeInterface()
    {
        stopRead();
        bool result = true;

        if (mInterfaceClaimed)
        {
            mInterfaceClaimed = false;

            if (mLibusbDeviceHandle)
            {
                // Set up control transfer for disconnect message (clears buffers)
                libusb_control_transfer(
                    mLibusbDeviceHandle.get(),
                    LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_OUT,
                    0x22, // bRequest
                    0x00, // wValue (disconnection)
                    mInterfaceNumber, // wIndex
                    nullptr, // data buffer
                    0,    // wLength
                    1000  // timeout in milliseconds
                );

                int r = libusb_release_interface(mLibusbDeviceHandle.get(), mInterfaceNumber);
                if (r < 0)
                {
                    mLastLibusbError.saveError(r, "libusb_release_interface");
                    result = false;
                }

            }


        }

        return result;
    }

    //! @return description of the last experienced error
    std::string getLastErrorStr() const
    {
        return mLastLibusbError.getErrorDesc();
    }

    //! @return true iff the interface is currently claimed
    bool isConnected()
    {
        return mInterfaceClaimed;
    }

    //! @return USB version number {major, minor, patch}
    std::array<std::uint8_t, 3> getVersion() const
    {
        std::array<std::uint8_t, 3> version;
        std::uint16_t bcdVer = mDesc->bcdDevice;
        version[0] = (bcdVer >> 8) & 0xFF;
        version[1] = (bcdVer >> 4) & 0x0F;
        version[2] = (bcdVer) & 0x0F;
        return version;
    }

    //! Set an error which occurs externally
    //! @param[in] where Explanation of where the error occurred
    void setExternalError(const char* where)
    {
        mLastLibusbError.saveError(LIBUSB_SUCCESS, where);
    }

    //! Retrieve the currently connected interface number (first VENDOR interface)
    //! @return the connected interface number
    int getInterfaceNumber()
    {
        return mInterfaceNumber;
    }

    //! @return the currently used IN endpoint
    std::uint8_t getEpIn()
    {
        return mEpIn;
    }

    //! @return the currently used OUT endpoint
    std::uint8_t getEpOut()
    {
        return mEpOut;
    }

public:
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
    //! The read thread created on beginRead()
    std::unique_ptr<std::thread> mReadThread;
    //! Serializes access to mReadThread
    std::recursive_mutex mReadThreadMutex;
    //! Holds the received data
    std::vector<std::uint8_t> mReceiveBuffer;
    //! The function to call whenever a packet is received
    std::function<void(uint64_t, uint8_t, std::vector<std::uint8_t>&)> mRxFn;
    //! The function to call when the read thread exits
    std::function<void(const std::string&)> mRxCompleteFn;
    //! Contains last libusb error data
    LibusbError mLastLibusbError;
    //! Set to true on first connection in order to force reset on subsequent connection
    bool mPreviouslyConnected = false;

}; // class DppDeviceImp

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
        DppDeviceImp::uint32ToBytes(buffer, word);
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
        packet.push_back(DppDeviceImp::bytesToUint32(&payload[i]));
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
                arr[aidx++] = DppDeviceImp::bytesToUint32(&payload[pidx]);
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

// (essentially, 4 byte max for address length at 7 bits of data per byte)
std::uint64_t DppDevice::mMaxAddr = 0x0FFFFFFF;

DppDevice::DppDevice(std::unique_ptr<DppDeviceImp>&& dev) : mImp(std::move(dev))
{}

DppDevice::~DppDevice()
{
    disconnect();
}

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

    return std::make_unique<DppDeviceFactory>(std::make_unique<DppDeviceImp>(
        foundDevice.serial,
        std::move(foundDevice.dev),
        std::move(libusbContext),
        std::move(foundDevice.devHandle)
    ));
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
    mMaxAddr = (std::max)(maxAddr, static_cast<std::uint64_t>(0x0FFFFFFF));
}

const std::string& DppDevice::getSerial() const
{
    return mImp->mSerial;
}

std::array<std::uint8_t, 3> DppDevice::getVersion() const
{
    return mImp->getVersion();
}

std::string DppDevice::getLastErrorStr()
{
    return mImp->getLastErrorStr();
}

bool DppDevice::connect(const std::function<void(const std::string& errStr)>& fn)
{
    // To satisfy an edge case, ensure complete disconnection and join before trying to reconnect
    disconnect();

    std::lock_guard<std::recursive_mutex> lock(mMutex);

    // This function may not be called from any callback context (would cause deadlock)
    if (mImp->isThisThreadReading() || (mTimeoutThread && mTimeoutThread->get_id() == std::this_thread::get_id()))
    {
        mImp->setExternalError("connect attempted within callback context");
        return false;
    }

    if (!mImp->openInterface())
    {
        return false;
    }

    if (
        !mImp->beginRead(
            [this](std::uint64_t addr, std::uint8_t cmd, std::vector<std::uint8_t>& payload)
            {
                handleReceive(addr, cmd, payload);
            },
            [this, fn](const std::string& errStr)
            {
                disconnect();
                if (fn)
                {
                    fn(errStr);
                }
            }
        )
    )
    {
        return false;
    }

    mTimeoutThread = std::make_unique<std::thread>(
        [this]()
        {
            while (true)
            {
                std::list<std::function<void(std::int16_t cmd, std::vector<std::uint8_t>&)>> fns;

                {
                    std::unique_lock<std::mutex> lock(mTimeoutMutex);

                    if (mTimeoutLookup.empty())
                    {
                        mTimeoutCv.wait(lock, [this](){return !isConnected();});
                    }
                    else
                    {
                        std::chrono::system_clock::time_point nextTimePoint = mTimeoutLookup.begin()->first;
                        mTimeoutCv.wait_until(lock, nextTimePoint, [this](){return !isConnected();});
                    }

                    if (!isConnected())
                    {
                        return;
                    }

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
                            fns.push_back(std::move(fnLookupIter->second.callback));
                            mFnLookup.erase(fnLookupIter);
                        }

                        iter = mTimeoutLookup.erase(iter);
                    }
                }

                for (const std::function<void(std::int16_t cmd, const std::vector<std::uint8_t>)>& fn : fns)
                {
                    fn(::dpp_api::msg::rx::Msg::kCmdTimeout, {});
                }
            }


            std::list<std::function<void(std::int16_t cmd, std::vector<std::uint8_t>&)>> fns;

            {
                std::unique_lock<std::mutex> lock(mTimeoutMutex);

                for (FunctionLookupMap::reference entry : mFnLookup)
                {
                    fns.push_back(std::move(entry.second.callback));
                }

                mFnLookup.clear();
                mTimeoutLookup.clear();
            }
        }
    );

    mConnected = true;

    return true;
}

bool DppDevice::disconnect()
{
    bool closed = false;
    std::unique_ptr<std::thread> readThread;
    std::unique_ptr<std::thread> timeoutThread;

    {
        std::lock_guard<std::recursive_mutex> lock(mMutex);

        mConnected = false;

        if (mTimeoutThread)
        {
            std::lock_guard<std::mutex> lock(mTimeoutMutex);
            mTimeoutCv.notify_all();
            if (mTimeoutThread->get_id() != std::this_thread::get_id())
            {
                timeoutThread = std::move(mTimeoutThread);
            }
        }

        readThread = mImp->consumeReadThread();

        // Calling this may cause a call to disconnect() from the read thread
        closed = mImp->closeInterface();
    }

    // Join threads outside of mutex scope

    if (readThread)
    {
        readThread->join();
    }

    if (timeoutThread)
    {
        timeoutThread->join();
    }

    return closed;
}

void DppDevice::handleReceive(std::uint64_t addr, std::uint8_t cmd, std::vector<std::uint8_t>& payload)
{
    std::function<void(std::uint8_t cmd, std::vector<std::uint8_t>& payload)> respFn;

    {
        std::lock_guard<std::recursive_mutex> lock(mMutex);
        FunctionLookupMap::iterator iter = mFnLookup.find(addr);
        if (iter != mFnLookup.end())
        {
            respFn = std::move(iter->second.callback);
            mTimeoutLookup.erase(iter->second.timeoutMapIter);
            mFnLookup.erase(iter);
        }
    }

    if (respFn)
    {
        respFn(cmd, payload);
    }
}

std::uint64_t DppDevice::send(
    std::uint8_t cmd,
    const std::vector<std::uint8_t>& payload,
    const std::function<void(std::int16_t cmd, std::vector<std::uint8_t>& payload)>& respFn,
    std::uint32_t timeoutMs
)
{
    std::uint64_t addr = 0;

    {
        std::lock_guard<std::recursive_mutex> lock(mMutex);

        if (mNextAddr < kMinAddr)
        {
            mNextAddr = kMinAddr;
        }

        addr = mNextAddr++;

        if (mNextAddr > mMaxAddr)
        {
            mNextAddr = kMinAddr;
        }

        if (respFn)
        {
            std::lock_guard<std::mutex> lock(mTimeoutMutex);

            FunctionLookupMapEntry entry;
            entry.callback = respFn;
            entry.timeoutMapIter = mTimeoutLookup.insert(std::make_pair(
                std::chrono::system_clock::now() + std::chrono::milliseconds(timeoutMs),
                addr
            ));

            mFnLookup[addr] = std::move(entry);

            mTimeoutCv.notify_all();
        }
    }

    return (mImp->send(addr, cmd, payload)) ? addr : 0;
}

bool DppDevice::isConnected()
{
    return mConnected;
}

std::size_t DppDevice::getNumWaiting()
{
    std::lock_guard<std::recursive_mutex> lock(mMutex);
    // Size of both of these maps should be equal
    return (std::max)(mFnLookup.size(), mTimeoutLookup.size());
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
