#include "coop/RelaySocket.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <random>

namespace coop {

// Wire format, shared with server/relay.py. Big-endian.
//   b"HCR1" | type u8 | body
//   HOST_REGISTER  version u8, nonce u32              (padded to 64 bytes)
//   HOST_OK        nonce u32, code[6]
//   JOIN           version u8, code[6]                (padded to 64 bytes)
//   JOIN_OK        -
//   DATA           peer u32, payload                  (to the host: peer = guest id; to a guest: 0)
//   ERROR          code u8, text
//   PING / PONG    -
//   PEER_LEFT      peer u32
namespace {

constexpr uint8_t c_Magic[4] = { 'H', 'C', 'R', '1' };
constexpr uint8_t c_RelayProtocol = 1;
constexpr size_t c_MinRequest = 64;
constexpr size_t c_CodeLength = 6;
constexpr const char* c_CodeAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";

enum : uint8_t { MsgHostRegister = 1, MsgHostOk, MsgJoin, MsgJoinOk, MsgData, MsgError, MsgPing, MsgPong, MsgPeerLeft };

constexpr double c_HelloInterval = 0.5;
constexpr double c_ConnectTimeout = 8.0; // below the session's 10 s, so this clearer error shows first
constexpr double c_PingInterval = 2.0;
constexpr double c_ServerTimeout = 12.0;

double Clock() {
    using namespace std::chrono;
    static const auto s_Start = steady_clock::now();
    return duration<double>(steady_clock::now() - s_Start).count();
}

void PutU32(uint8_t* p_Out, uint32_t p_Value) {
    p_Out[0] = static_cast<uint8_t>(p_Value >> 24);
    p_Out[1] = static_cast<uint8_t>(p_Value >> 16);
    p_Out[2] = static_cast<uint8_t>(p_Value >> 8);
    p_Out[3] = static_cast<uint8_t>(p_Value);
}

uint32_t GetU32(const uint8_t* p_In) {
    return (static_cast<uint32_t>(p_In[0]) << 24) | (static_cast<uint32_t>(p_In[1]) << 16) |
        (static_cast<uint32_t>(p_In[2]) << 8) | p_In[3];
}

} // namespace

bool RelaySocket::NormalizeCode(const std::string& p_Input, std::string& p_Code) {
    p_Code.clear();

    for (const char c : p_Input) {
        if (c == ' ' || c == '-' || c == '\t')
            continue;

        const char s_Upper = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

        if (!std::strchr(c_CodeAlphabet, s_Upper) || s_Upper == '\0')
            return false;

        p_Code.push_back(s_Upper);
    }

    return p_Code.size() == c_CodeLength;
}

std::unique_ptr<RelaySocket> RelaySocket::Host(const Endpoint& p_Server, std::string& p_Error) {
    auto s_Udp = UdpSocket::Open(0, p_Error);

    if (!s_Udp)
        return nullptr;

    std::unique_ptr<RelaySocket> s_Relay(new RelaySocket());
    s_Relay->m_Socket = std::move(s_Udp);
    s_Relay->m_Server = p_Server;
    s_Relay->m_IsHost = true;
    s_Relay->m_Nonce = std::random_device {}();
    s_Relay->m_Started = s_Relay->m_LastHeard = Clock();
    s_Relay->SendHello();
    return s_Relay;
}

std::unique_ptr<RelaySocket> RelaySocket::Join(const Endpoint& p_Server, const std::string& p_Code, std::string& p_Error) {
    std::string s_Code;

    if (!NormalizeCode(p_Code, s_Code)) {
        p_Error = "That doesn't look like a join code (6 letters and digits).";
        return nullptr;
    }

    auto s_Udp = UdpSocket::Open(0, p_Error);

    if (!s_Udp)
        return nullptr;

    std::unique_ptr<RelaySocket> s_Relay(new RelaySocket());
    s_Relay->m_Socket = std::move(s_Udp);
    s_Relay->m_Server = p_Server;
    s_Relay->m_JoinCode = s_Code;
    s_Relay->m_Started = s_Relay->m_LastHeard = Clock();
    s_Relay->SendHello();
    return s_Relay;
}

void RelaySocket::SendControl(uint8_t p_Type, const uint8_t* p_Body, size_t p_Size, bool p_Pad) {
    uint8_t s_Packet[128] {};

    if (5 + p_Size > sizeof(s_Packet))
        return;

    std::memcpy(s_Packet, c_Magic, 4);
    s_Packet[4] = p_Type;

    if (p_Size)
        std::memcpy(s_Packet + 5, p_Body, p_Size);

    const size_t s_Total = p_Pad ? std::max(5 + p_Size, c_MinRequest) : 5 + p_Size;
    m_Socket->SendTo(m_Server, s_Packet, s_Total);
}

void RelaySocket::SendHello() {
    m_LastHello = Clock();

    if (m_IsHost) {
        uint8_t s_Body[5] = { c_RelayProtocol };
        PutU32(s_Body + 1, m_Nonce);
        SendControl(MsgHostRegister, s_Body, sizeof(s_Body), true);
    }
    else {
        uint8_t s_Body[1 + c_CodeLength] = { c_RelayProtocol };
        std::memcpy(s_Body + 1, m_JoinCode.data(), c_CodeLength);
        SendControl(MsgJoin, s_Body, sizeof(s_Body), true);
    }
}

void RelaySocket::Fail(const std::string& p_Error) {
    if (m_State->phase == RelayState::Phase::Failed)
        return;

    m_State->phase = RelayState::Phase::Failed;
    m_State->error = p_Error;
}

void RelaySocket::Pump() {
    const double s_Now = Clock();

    switch (m_State->phase) {
        case RelayState::Phase::Connecting:
            if (s_Now - m_Started > c_ConnectTimeout)
                Fail("Can't reach the co-op server at " + m_Server.ToString() + ".");
            else if (s_Now - m_LastHello >= c_HelloInterval)
                SendHello();

            break;

        case RelayState::Phase::Ready:
            if (s_Now - m_LastHeard > c_ServerTimeout) {
                Fail("Lost the connection to the co-op server.");
            }
            else if (s_Now - m_LastPing >= c_PingInterval) {
                m_LastPing = m_PingSent = s_Now;
                SendControl(MsgPing, nullptr, 0, false);
            }

            break;

        case RelayState::Phase::Failed:
            break;
    }
}

void RelaySocket::HandleControl(uint8_t p_Type, const uint8_t* p_Body, size_t p_Size) {
    switch (p_Type) {
        case MsgHostOk:
            if (m_IsHost && m_State->phase == RelayState::Phase::Connecting && p_Size >= 4 + c_CodeLength &&
                GetU32(p_Body) == m_Nonce) {
                m_State->code.assign(reinterpret_cast<const char*>(p_Body + 4), c_CodeLength);
                m_State->phase = RelayState::Phase::Ready;
            }

            break;

        case MsgJoinOk:
            if (!m_IsHost && m_State->phase == RelayState::Phase::Connecting)
                m_State->phase = RelayState::Phase::Ready;

            break;

        case MsgError: {
            std::string s_Text = p_Size > 1 ? std::string(reinterpret_cast<const char*>(p_Body + 1), p_Size - 1) : "";

            // Only printable text from the server.
            for (auto& c : s_Text) {
                if (c < 0x20 || c > 0x7E)
                    c = '?';
            }

            Fail(s_Text.empty() ? "The co-op server refused the request." : s_Text);
            break;
        }

        case MsgPong:
            if (m_PingSent >= 0.0) {
                const double s_Sample = (Clock() - m_PingSent) * 1000.0;
                m_State->rttMs = m_State->rttMs <= 0.0 ? s_Sample : m_State->rttMs * 0.8 + s_Sample * 0.2;
                m_PingSent = -1.0;
            }

            break;

        default:
            break; // PEER_LEFT and anything unknown: the session notices departures itself
    }
}

bool RelaySocket::SendTo(const Endpoint& p_To, const uint8_t* p_Data, size_t p_Size) {
    if (m_State->phase != RelayState::Phase::Ready || !p_To.IsRelay() || p_Size > sizeof(m_Packet) - 9)
        return false;

    uint8_t s_Packet[sizeof(m_Packet)];
    std::memcpy(s_Packet, c_Magic, 4);
    s_Packet[4] = MsgData;
    PutU32(s_Packet + 5, m_IsHost ? p_To.relayPeer : 0);
    std::memcpy(s_Packet + 9, p_Data, p_Size);
    return m_Socket->SendTo(m_Server, s_Packet, 9 + p_Size);
}

int RelaySocket::RecvFrom(Endpoint& p_From, uint8_t* p_Buffer, size_t p_Capacity) {
    Pump();

    while (true) {
        Endpoint s_From;
        const int s_Size = m_Socket->RecvFrom(s_From, m_Packet, sizeof(m_Packet));

        if (s_Size < 0)
            return -1;

        // Only the server talks to us, and only in our format.
        if (s_From != m_Server || s_Size < 5 || std::memcmp(m_Packet, c_Magic, 4) != 0)
            continue;

        m_LastHeard = Clock();
        const uint8_t s_Type = m_Packet[4];

        if (s_Type != MsgData) {
            HandleControl(s_Type, m_Packet + 5, static_cast<size_t>(s_Size) - 5);
            continue;
        }

        if (m_State->phase != RelayState::Phase::Ready || s_Size < 9)
            continue;

        const uint32_t s_Peer = GetU32(m_Packet + 5);
        const size_t s_Payload = static_cast<size_t>(s_Size) - 9;

        if (s_Payload > p_Capacity || (m_IsHost && s_Peer == 0))
            continue;

        std::memcpy(p_Buffer, m_Packet + 9, s_Payload);
        p_From = Endpoint::Relay(m_IsHost ? s_Peer : kRelayHostPeer);
        return static_cast<int>(s_Payload);
    }
}

} // namespace coop
