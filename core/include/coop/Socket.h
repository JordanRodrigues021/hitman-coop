#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace coop {

// Where a peer is: an IPv4 address and port (host byte order), or a peer reached through the
// co-op relay server when relayPeer is set (see RelaySocket).
struct Endpoint {
    uint32_t ip = 0;
    uint16_t port = 0;
    uint32_t relayPeer = 0;

    bool IsRelay() const { return relayPeer != 0; }

    bool operator==(const Endpoint& p_Other) const {
        return ip == p_Other.ip && port == p_Other.port && relayPeer == p_Other.relayPeer;
    }

    bool operator!=(const Endpoint& p_Other) const { return !(*this == p_Other); }

    static Endpoint Relay(uint32_t p_Peer) { return { 0, 0, p_Peer }; }

    std::string ToString() const;
};

class IDatagramSocket {
public:
    virtual ~IDatagramSocket() = default;

    virtual bool SendTo(const Endpoint& p_To, const uint8_t* p_Data, size_t p_Size) = 0;

    // Non-blocking. Returns the datagram size, or -1 when nothing is waiting.
    virtual int RecvFrom(Endpoint& p_From, uint8_t* p_Buffer, size_t p_Capacity) = 0;
};

// Winsock UDP socket. Bind port 0 to get an ephemeral port.
class UdpSocket final : public IDatagramSocket {
public:
    ~UdpSocket() override;

    static std::unique_ptr<UdpSocket> Open(uint16_t p_Port, std::string& p_Error);

    bool SendTo(const Endpoint& p_To, const uint8_t* p_Data, size_t p_Size) override;
    int RecvFrom(Endpoint& p_From, uint8_t* p_Buffer, size_t p_Capacity) override;

    uint16_t LocalPort() const { return m_Port; }

private:
    UdpSocket() = default;

    uintptr_t m_Socket = ~static_cast<uintptr_t>(0);
    uint16_t m_Port = 0;
};

// One socket over two transports: relay endpoints go to p_Relay, everything else to p_Udp.
// Either may be null. A host listens on both, so friends can join with a code or by address.
class MultiSocket final : public IDatagramSocket {
public:
    MultiSocket(std::unique_ptr<IDatagramSocket> p_Udp, std::unique_ptr<IDatagramSocket> p_Relay) :
        m_Udp(std::move(p_Udp)), m_Relay(std::move(p_Relay)) {}

    bool SendTo(const Endpoint& p_To, const uint8_t* p_Data, size_t p_Size) override {
        IDatagramSocket* s_Socket = p_To.IsRelay() ? m_Relay.get() : m_Udp.get();
        return s_Socket && s_Socket->SendTo(p_To, p_Data, p_Size);
    }

    int RecvFrom(Endpoint& p_From, uint8_t* p_Buffer, size_t p_Capacity) override {
        // Alternate so a flood on one transport can't starve the other.
        for (int i = 0; i < 2; ++i) {
            m_RelayFirst = !m_RelayFirst;
            IDatagramSocket* s_Socket = m_RelayFirst ? m_Relay.get() : m_Udp.get();

            if (s_Socket) {
                const int s_Size = s_Socket->RecvFrom(p_From, p_Buffer, p_Capacity);

                if (s_Size >= 0)
                    return s_Size;
            }
        }

        return -1;
    }

private:
    std::unique_ptr<IDatagramSocket> m_Udp;
    std::unique_ptr<IDatagramSocket> m_Relay;
    bool m_RelayFirst = false;
};

// Resolves "host", "host:port", or "a.b.c.d:port". Uses p_DefaultPort when no port is given.
bool ResolveEndpoint(const std::string& p_Address, uint16_t p_DefaultPort, Endpoint& p_Out, std::string& p_Error);

// This PC's address on the local network (the one a router's port forward should point at),
// or empty if there is no network. Sends no packets.
std::string LocalLanAddress();

// This PC's Tailscale address (100.64.0.0/10), or empty when Tailscale isn't running.
std::string LocalTailscaleAddress();

} // namespace coop
