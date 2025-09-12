#pragma once

#include <memory>
#include <string>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <functional>

namespace dpp_api
{

class DppDevice
{
private:
    DppDevice(std::unique_ptr<class DppDeviceImp>&& dev);
public:
    virtual ~DppDevice();

    static std::unique_ptr<DppDevice> find(const std::string& serial);

    static std::unique_ptr<DppDevice> findAtIndex(std::size_t idx);

    static std::size_t getCount();

    static std::string getSerialAt(std::size_t idx);

    const std::string& getSerial() const;

    bool connect();

    bool disconnect();

    bool send(
        std::uint8_t cmd,
        const std::vector<std::uint8_t>& payload,
        const std::function<void(std::uint8_t cmd, const std::vector<std::uint8_t>& payload)>& respFn
    );

private:
    void handleReceive(std::uint64_t addr, std::uint8_t cmd, const std::vector<std::uint8_t>& payload);
    void handleReceiveComplete();

private:
    //! Forward declared pointer to internal implementation class
    std::unique_ptr<class DppDeviceImp> mImp;

    using FunctionLookupMap =
        std::unordered_map<
            std::uint64_t,
            std::function<void(std::uint8_t cmd, const std::vector<std::uint8_t>& payload)>
        >;

    FunctionLookupMap mFnLookup;
    std::uint64_t mNextAddr = 0;
    std::mutex mFnLookupMutex;
};

}
