/*
chatbox - a simple peer-to-peer chat application
written by "Nav2727" (what? you think i would put my real name on the internet?)
license: GNU GPLv3 (https://www.gnu.org/licenses/gpl-3.0.html)
*/
#pragma once

#include "common/common.h"

#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
class UPnPMapper
{
public:
    UPnPMapper() = default;

    ~UPnPMapper()
    {
        if (!discovered_)
            return;

        std::lock_guard lock(mapping_mutex_);
        for (auto& m : mappings_) {
            const auto key = std::string(urls_.controlURL) + "|" + m.port + "|" + m.proto;
            if (mapping_owners_[key] != m.owner) continue;
            mapping_owners_.erase(key);
            UPNP_DeletePortMapping(
                urls_.controlURL,
                data_.first.servicetype,
                m.port.c_str(),
                m.proto.c_str(),
                nullptr);
        }

        FreeUPNPUrls(&urls_);
    }

    bool discover()
    {
        int error = 0;

        UPNPDev* devlist = upnpDiscover(
            2000, nullptr, nullptr, 0, 0, 2, &error);

        if (!devlist)
        {
            lastError_ = "No UPnP devices found (err " + std::to_string(error) + ")";
            return false;
        }

        char lanAddr[64] = {};
        char wanAddr[64] = {};

        int status = UPNP_GetValidIGD(
            devlist, &urls_, &data_,
            lanAddr, sizeof(lanAddr),
            wanAddr, sizeof(wanAddr));

        freeUPNPDevlist(devlist);

        if (status != 1)
        {
            FreeUPNPUrls(&urls_);
            lastError_ = "No valid IGD found (status " + std::to_string(status) + ")";
            return false;
        }

        localIP_ = lanAddr;
        discovered_ = true;

        char extIP[64] = {};
        if (UPNP_GetExternalIPAddress(
            urls_.controlURL,
            data_.first.servicetype,
            extIP) == UPNPCOMMAND_SUCCESS)
        {
            externalIP_ = extIP;
        }

        return true;
    }

    bool openPort(
        const std::string& port,
        const std::string& proto = "TCP",
        const std::string& desc = "P2P Chat")
    {
        if (!discovered_)
            return false;

        std::lock_guard lock(mapping_mutex_);
        int r = UPNP_AddPortMapping(
            urls_.controlURL,
            data_.first.servicetype,
            port.c_str(), port.c_str(),
            localIP_.c_str(), desc.c_str(),
            proto.c_str(), nullptr, "0");

        if (r != UPNPCOMMAND_SUCCESS)
        {
            lastError_ = "AddPortMapping failed: code " + std::to_string(r);
            return false;
        }

        const auto owner = ++next_owner_;
        mapping_owners_[std::string(urls_.controlURL) + "|" + port + "|" + proto] = owner;
        mappings_.push_back({ port, proto, owner });
        return true;
    }

    bool openPortBoth(const std::string& port, const std::string& desc = "P2P Chat")
    {
        bool tcp = openPort(port, "TCP", desc);
        bool udp = openPort(port, "UDP", desc);
        return tcp || udp;
    }

    std::string externalIP() const { return externalIP_; }
    std::string localIP()    const { return localIP_; }
    std::string lastError()  const { return lastError_; }

private:
    struct Mapping { std::string port, proto; std::uint64_t owner; };
    inline static std::mutex mapping_mutex_;
    inline static std::map<std::string, std::uint64_t> mapping_owners_;
    inline static std::uint64_t next_owner_ = 0;

    UPNPUrls    urls_{};
    IGDdatas    data_{};
    std::string localIP_, externalIP_, lastError_;
    bool        discovered_ = false;
    std::vector<Mapping> mappings_;
};
