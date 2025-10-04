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

#ifndef DREAMPICOPORT_NO_LIBUSB

#include "DppLibusbDeviceImp.hpp"

namespace dpp_api
{
DppLibUsbDeviceImp::DppLibUsbDeviceImp(std::unique_ptr<LibusbDevice>&& dev) :
    mLibusbDevice(std::move(dev))
{}

DppLibUsbDeviceImp::~DppLibUsbDeviceImp()
{
    disconnect();
}

const std::string& DppLibUsbDeviceImp::getSerial() const
{
    return mLibusbDevice->mSerial;
}

std::array<std::uint8_t, 3> DppLibUsbDeviceImp::getVersion() const
{
    return mLibusbDevice->getVersion();
}

std::string DppLibUsbDeviceImp::getLastErrorStr()
{
    return mLibusbDevice->getLastErrorStr();
}

void DppLibUsbDeviceImp::setExternalError(const char* where)
{
    mLibusbDevice->setExternalError(where);
}

int DppLibUsbDeviceImp::getInterfaceNumber()
{
    return mLibusbDevice->getInterfaceNumber();
}

std::uint8_t DppLibUsbDeviceImp::getEpIn()
{
    return mLibusbDevice->getEpIn();
}

std::uint8_t DppLibUsbDeviceImp::getEpOut()
{
    return mLibusbDevice->getEpOut();
}

bool DppLibUsbDeviceImp::readInit(const std::function<void(const std::uint8_t*, int)>& rxFn)
{
    return mLibusbDevice->runInit(rxFn);
}

void DppLibUsbDeviceImp::readLoop()
{
    mLibusbDevice->run();
}

void DppLibUsbDeviceImp::stopRead()
{
    mLibusbDevice->stopRead();
}

bool DppLibUsbDeviceImp::closeInterface()
{
    return mLibusbDevice->closeInterface();
}

bool DppLibUsbDeviceImp::send(std::uint8_t* data, int length, unsigned int timeoutMs)
{
    return mLibusbDevice->send(data, length, timeoutMs);
}

}

#endif // #ifndef DREAMPICOPORT_NO_LIBUSB
