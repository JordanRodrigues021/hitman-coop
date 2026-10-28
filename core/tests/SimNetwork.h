#pragma once

#include <map>
#include <memory>
#include <random>
#include <vector>

#include "coop/Socket.h"

// In-memory UDP with configurable latency, jitter (which reorders), loss, duplication and link cuts.
// Deterministic for a given seed.
class SimNetwork : public std::enable_shared_from_this<SimNetwork> {
public:
    struct LinkConfig {
        double minLatency = 0.02;
        double maxLatency = 0.06;
        double loss = 0.0;
        double duplicate = 0.0;
        bool cut = false;
    };

    explicit SimNetwork(uint32_t p_Seed) : m_Rng(p_Seed) {}

    static std::shared_ptr<SimNetwork> Create(uint32_t p_Seed) { return std::make_shared<SimNetwork>(p_Seed); }

    std::unique_ptr<coop::IDatagramSocket> Open(coop::Endpoint p_Endpoint);

    void SetTime(double p_Now) { m_Now = p_Now; }
    double Now() const { return m_Now; }

    LinkConfig config;
    uint64_t delivered = 0;
    uint64_t dropped = 0;

private:
    friend class SimSocket;

    struct InFlight {
        double deliverAt;
        uint64_t order;
        coop::Endpoint from;
        std::vector<uint8_t> data;
    };

    void Send(const coop::Endpoint& p_From, const coop::Endpoint& p_To, const uint8_t* p_Data, size_t p_Size);
    int Receive(const coop::Endpoint& p_At, coop::Endpoint& p_From, uint8_t* p_Buffer, size_t p_Capacity);

    std::mt19937 m_Rng;
    double m_Now = 0.0;
    uint64_t m_Order = 0;
    std::map<uint64_t, std::vector<InFlight>> m_Queues; // keyed by destination endpoint
};

class SimSocket final : public coop::IDatagramSocket {
public:
    SimSocket(std::shared_ptr<SimNetwork> p_Net, coop::Endpoint p_Endpoint) : m_Net(std::move(p_Net)), m_Endpoint(p_Endpoint) {}

    bool SendTo(const coop::Endpoint& p_To, const uint8_t* p_Data, size_t p_Size) override {
        m_Net->Send(m_Endpoint, p_To, p_Data, p_Size);
        return true;
    }

    int RecvFrom(coop::Endpoint& p_From, uint8_t* p_Buffer, size_t p_Capacity) override {
        return m_Net->Receive(m_Endpoint, p_From, p_Buffer, p_Capacity);
    }

private:
    std::shared_ptr<SimNetwork> m_Net;
    coop::Endpoint m_Endpoint;
};

inline uint64_t EndpointKey(const coop::Endpoint& p_Endpoint) {
    // IPv4 keys fit in 48 bits; relay peers get the top bit so they can't collide.
    if (p_Endpoint.IsRelay())
        return (1ull << 63) | p_Endpoint.relayPeer;

    return (static_cast<uint64_t>(p_Endpoint.ip) << 16) | p_Endpoint.port;
}

inline std::unique_ptr<coop::IDatagramSocket> SimNetwork::Open(coop::Endpoint p_Endpoint) {
    return std::make_unique<SimSocket>(shared_from_this(), p_Endpoint);
}

inline void SimNetwork::Send(const coop::Endpoint& p_From, const coop::Endpoint& p_To, const uint8_t* p_Data, size_t p_Size) {
    std::uniform_real_distribution<double> s_Unit(0.0, 1.0);
    std::uniform_real_distribution<double> s_Latency(config.minLatency, config.maxLatency);

    if (config.cut || s_Unit(m_Rng) < config.loss) {
        ++dropped;
        return;
    }

    const int s_Copies = s_Unit(m_Rng) < config.duplicate ? 2 : 1;

    for (int i = 0; i < s_Copies; ++i) {
        m_Queues[EndpointKey(p_To)].push_back(
            { m_Now + s_Latency(m_Rng), m_Order++, p_From, std::vector<uint8_t>(p_Data, p_Data + p_Size) });
    }
}

inline int SimNetwork::Receive(const coop::Endpoint& p_At, coop::Endpoint& p_From, uint8_t* p_Buffer, size_t p_Capacity) {
    auto& s_Queue = m_Queues[EndpointKey(p_At)];
    size_t s_Best = s_Queue.size();

    for (size_t i = 0; i < s_Queue.size(); ++i) {
        if (s_Queue[i].deliverAt > m_Now)
            continue;

        if (s_Best == s_Queue.size() || s_Queue[i].deliverAt < s_Queue[s_Best].deliverAt)
            s_Best = i;
    }

    if (s_Best == s_Queue.size())
        return -1;

    InFlight s_Packet = std::move(s_Queue[s_Best]);
    s_Queue.erase(s_Queue.begin() + static_cast<std::ptrdiff_t>(s_Best));

    if (s_Packet.data.size() > p_Capacity)
        return -1;

    std::copy(s_Packet.data.begin(), s_Packet.data.end(), p_Buffer);
    p_From = s_Packet.from;
    ++delivered;
    return static_cast<int>(s_Packet.data.size());
}
