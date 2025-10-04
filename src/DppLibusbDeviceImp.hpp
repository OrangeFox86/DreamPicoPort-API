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

    //! @return the serial of this device
    const std::string& getSerial() const override;

    //! @return USB version number {major, minor, patch}
    std::array<std::uint8_t, 3> getVersion() const override;

    //! @return string representation of last error
    std::string getLastErrorStr() override;

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
    //! Initialize for subsequent read
    //! @param[in] rxFn The function to call when a full packet is received
    //! @return true if interface was open or opened and transfers ready for read loop
    bool readInit(const std::function<void(const std::uint8_t*, int)>& rxFn) override;

    //! Executes the read loop, blocking until disconnect
    void readLoop() override;

    //! Signal the read loop to stop (non-blocking)
    void stopRead() override;

    //! Close the USB interface
    //! @return true iff interface was closed
    bool closeInterface() override;

    //! Sends data on the vendor interface
    //! @param[in] data Buffer to send
    //! @param[in] length Number of bytes in \p data
    //! @param[in] timeoutMs Send timeout in milliseconds
    //! @return true if data was successfully sent
    bool send(std::uint8_t* data, int length, unsigned int timeoutMs = 1000) override;

private:
    std::unique_ptr<LibusbDevice> mLibusbDevice;


};
}

#endif // __DREAM_PICO_PORT_LIBUSB_DEVICE_IMP_HPP__
