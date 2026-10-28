#pragma once

#include <memory>
#include <string>

#include "Socket.h"

namespace coop {

constexpr uint16_t kDefaultRelayPort = 47100;

// The host as seen by a client that joined through the relay.
constexpr uint32_t kRelayHostPeer = 0xFFFFFFFFu;

struct RelayState {
    enum class Phase { Connecting, Ready, Failed };

    Phase phase = Phase::Connecting;
    std::string code;   // host: the join code to give friends
    std::string error;  // why it failed
    double rttMs = 0.0;
};

// Talks to the co-op relay server (server/relay.py). A host registers and gets a join code; a
// client joins with a code. After that it behaves like any socket: peers are relay endpoints
// (Endpoint::Relay), and the server forwards packets between them.
class RelaySocket final : public IDatagramSocket {
public:
    static std::unique_ptr<RelaySocket> Host(const Endpoint& p_Server, std::string& p_Error);
    static std::unique_ptr<RelaySocket> Join(const Endpoint& p_Server, const std::string& p_Code, std::string& p_Error);

    bool SendTo(const Endpoint& p_To, const uint8_t* p_Data, size_t p_Size) override;

    // Also runs the relay's own timers (registration retries, keepalives), so call it regularly.
    int RecvFrom(Endpoint& p_From, uint8_t* p_Buffer, size_t p_Capacity) override;

    // Shared, so it can still be read after the session drops the socket (for example to show
    // why it failed). Only touch it on the thread that ticks the session.
    std::shared_ptr<const RelayState> State() const { return m_State; }

    // Uppercases and strips spaces and dashes. True if the result looks like a join code.
    static bool NormalizeCode(const std::string& p_Input, std::string& p_Code);

private:
    RelaySocket() = default;

    void Pump();
    void SendControl(uint8_t p_Type, const uint8_t* p_Body, size_t p_Size, bool p_Pad);
    void SendHello();
    void HandleControl(uint8_t p_Type, const uint8_t* p_Body, size_t p_Size);
    void Fail(const std::string& p_Error);

    std::unique_ptr<UdpSocket> m_Socket;
    Endpoint m_Server;
    bool m_IsHost = false;
    std::string m_JoinCode;
    uint32_t m_Nonce = 0;

    std::shared_ptr<RelayState> m_State = std::make_shared<RelayState>();
    double m_Started = 0.0;
    double m_LastHello = -1.0;
    double m_LastPing = 0.0;
    double m_PingSent = -1.0;
    double m_LastHeard = 0.0;

    uint8_t m_Packet[1500] {}; // receive buffer

};

} // namespace coop
