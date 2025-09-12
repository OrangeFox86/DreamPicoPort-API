#include "DreamPicoPortApi.hpp"

#include "libusb.h"

#include <cstdint>
#include <cstdlib>
#include <vector>
#include <thread>
#include <mutex>
#include <functional>

namespace dpp_api
{

struct LibusbContextDeleter
{
    void operator()(libusb_context* p) const
    {
        libusb_exit(p);
    }
};

struct LibusbDeviceHandleDeleter
{
    void operator()(libusb_device_handle* handle) const
    {
        libusb_close(handle);
    }
};

struct LibusbDeviceListDeleter
{
    void operator()(libusb_device** devs) const
    {
        libusb_free_device_list(devs, 1);
    }
};

struct LibusbConfigDescriptorDeleter
{
    void operator()(libusb_config_descriptor* config) const
    {
        libusb_free_config_descriptor(config);
    }
};

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

void LIBUSB_CALL on_libusb_transfer_complete(libusb_transfer *transfer);

class DppDeviceImp
{
public:
    DppDeviceImp(
        const std::string& serial,
        const libusb_device_descriptor& desc,
        std::unique_ptr<libusb_context, LibusbContextDeleter>&& libusbContext,
        std::unique_ptr<libusb_device_handle, LibusbDeviceHandleDeleter>&& libusbDeviceHandle
    ) :
        mSerial(serial),
        mDesc(desc),
        mLibusbContext(std::move(libusbContext)),
        mLibusbDeviceHandle(std::move(libusbDeviceHandle)),
        mInterfaceClaimed(false),
        mEpIn(0),
        mEpOut(0),
        mReadThread()
    {
    }

    ~DppDeviceImp()
    {
        closeInterface();
        if (mReadThread)
        {
            mReadThread->join();
        }
    }

    bool openInterface()
    {
        std::lock_guard<std::mutex> lock(mMutex);

        if (mInterfaceClaimed)
        {
            return true;
        }

        // Dynamically retrieve endpoint addresses for the interface
        std::unique_ptr<libusb_config_descriptor, LibusbConfigDescriptorDeleter> configDescriptor;

        {
            libusb_config_descriptor *config;
            int r = libusb_get_active_config_descriptor(libusb_get_device(mLibusbDeviceHandle.get()), &config);
            if (r < 0)
            {
                return false;
            }
            configDescriptor.reset(config);
        }

        const libusb_interface *selectedInterface = nullptr;
        for (std::uint8_t i = 0; i < configDescriptor->bNumInterfaces; ++i)
        {
            const libusb_interface *itf = &configDescriptor->interface[i];
            if (itf->altsetting->bInterfaceNumber == kInterfaceNumber)
            {
                selectedInterface = itf;
                break;
            }
        }

        if (!selectedInterface || selectedInterface->num_altsetting <= 0)
        {
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
            return false;
        }

        mEpOut = static_cast<std::uint8_t>(outEndpoint);
        mEpIn = static_cast<std::uint8_t>(inEndpoint);
        configDescriptor.reset();

        int r = libusb_claim_interface(mLibusbDeviceHandle.get(), kInterfaceNumber);
        if (r < 0)
        {
            // Handle error - interface claim failed
            return false;
        }

        // Set up control transfer for connect message (clears buffers)
        r = libusb_control_transfer(
            mLibusbDeviceHandle.get(),
            LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_OUT,
            0x22, // bRequest
            0x01, // wValue (connection)
            kInterfaceNumber, // wIndex
            nullptr, // data buffer
            0,    // wLength
            1000  // timeout in milliseconds
        );

        if (r < 0)
        {
            // Handle control transfer error
            libusb_release_interface(mLibusbDeviceHandle.get(), kInterfaceNumber);
            return false;
        }

        mInterfaceClaimed = true;
        return true;
    }

    bool send(std::uint64_t addr, std::uint8_t cmd, const std::vector<std::uint8_t>& payload)
    {
        if (!openInterface())
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

        return r >= 0 && transferred == static_cast<int>(data.size());
    }

    static std::uint16_t bytesToUint16(const void* payload)
    {
        const std::uint8_t* p8 = reinterpret_cast<const std::uint8_t*>(payload);
        return (static_cast<std::uint16_t>(p8[0]) << 8 | p8[1]);
    }

    static void uint16ToBytes(void* out, std::uint16_t data)
    {
        std::uint8_t* p8 = reinterpret_cast<std::uint8_t*>(out);
        *p8++ = data >> 8;
        *p8 = data & 0xFF;
    }

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

    static std::uint16_t computeCrc16(const void* buffer, std::uint16_t bufLen)
    {
        return computeCrc16(0xFFFFU, buffer, bufLen);
    }

    void processPackets()
    {
        while (mCombinedReceiveBuffer.size() >= kMinPacketSize)
        {
            std::size_t magicStart = 0;
            std::size_t magicSize = 0;
            std::size_t idx = 0;
            std::size_t magicIdx = 0;
            while (idx < mCombinedReceiveBuffer.size() && magicSize < kSizeMagic)
            {
                if (kMagicSequence[magicIdx] == mCombinedReceiveBuffer[idx])
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
                mCombinedReceiveBuffer.erase(mCombinedReceiveBuffer.begin(), mCombinedReceiveBuffer.begin() + magicStart);
                if (mCombinedReceiveBuffer.size() < kMinPacketSize)
                {
                    // Not large enough for a full packet
                    return;
                }
            }

            std::uint16_t size = bytesToUint16(&mCombinedReceiveBuffer[kSizeMagic]);
            std::uint16_t sizeInv = bytesToUint16(&mCombinedReceiveBuffer[kSizeMagic + 2]);
            if ((size ^ sizeInv) != 0xFFFF || size < (kMinSizeAddress + kSizeCrc))
            {
                // Invalid size inverse, discard first byte and retry
                mCombinedReceiveBuffer.erase(mCombinedReceiveBuffer.begin(), mCombinedReceiveBuffer.begin() + 1);
                continue;
            }

            // Check if full payload is available
            if (mCombinedReceiveBuffer.size() < (kSizeMagic + kSizeSize + size))
            {
                // Wait for more data
                return;
            }

            // Check CRC
            std::size_t pktSize = kSizeMagic + kSizeSize + size;
            const std::uint16_t receivedCrc = bytesToUint16(&mCombinedReceiveBuffer[pktSize - kSizeCrc]);
            const std::uint16_t computedCrc =
                computeCrc16(&mCombinedReceiveBuffer[kSizeMagic + kSizeSize], size - kSizeCrc);

            if (receivedCrc != computedCrc)
            {
                // Invalid CRC, discard first byte and retry
                mCombinedReceiveBuffer.erase(mCombinedReceiveBuffer.begin(), mCombinedReceiveBuffer.begin() + 1);
                continue;
            }

            // Extract address (variable-length, 7 bits per byte, MSb=1 if more bytes follow)
            std::uint64_t addr = 0;
            std::int8_t addrLen = 0;
            bool lastByteBreak = false;
            std::size_t maxAddrSize = mCombinedReceiveBuffer.size() - kSizeMagic - kSizeSize - kSizeCrc;
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
              const std::uint8_t thisByte = mCombinedReceiveBuffer[kSizeMagic + kSizeSize + i];
              addr |= (thisByte & mask) << (7 * i);
              ++addrLen;
              if ((thisByte & 0x80) == 0)
              {
                lastByteBreak = true;
                break;
              }
            }
            if (mCombinedReceiveBuffer.size() <= (kSizeMagic + kSizeSize + addrLen + kSizeCrc))
            {
                // Missing command byte, discard first byte and retry
                mCombinedReceiveBuffer.erase(mCombinedReceiveBuffer.begin(), mCombinedReceiveBuffer.begin() + 1);
                continue;
            }

            // Extract command
            const std::uint8_t cmd = mCombinedReceiveBuffer[kSizeMagic + kSizeSize + addrLen];

            // Extract payload
            const std::size_t beginIdx = kSizeMagic + kSizeSize + addrLen + kSizeCommand;
            const std::size_t endIdx = kSizeMagic + kSizeSize + size - kSizeCrc;
            std::vector<std::uint8_t> payload(
                mCombinedReceiveBuffer.begin() + beginIdx,
                mCombinedReceiveBuffer.begin() + endIdx
            );

            // Erase this packet from data
            mCombinedReceiveBuffer.erase(
                mCombinedReceiveBuffer.begin(),
                mCombinedReceiveBuffer.begin() + kSizeMagic + kSizeSize + size
            );

            // Process the data
            if (mRxFn)
            {
                mRxFn(addr, cmd, payload);
            }
        }
    }

    void transferComplete(libusb_transfer *transfer)
    {
        if (mRxFn)
        {
            if (transfer->actual_length < mReceiveBuffer.size())
            {
                mReceiveBuffer.resize(transfer->actual_length);
            }

            mCombinedReceiveBuffer.insert(mCombinedReceiveBuffer.end(), mReceiveBuffer.begin(), mReceiveBuffer.end());
            processPackets();

            mReceiveBuffer.resize(kRxSize);
            transfer->buffer = &mReceiveBuffer[0];
            transfer->length = mReceiveBuffer.size();
        }

        if (mInterfaceClaimed)
        {
            // Submit new transfer
            int r = libusb_submit_transfer(transfer);
            if (r < 0)
            {
                libusb_free_transfer(transfer);
                closeInterface();
            }
        }
        else
        {
            libusb_free_transfer(transfer);
        }

    }

    bool beginRead(
        const std::function<void(uint64_t, uint8_t, const std::vector<std::uint8_t>&)>& rxFn,
        const std::function<void()>& completeFn
    )
    {
        if (!openInterface())
        {
            return false;
        }

        libusb_transfer *transfer = libusb_alloc_transfer(0); // 0 for default number of ISO packets
        if (!transfer)
        {
            return false;
        }

        mReceiveBuffer.resize(kRxSize);
        libusb_fill_bulk_transfer(transfer, mLibusbDeviceHandle.get(), mEpIn, &mReceiveBuffer[0], mReceiveBuffer.size(), on_libusb_transfer_complete, this, 0);
        int r = libusb_submit_transfer(transfer);
        if (r < 0)
        {
            libusb_free_transfer(transfer);
            return false;
        }

        mRxFn = rxFn;
        mRxCompleteFn = completeFn;

        mReadThread = std::make_unique<std::thread>(
            [this]()
            {
                while (mInterfaceClaimed)
                {
                    libusb_handle_events(mLibusbContext.get()); // Process pending events and call callbacks
                }

                if (mRxCompleteFn)
                {
                    mRxCompleteFn();
                }
            }
        );

        return true;
    }

    bool closeInterface()
    {
        std::lock_guard<std::mutex> lock(mMutex);

        if (mInterfaceClaimed)
        {
            mInterfaceClaimed = false;

            // Set up control transfer for disconnect message (clears buffers)
            libusb_control_transfer(
                mLibusbDeviceHandle.get(),
                LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_OUT,
                0x22, // bRequest
                0x00, // wValue (disconnection)
                kInterfaceNumber, // wIndex
                nullptr, // data buffer
                0,    // wLength
                1000  // timeout in milliseconds
            );

            int r = libusb_release_interface(mLibusbDeviceHandle.get(), kInterfaceNumber);
            if (r < 0)
            {
                return false;
            }

        }

        return true;
    }

public:
    static const int kInterfaceNumber = 7;
    const std::string mSerial;
    static constexpr const std::uint8_t kMagicSequence[] = {0xDB, 0x8B, 0xAF, 0xD5};
    static constexpr const std::int8_t kSizeMagic = sizeof(kMagicSequence);
    static constexpr const std::int8_t kSizeSize = 4;
    static constexpr const std::int8_t kMinSizeAddress = 1;
    static constexpr const std::int8_t kMaxSizeAddress = 9;
    static constexpr const std::int8_t kSizeCommand = 1;
    static constexpr const std::int8_t kSizeCrc = 2;
    static constexpr const std::int8_t kMinPacketSize =
        kSizeMagic + kSizeSize + kMinSizeAddress + kSizeCommand + kSizeCrc;

private:
    static const std::size_t kRxSize = 2048;
    libusb_device_descriptor mDesc;
    std::unique_ptr<libusb_context, LibusbContextDeleter> mLibusbContext;
    std::unique_ptr<libusb_device_handle, LibusbDeviceHandleDeleter> mLibusbDeviceHandle;
    bool mInterfaceClaimed;
    std::uint8_t mEpIn;
    std::uint8_t mEpOut;
    std::unique_ptr<std::thread> mReadThread;
    std::mutex mMutex;
    std::vector<std::uint8_t> mReceiveBuffer;
    std::vector<std::uint8_t> mCombinedReceiveBuffer;
    std::function<void(uint64_t, uint8_t, const std::vector<std::uint8_t>&)> mRxFn;
    std::function<void()> mRxCompleteFn;
};

void LIBUSB_CALL on_libusb_transfer_complete(libusb_transfer *transfer)
{
    DppDeviceImp* dppDeviceImp = static_cast<DppDeviceImp*>(transfer->user_data);
    dppDeviceImp->transferComplete(transfer);
}

DppDevice::DppDevice(std::unique_ptr<DppDeviceImp>&& dev) : mImp(std::move(dev))
{}

DppDevice::~DppDevice()
{}

std::unique_ptr<DppDevice> DppDevice::find(const std::string& serial)
{
    std::unique_ptr<libusb_context, LibusbContextDeleter> libusbContext = make_libusb_context();
    LibusbDeviceList deviceList(libusbContext);

    for (libusb_device* dev : deviceList)
    {
        libusb_device_descriptor desc;
        int r = libusb_get_device_descriptor(dev, &desc);
        if (r < 0)
        {
            continue;
        }

        if (desc.idVendor != 0x1209 || desc.idProduct != 0x2F07)
        {
            continue;
        }

        std::unique_ptr<libusb_device_handle, LibusbDeviceHandleDeleter> deviceHandle = make_libusb_device_handle(dev);
        if (!deviceHandle)
        {
            continue;
        }

        unsigned char serial_string[256];
        if (desc.iSerialNumber > 0)
        {
            r = libusb_get_string_descriptor_ascii(
                deviceHandle.get(),
                desc.iSerialNumber,
                serial_string,
                sizeof(serial_string)
            );

            if (r > 0)
            {
                std::string device_serial(reinterpret_cast<char*>(serial_string));
                if (device_serial == serial)
                {
                    struct DppDeviceFactory : public DppDevice
                    {
                        DppDeviceFactory(std::unique_ptr<class DppDeviceImp>&& dev) : DppDevice(std::move(dev)) {}
                    };

                    return std::make_unique<DppDeviceFactory>(std::make_unique<DppDeviceImp>(
                        serial,
                        desc,
                        std::move(libusbContext),
                        std::move(deviceHandle)
                    ));
                }
            }
        }
    }

    return nullptr;
}

std::unique_ptr<DppDevice> DppDevice::findAtIndex(std::size_t idx)
{
    std::unique_ptr<libusb_context, LibusbContextDeleter> libusbContext = make_libusb_context();
    LibusbDeviceList deviceList(libusbContext);

    if (idx >= deviceList.size())
    {
        return nullptr;
    }

    libusb_device* selectedDev = nullptr;
    libusb_device_descriptor desc;
    std::size_t currentIdx = 0;
    for (libusb_device* dev : deviceList)
    {
        int r = libusb_get_device_descriptor(dev, &desc);
        if (r < 0)
        {
            continue;
        }

        if (desc.idVendor != 0x1209 || desc.idProduct != 0x2F07)
        {
            continue;
        }

        if (idx == currentIdx++)
        {
            selectedDev = dev;
            break;
        }
    }

    if (!selectedDev)
    {
        return nullptr;
    }

    auto deviceHandle = make_libusb_device_handle(selectedDev);
    if (!deviceHandle)
    {
        return nullptr;
    }

    unsigned char serial_string[256];
    int r = libusb_get_string_descriptor_ascii(
        deviceHandle.get(),
        desc.iSerialNumber,
        serial_string,
        sizeof(serial_string)
    );
    if (r < 0)
    {
        return nullptr;
    }

    struct DppDeviceFactory : public DppDevice
    {
        DppDeviceFactory(std::unique_ptr<class DppDeviceImp>&& dev) : DppDevice(std::move(dev)) {}
    };

    return std::make_unique<DppDeviceFactory>(std::make_unique<DppDeviceImp>(
        std::string(reinterpret_cast<char*>(serial_string)),
        desc,
        std::move(libusbContext),
        std::move(deviceHandle)
    ));
}

std::size_t DppDevice::getCount()
{
    std::unique_ptr<libusb_context, LibusbContextDeleter> libusbContext = make_libusb_context();
    LibusbDeviceList deviceList(libusbContext);

    libusb_device* selectedDev = nullptr;
    libusb_device_descriptor desc;
    std::size_t count = 0;
    for (libusb_device* dev : deviceList)
    {
        int r = libusb_get_device_descriptor(dev, &desc);
        if (r < 0)
        {
            continue;
        }

        if (desc.idVendor != 0x1209 || desc.idProduct != 0x2F07)
        {
            continue;
        }

        ++count;
    }

    return count;
}

std::string DppDevice::getSerialAt(std::size_t idx)
{
    std::unique_ptr<DppDevice> dev = findAtIndex(idx);
    if (dev)
    {
        return dev->getSerial();
    }
    return {};
}

const std::string& DppDevice::getSerial() const
{
    return mImp->mSerial;
}

bool DppDevice::connect()
{
    if (!mImp->openInterface())
    {
        return false;
    }

    if (
        !mImp->beginRead(
            [this](std::uint64_t addr, std::uint8_t cmd, const std::vector<std::uint8_t>& payload)
            {
                handleReceive(addr, cmd, payload);
            },
            [this](){handleReceiveComplete();}
        )
    )
    {
        return false;
    }

    return true;
}

bool DppDevice::disconnect()
{
    return mImp->closeInterface();
}

void DppDevice::handleReceive(std::uint64_t addr, std::uint8_t cmd, const std::vector<std::uint8_t>& payload)
{
    std::function<void(std::uint8_t cmd, const std::vector<std::uint8_t>& payload)> respFn;

    {
        std::lock_guard<std::mutex> lock(mFnLookupMutex);
        FunctionLookupMap::iterator iter = mFnLookup.find(addr);
        if (iter != mFnLookup.end())
        {
            respFn = std::move(iter->second);
            mFnLookup.erase(iter);
        }
    }

    if (respFn)
    {
        respFn(cmd, payload);
    }
}

void DppDevice::handleReceiveComplete()
{}

bool DppDevice::send(
    std::uint8_t cmd,
    const std::vector<std::uint8_t>& payload,
    const std::function<void(std::uint8_t cmd, const std::vector<std::uint8_t>& payload)>& respFn
)
{
    std::uint64_t addr = 0;

    {
        std::lock_guard<std::mutex> lock(mFnLookupMutex);
        addr = mNextAddr++;
        mFnLookup[addr] = respFn;
    }

    return mImp->send(addr, cmd, payload);
}

}
