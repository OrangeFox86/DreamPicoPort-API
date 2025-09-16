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

//! @file this file is a hodge-podge of different tests (not meant to be thorough or give any PASS/FAIL result)

#include "DreamPicoPortApi.hpp"

#include <cstdio>
#include <thread>
#include <mutex>
#include <condition_variable>

std::condition_variable mainCv;
std::mutex mainMutex;
std::uint32_t respCount = 0;

void update_response_count()
{
    std::lock_guard<std::mutex> lock(mainMutex);
    ++respCount;
    mainCv.notify_all();
}

void response(std::int16_t cmd, const std::vector<std::uint8_t>& payload)
{
    if (cmd < 0)
    {
        printf("Response timeout\n");
    }
    else
    {
        printf("Response received; cmd: %02hhx\n", static_cast<std::uint8_t>(cmd));
        for (std::uint8_t b : payload)
        {
            printf("%02hhx ", b);
        }
        printf("\n");
    }

    update_response_count();
}

void response32(const dpp_api::msg::rx::Maple32& msg)
{
    if (msg.cmd < 0)
    {
        printf("Response timeout\n");
    }
    else
    {
        printf("Response received; cmd: %02hhx\n", static_cast<std::uint8_t>(msg.cmd));
        for (std::uint32_t b : msg.packet)
        {
            printf("%04x ", b);
        }
        printf("\n");
    }

    update_response_count();
}

void player_reset_response(std::int16_t cmd, std::uint8_t numReset)
{
    if (cmd < 0)
    {
        printf("Response timeout\n");
    }
    else
    {
        printf("Response received; cmd: %02hhx, numReset: %hhu\n", static_cast<std::uint8_t>(cmd), numReset);
    }

    update_response_count();
}

void simple_response(std::int16_t cmd)
{
    if (cmd < 0)
    {
        printf("Response timeout\n");
    }
    else
    {
        printf("Response received; cmd: %02hhx\n", static_cast<std::uint8_t>(cmd));
    }

    update_response_count();
}

void summary_response(std::int16_t cmd, const std::list<std::list<std::array<uint32_t, 2>>>& summary)
{
    if (cmd < 0)
    {
        printf("Response timeout\n");
    }
    else
    {
        printf("Response received; cmd: %02hhx\n", static_cast<std::uint8_t>(cmd));
        printf("{");
        bool outerFirst = true;
        for (const std::list<std::array<uint32_t, 2>>& periph : summary)
        {
            if (!outerFirst)
            {
                printf(",");
            }

            printf("\n  {");
            bool innerFirst = true;
            for (const std::array<uint32_t, 2>& fns : periph)
            {
                if (!innerFirst)
                {
                    printf(",");
                }
                printf("{%08X, %08X}", fns[0], fns[1]);
                innerFirst = false;
            }
            printf("}");
            outerFirst = false;
        }
        if (!outerFirst)
        {
            printf("\n");
        }
        printf("}\n");
    }

    update_response_count();
}

void ver_response(std::int16_t cmd, std::uint8_t verMajor, std::uint8_t verMinor)
{
    if (cmd < 0)
    {
        printf("Response timeout\n");
    }
    else
    {
        printf("Response received; cmd: %02hhx, ver:%hhu.%hhu\n", static_cast<std::uint8_t>(cmd), verMajor, verMinor);
    }

    update_response_count();
}

void controller_state_response(dpp_api::msg::rx::GetControllerState& msg)
{
    if (msg.cmd < 0)
    {
        printf("Response timeout\n");
    }
    else
    {
        printf("Response received; cmd: %02hhx\n", static_cast<std::uint8_t>(msg.cmd));
        printf("Left analog: %hhi,%hhi\n", msg.controllerState.x, msg.controllerState.y);
        printf("Right analog: %hhi,%hhi\n", msg.controllerState.rx, msg.controllerState.ry);
        printf("Hat: ");
        switch(msg.controllerState.hat)
        {
            case dpp_api::ControllerState::GAMEPAD_HAT_UP:
                printf("UP\n");
                break;
            case dpp_api::ControllerState::GAMEPAD_HAT_UP_RIGHT:
                printf("UP-RIGHT\n");
                break;
            case dpp_api::ControllerState::GAMEPAD_HAT_RIGHT:
                printf("RIGHT\n");
                break;
            case dpp_api::ControllerState::GAMEPAD_HAT_DOWN_RIGHT:
                printf("DOWN-RIGHT\n");
                break;
            case dpp_api::ControllerState::GAMEPAD_HAT_DOWN:
                printf("DOWN\n");
                break;
            case dpp_api::ControllerState::GAMEPAD_HAT_DOWN_LEFT:
                printf("DOWN-LEFT\n");
                break;
            case dpp_api::ControllerState::GAMEPAD_HAT_LEFT:
                printf("LEFT\n");
                break;
            case dpp_api::ControllerState::GAMEPAD_HAT_UP_LEFT:
                printf("UP-LEFT\n");
                break;
            case dpp_api::ControllerState::GAMEPAD_HAT_CENTERED: // fall through
            default:
                printf("CENTERED\n");
                break;
        }
        printf(
            "Buttons: %s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s\n",
            (msg.controllerState.isPressed(dpp_api::ControllerState::GAMEPAD_BUTTON_A) ? "A" : ""),
            (msg.controllerState.isPressed(dpp_api::ControllerState::GAMEPAD_BUTTON_B) ? "B" : ""),
            (msg.controllerState.isPressed(dpp_api::ControllerState::GAMEPAD_BUTTON_C) ? "C" : ""),
            (msg.controllerState.isPressed(dpp_api::ControllerState::GAMEPAD_BUTTON_X) ? "X" : ""),
            (msg.controllerState.isPressed(dpp_api::ControllerState::GAMEPAD_BUTTON_Y) ? "Y" : ""),
            (msg.controllerState.isPressed(dpp_api::ControllerState::GAMEPAD_BUTTON_Z) ? "Z" : ""),
            (msg.controllerState.isPressed(dpp_api::ControllerState::GAMEPAD_BUTTON_DPAD_B_R) ? "R" : ""),
            (msg.controllerState.isPressed(dpp_api::ControllerState::GAMEPAD_BUTTON_DPAD_B_L) ? "L" : ""),
            (msg.controllerState.isPressed(dpp_api::ControllerState::GAMEPAD_BUTTON_DPAD_B_D) ? "D" : ""),
            (msg.controllerState.isPressed(dpp_api::ControllerState::GAMEPAD_BUTTON_DPAD_B_U) ? "U" : ""),
            (msg.controllerState.isPressed(dpp_api::ControllerState::GAMEPAD_BUTTON_D) ? "D" : ""),
            (msg.controllerState.isPressed(dpp_api::ControllerState::GAMEPAD_BUTTON_START) ? "S" : ""),
            (msg.controllerState.isPressed(dpp_api::ControllerState::GAMEPAD_VMU1_A) ? "[A]" : ""),
            (msg.controllerState.isPressed(dpp_api::ControllerState::GAMEPAD_VMU1_B) ? "[B]" : ""),
            (msg.controllerState.isPressed(dpp_api::ControllerState::GAMEPAD_VMU1_U) ? "[U]" : ""),
            (msg.controllerState.isPressed(dpp_api::ControllerState::GAMEPAD_VMU1_D) ? "[D]" : ""),
            (msg.controllerState.isPressed(dpp_api::ControllerState::GAMEPAD_VMU1_L) ? "[L]" : ""),
            (msg.controllerState.isPressed(dpp_api::ControllerState::GAMEPAD_VMU1_R) ? "[R]" : ""),
            (msg.controllerState.isPressed(dpp_api::ControllerState::GAMEPAD_CHANGE_DETECT) ? "*" : "")
        );
        printf("Pad: %hhu\n", msg.controllerState.pad);
    }

    update_response_count();
}

void controller_connection_response(dpp_api::msg::rx::GetConnectedGamepads& msg)
{
    if (msg.cmd < 0)
    {
        printf("Response timeout\n");
    }
    else
    {
        printf("Response received; cmd: %02hhx\n", static_cast<std::uint8_t>(msg.cmd));
        int idx = 0;
        for (const dpp_api::GamepadConnectionState& state : msg.gamepadConnectionStates)
        {
            printf("%c: ", 'A' + idx++);
            switch (state)
            {
                case dpp_api::GamepadConnectionState::NOT_CONNECTED:
                    printf("NOT CONNECTED\n");
                    break;
                case dpp_api::GamepadConnectionState::CONNECTED:
                    printf("CONNECTED\n");
                    break;
                case dpp_api::GamepadConnectionState::UNAVAILABLE: // fall through
                default:
                    printf("UNAVAILABLE\n");
                    break;
            }
        }
    }

    update_response_count();
}

void read_complete(const std::string& errStr)
{
    printf("Disconnected%s%s\n", (errStr.empty() ? "" : ": "), errStr.c_str());
    fflush(stdout);
}

void list_all_devices()
{
    dpp_api::DppDevice::Filter filter;
    filter.minBcdDevice = 0x0120;
    std::uint32_t count = dpp_api::DppDevice::getCount(filter);
    printf("%u device(s) found\n", count);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        filter.idx = i;
        std::unique_ptr<dpp_api::DppDevice> currentDppDevice = dpp_api::DppDevice::find(filter);
        if (currentDppDevice)
        {
            printf("   %s\n", currentDppDevice->getSerial().c_str());
        }
    }
}

int main(int argc, char **argv)
{
    list_all_devices();
    dpp_api::DppDevice::Filter filter;
    filter.minBcdDevice = 0x0120;
    std::unique_ptr<dpp_api::DppDevice> dppDevice = dpp_api::DppDevice::find(filter);

    if (dppDevice)
    {
        std::array<std::uint8_t, 3> ver = dppDevice->getVersion();
        printf("FOUND! %s v%hhu.%hhu.%hhu\n", dppDevice->getSerial().c_str(), ver[0], ver[1], ver[2]);
        if (!dppDevice->connect(read_complete))
        {
            printf("Failed to connect: %s\n", dppDevice->getLastErrorStr().c_str());
            return 1;
        }

        std::uint32_t numExpected = 0;

        std::uint64_t sent = 0;

        sent = dppDevice->send(
            dpp_api::msg::tx::Maple32{
                {
                    0x0C010032, 0x00000004, 0x00000000,
                    0x68003FC0, 0x0201FC01, 0xC03C0C03, 0xFE060007, 0x98076F08, 0x0071F00E, 0xFF10000C, 0x601FFF60,
                    0x0002303F, 0x6DC00001, 0x1836FF83, 0x01F0147F, 0xFFBD060E, 0x107F6DCE, 0x3807A0F6, 0xFF8BC807,
                    0xC0FFC30F, 0x0C05407F, 0x1F1F0E05, 0x607E67FD, 0x0B873FC7, 0x8FEB0DFF, 0xF8F13355, 0x0ABFFF3D,
                    0x46AB0D55, 0x63E10F55, 0x0AAAA07B, 0x3AAA0555, 0x6021E356, 0x06AAA03C, 0x82AA0555, 0x60270756,
                    0x02AAA021, 0x06AA0355, 0x40200754, 0x01AAC020, 0x0DAC00D5, 0x40601CF8, 0x006A8070, 0x1410003F,
                    0x00502200, 0x00180048, 0x2200000E, 0x00884100, 0x00000084, 0x41000000, 0x01048080, 0x00000102
                }
            },
            response32,
            500
        );

        if (sent == 0)
        {
            printf("Failed to send\n");
            return 2;
        }

        ++numExpected;
        printf("Sent address: %llu\n", static_cast<long long unsigned>(sent));

        // sent = dppDevice->sendPlayerReset(0, player_reset_response, 500);

        // sent = dppDevice->sendChangePlayerDisplay(0, 1, simple_response, 500);

        // sent = dppDevice->sendGetDcSummary(0, summary_response, 500);

        // sent = dppDevice->sendGetInterfaceVersion(ver_response, 500);

        // sent = dppDevice->sendGetControllerState(0, controller_state_response, 500);

        // sent = dppDevice->sendRefreshGamepad(0, simple_response, 500);

        sent = dppDevice->send(dpp_api::msg::tx::GetConnectedGamepads{}, controller_connection_response, 500);

        if (sent == 0)
        {
            printf("Failed to send\n");
            return 2;
        }

        ++numExpected;
        printf("Sent address: %llu\n", static_cast<long long unsigned>(sent));

        sent = dppDevice->send(dpp_api::msg::tx::GetConnectedGamepads{}, controller_connection_response, 500);

        if (sent == 0)
        {
            printf("Failed to send\n");
            return 2;
        }

        ++numExpected;
        printf("Sent address: %llu\n", static_cast<long long unsigned>(sent));

        {
            std::unique_lock<std::mutex> lock(mainMutex);
            std::uint32_t* c = &respCount;
            mainCv.wait_for(lock, std::chrono::milliseconds(1000), [c, numExpected](){return *c >= numExpected;});
        }

        dpp_api::msg::rx::GetControllerState st = dppDevice->send(dpp_api::msg::tx::GetControllerState{0}, 500);
        controller_state_response(st);

        // std::this_thread::sleep_for(std::chrono::milliseconds(5000));

        printf("Num waiting: %i\n", static_cast<int>(dppDevice->getNumWaiting()));
    }
    else
    {
        printf("not found :(\n");
        return 3;
    }
    return 0;
}
