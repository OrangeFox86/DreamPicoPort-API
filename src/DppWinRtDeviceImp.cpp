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

#if defined(_WIN32) //&& defined(DREAMPICOPORT_NO_LIBUSB)

#include "DppWinRtDeviceImp.hpp"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Usb.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Storage.Streams.h>

using namespace winrt;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::Devices::Usb;
using namespace winrt::Windows::Storage;

namespace dpp_api {

DppWinRtDeviceImp::DppWinRtDeviceImp(const std::string& containerId)
{
    std::wstring wContainerId(containerId.begin(), containerId.end());
    std::wstring deviceSelector = L"System.Devices.ContainerId:=\"";
    deviceSelector += wContainerId;
    deviceSelector +=
        L"\" AND System.Devices.InterfaceEnabled:=System.StructuredQueryType.Boolean#True";
        L" AND System.Devices.DeviceInstanceId:~<\"USB\\\"";
    IVector<winrt::hstring> additionalProperties = winrt::single_threaded_vector<winrt::hstring>();
    additionalProperties.Append(L"System.Devices.DeviceInstanceId");
    DeviceInformationCollection deviceInfos =
        DeviceInformation::FindAllAsync(
            deviceSelector,
            additionalProperties,
            DeviceInformationKind::DeviceInterface
        ).get();

    for (const auto& devInfo : deviceInfos)
    {
        auto props = devInfo.Properties();
        winrt::hstring instId;
        devInfo.Properties().Lookup(L"System.Devices.DeviceInstanceId").as(instId);

        std::wstring instIdWStr(instId);
        std::size_t itfPos = instIdWStr.find(L"&MI_");
        if (itfPos == std::wstring::npos)
        {
            // Extract serial number from device instance ID
            size_t backslashPos = instIdWStr.find_last_of(L'\\');
            if (backslashPos != std::wstring::npos && backslashPos + 1 < instIdWStr.length())
            {
                std::wstring serialWide = instIdWStr.substr(backslashPos + 1);
                mSerial = std::string(serialWide.begin(), serialWide.end());
                break; // Found the serial, exit the loop
            }
        }
        else
        {
            // Extract 2-digit hex interface number after "&MI_"
            // TODO: there is likely a better way of doing this
            if (itfPos + 6 <= instIdWStr.size())
            {
                auto hexVal = [](wchar_t c) -> int {
                    if (c >= L'0' && c <= L'9') return c - L'0';
                    if (c >= L'a' && c <= L'f') return 10 + (c - L'a');
                    if (c >= L'A' && c <= L'F') return 10 + (c - L'A');
                    return -1;
                };

                int hi = hexVal(instIdWStr[itfPos + 4]);
                int lo = hexVal(instIdWStr[itfPos + 5]);
                if (hi >= 0 && lo >= 0)
                {
                    // Save the lowest known interface number and its path
                    // TODO: need to filter specifically for vendor interface
                    int itf = (hi << 4) | lo;
                    // if (mInterfaceNumber < 0 || itf < mInterfaceNumber)
                    if (itf == 7)
                    {
                        mInterfaceNumber = itf;
                        mDeviceInterfacePath = devInfo.Id();
                    }
                }
            }
        }
    }
}

DppWinRtDeviceImp::~DppWinRtDeviceImp()
{
    // Ensure disconnection
    disconnect();
}

bool DppWinRtDeviceImp::openInterface()
{
    if (mDeviceInterfacePath.empty())
    {
        return false;
    }

    mDevice = UsbDevice::FromIdAsync(mDeviceInterfacePath).get();

    if (!mDevice)
    {
        return false;
    }

    // Get the first configuration
    auto configuration = mDevice.Configuration();
    if (!configuration)
    {
        return false;
    }

    // Find the interface with the matching interface number
    UsbInterface targetInterface = nullptr;
    for (auto interface : configuration.UsbInterfaces())
    {
        if (interface.InterfaceNumber() == mInterfaceNumber)
        {
            targetInterface = interface;
            break;
        }
    }

    if (!targetInterface)
    {
        return false;
    }

    // Find the bulk endpoints
    mEpIn = 0;
    for (auto pipe : targetInterface.BulkInPipes())
    {
        if (mEpIn == 0)
        {
            mEpIn = pipe.EndpointDescriptor().EndpointNumber();
            mEpInPipe = pipe;
            break;
        }
    }

    mEpOut = 0;
    for (auto pipe : targetInterface.BulkOutPipes())
    {
        if (mEpOut == 0)
        {
            mEpOut = pipe.EndpointDescriptor().EndpointNumber();
            mEpOutPipe = pipe;
            break;
        }
    }

    // Ensure we found both endpoints
    if (mEpIn == 0 || mEpOut == 0)
    {
        return false;
    }

    return true;
}

const std::string& DppWinRtDeviceImp::getSerial() const
{
    return mSerial;
}

std::array<std::uint8_t, 3> DppWinRtDeviceImp::getVersion() const
{
    return mVersion;
}

std::string DppWinRtDeviceImp::getLastErrorStr() const
{
    std::lock_guard<std::mutex> lock(mLastErrorMutex);
    return mLastError;
}

void DppWinRtDeviceImp::setExternalError(const char* where)
{
    std::lock_guard<std::mutex> lock(mLastErrorMutex);
    mLastError = (where ? where : "");
}

int DppWinRtDeviceImp::getInterfaceNumber() const
{
    return mInterfaceNumber;
}

std::uint8_t DppWinRtDeviceImp::getEpIn() const
{
    return mEpIn;
}

std::uint8_t DppWinRtDeviceImp::getEpOut() const
{
    return mEpOut;
}

std::unique_ptr<DppWinRtDeviceImp> DppWinRtDeviceImp::find(const DppDevice::Filter& filter)
{
    hstring deviceSelector = makeSelector(filter);
    // Find all matching devices
    auto additionalProperties = winrt::single_threaded_vector<winrt::hstring>();
    additionalProperties.Append(L"System.Devices.ContainerId");
    auto deviceInfos = DeviceInformation::FindAllAsync(
        deviceSelector,
        additionalProperties,
        DeviceInformationKind::DeviceInterface
    ).get();

    std::uint32_t count = 0;
    for (const auto& devInfo : deviceInfos)
    {
        UsbDevice dev = UsbDevice::FromIdAsync(devInfo.Id()).get();
        UsbDeviceDescriptor desc = dev.DeviceDescriptor();
        guid containerGuid;
        devInfo.Properties().Lookup(L"System.Devices.ContainerId").as(containerGuid);
        hstring containerId;
        containerId = winrt::to_hstring(containerGuid);
        bool match = false;
        if (desc.BcdDeviceRevision() >= filter.minBcdDevice && desc.BcdDeviceRevision() <= filter.maxBcdDevice)
        {
            if (filter.serial.empty())
            {
                // No serial to match
                match = true;
            }
            else
            {
                std::wstring rootSelector =
                    L"System.Devices.InterfaceEnabled:=System.StructuredQueryType.Boolean#True"
                    L" AND System.Devices.DeviceInstanceId:~<\"USB\\" +
                    makeVidPidStr(filter.idVendor, filter.idProduct) +
                    L"\\\"";

                auto rootDevInfos = DeviceInformation::FindAllAsync(rootSelector).get();

                if (rootDevInfos.Size() > 0)
                {
                    winrt::hstring instId = rootDevInfos.GetAt(0).Id();
                    std::wstring instIdWStr(instId);
                    // Extract serial number from device instance ID
                    size_t backslashPos = instIdWStr.find_last_of(L'\\');
                    if (backslashPos != std::wstring::npos && backslashPos + 1 < instIdWStr.length())
                    {
                        std::wstring serialWide = instIdWStr.substr(backslashPos + 1);
                        std::string serial = std::string(serialWide.begin(), serialWide.end());
                        if (serial == filter.serial)
                        {
                            match = true;
                        }
                    }
                }
            }

            if (match)
            {
                if (filter.idx == count)
                {
                    std::wstring containerIdWStr(containerId);
                    std::string containerIdStr(containerIdWStr.begin(), containerIdWStr.end());
                    return std::make_unique<DppWinRtDeviceImp>(containerIdStr);
                }
                ++count;
            }
        }
    }

    return nullptr;
}

std::uint32_t DppWinRtDeviceImp::getCount(const DppDevice::Filter& filter)
{
    hstring deviceSelector = makeSelector(filter);
    // Find all matching devices
    auto additionalProperties = winrt::single_threaded_vector<winrt::hstring>();
    additionalProperties.Append(L"System.Devices.ContainerId");
    auto deviceInfos = DeviceInformation::FindAllAsync(
        deviceSelector,
        additionalProperties,
        DeviceInformationKind::DeviceInterface
    ).get();

    std::uint32_t count = 0;
    for (const auto& devInfo : deviceInfos)
    {
        UsbDevice dev = UsbDevice::FromIdAsync(devInfo.Id()).get();
        UsbDeviceDescriptor desc = dev.DeviceDescriptor();
        if (desc.BcdDeviceRevision() >= filter.minBcdDevice && desc.BcdDeviceRevision() <= filter.maxBcdDevice)
        {
            if (filter.serial.empty())
            {
                // No serial to match
                ++count;
            }
            else
            {
                guid containerGuid;
                devInfo.Properties().Lookup(L"System.Devices.ContainerId").as(containerGuid);
                hstring containerId;
                containerId = winrt::to_hstring(containerGuid);

                std::wstring rootSelector =
                    L"System.Devices.InterfaceEnabled:=System.StructuredQueryType.Boolean#True"
                    L" AND System.Devices.DeviceInstanceId:~<\"USB\\" +
                    makeVidPidStr(filter.idVendor, filter.idProduct) +
                    L"\\\"";

                auto rootDevInfos = DeviceInformation::FindAllAsync(rootSelector).get();

                if (rootDevInfos.Size() > 0)
                {
                    winrt::hstring instId = rootDevInfos.GetAt(0).Id();
                    std::wstring instIdWStr(instId);
                    // Extract serial number from device instance ID
                    size_t backslashPos = instIdWStr.find_last_of(L'\\');
                    if (backslashPos != std::wstring::npos && backslashPos + 1 < instIdWStr.length())
                    {
                        std::wstring serialWide = instIdWStr.substr(backslashPos + 1);
                        std::string serial = std::string(serialWide.begin(), serialWide.end());
                        if (serial == filter.serial)
                        {
                            ++count;
                        }
                    }
                }
            }
        }
    }
    return count;
}

bool DppWinRtDeviceImp::readInit()
{
    if (!openInterface())
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(mReadMutex);
    mStopRequested = false;
    mReading = true;

    nextTransferIn();

    return true;
}

void DppWinRtDeviceImp::readLoop()
{
    std::unique_lock<std::mutex> lock(mReadMutex);

    // Nothing actually done here - just for compatibility
    // TODO: make readLoop optional
    mReadCv.wait(lock, [this](){return !mReading || mStopRequested;});

    if (mReadOperation)
    {
        try
        {
            mReadOperation.get();
            mReadOperation = nullptr;
        }
        catch (...)
        {}
    }

    mReading = false;
}

void DppWinRtDeviceImp::stopRead()
{
    std::lock_guard<std::mutex> lock(mReadMutex);
    mStopRequested = true;
    if (mReadOperation)
    {
        mReadOperation.Cancel();
    }
    mReadCv.notify_all();
}

bool DppWinRtDeviceImp::closeInterface()
{
    stopRead();
    mEpInPipe = nullptr;
    mEpOutPipe = nullptr;
    mDevice = nullptr;
    return true;
}

bool DppWinRtDeviceImp::send(std::uint8_t* data, int length, unsigned int timeoutMs)
{
    winrt::Windows::Storage::Streams::Buffer writeBuf(length);
    writeBuf.Length(length);
    memcpy(writeBuf.data(), data, length);
    auto writeOp = mEpOutPipe.OutputStream().WriteAsync(writeBuf);
    auto status = writeOp.wait_for(std::chrono::milliseconds(timeoutMs));
    if (status != Windows::Foundation::AsyncStatus::Completed)
    {
        writeOp.Cancel();
        writeOp.get();
        return false;
    }
    return true;
}

void DppWinRtDeviceImp::nextTransferIn()
{
    mReadBuffer.Length(0);
    mReadOperation = mEpInPipe.InputStream().ReadAsync(
        mReadBuffer,
        kRxSize,
        Streams::InputStreamOptions::Partial | Streams::InputStreamOptions::ReadAhead
    );
    mReadOperation.Completed(
        [this]
        (
            const winrt::Windows::Foundation::IAsyncOperationWithProgress<winrt::Windows::Storage::Streams::IBuffer,uint32_t>& sender,
            winrt::Windows::Foundation::AsyncStatus status
        )
        {
            transferInComplete(sender, status);
        }
    );
}

void DppWinRtDeviceImp::transferInComplete(
    const winrt::Windows::Foundation::IAsyncOperationWithProgress<winrt::Windows::Storage::Streams::IBuffer,uint32_t>& sender,
    winrt::Windows::Foundation::AsyncStatus status
)
{
    if (status == winrt::Windows::Foundation::AsyncStatus::Completed)
    {
        auto result = sender.get();
        handleReceive(result.data(), static_cast<int>(result.Length()));

        std::lock_guard<std::mutex> lock(mReadMutex);
        nextTransferIn();
    }
    else
    {
        {
            std::lock_guard<std::mutex> lock(mLastErrorMutex);
            if (mLastError.empty())
            {
                mLastError = "Read ";
                switch (status)
                {
                    case Windows::Foundation::AsyncStatus::Canceled: mLastError += "canceled"; break;
                    case Windows::Foundation::AsyncStatus::Error: mLastError += "error"; break;
                    default: mLastError += "failed"; break;
                }
            }
        }

        stopRead();
    }
}

std::wstring DppWinRtDeviceImp::makeVidPidStr(std::uint16_t vid, std::uint16_t pid)
{
    std::wstring vidPidStr = L"VID_";

    // Convert idVendor to 4-digit hex string
    wchar_t vendorHex[5];
    swprintf_s(vendorHex, L"%04X", vid);
    vidPidStr += vendorHex;

    vidPidStr += L"&PID_";

    // Convert idProduct to 4-digit hex string
    wchar_t productHex[5];
    swprintf_s(productHex, L"%04X", pid);
    vidPidStr += productHex;

    return vidPidStr;
}

winrt::hstring DppWinRtDeviceImp::makeSelector(const DppDevice::Filter& filter)
{
    // The interface GUID is set on the DreamPicoPort
    std::wstring deviceSelector =
        L"System.Devices.InterfaceEnabled:=System.StructuredQueryType.Boolean#True"
        L" AND System.Devices.InterfaceClassGuid:=\"{31C4F7D3-1AF2-4AD0-B461-3A760CBBD4FB}\""
        L" AND System.Devices.DeviceInstanceId:~<\"USB\\" +
        makeVidPidStr(filter.idVendor, filter.idProduct) +
        L"\"";

    return winrt::hstring(deviceSelector);
}

} // namespace dpp_api

#endif
