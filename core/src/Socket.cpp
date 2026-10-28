#include "coop/Socket.h"

#include <cstdlib>

#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

namespace coop {

std::string Endpoint::ToString() const {
    if (IsRelay())
        return "relay peer " + std::to_string(relayPeer);

    return std::to_string((ip >> 24) & 0xFF) + "." + std::to_string((ip >> 16) & 0xFF) + "." +
        std::to_string((ip >> 8) & 0xFF) + "." + std::to_string(ip & 0xFF) + ":" + std::to_string(port);
}

static bool EnsureWinsock() {
    static const bool s_Ok = [] {
        WSADATA s_Data;
        return WSAStartup(MAKEWORD(2, 2), &s_Data) == 0;
    }();

    return s_Ok;
}

UdpSocket::~UdpSocket() {
    if (m_Socket != static_cast<uintptr_t>(INVALID_SOCKET))
        closesocket(static_cast<SOCKET>(m_Socket));
}

std::unique_ptr<UdpSocket> UdpSocket::Open(uint16_t p_Port, std::string& p_Error) {
    if (!EnsureWinsock()) {
        p_Error = "WSAStartup failed";
        return nullptr;
    }

    const SOCKET s_Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (s_Socket == INVALID_SOCKET) {
        p_Error = "socket() failed: " + std::to_string(WSAGetLastError());
        return nullptr;
    }

    std::unique_ptr<UdpSocket> s_Result(new UdpSocket());
    s_Result->m_Socket = static_cast<uintptr_t>(s_Socket);

    u_long s_NonBlocking = 1;
    ioctlsocket(s_Socket, FIONBIO, &s_NonBlocking);

    // Stop Windows from reporting ICMP port-unreachable as WSAECONNRESET on later recvfrom calls.
    BOOL s_NewBehavior = FALSE;
    DWORD s_Returned = 0;
    WSAIoctl(s_Socket, _WSAIOW(IOC_VENDOR, 12), &s_NewBehavior, sizeof(s_NewBehavior), nullptr, 0, &s_Returned,
        nullptr, nullptr);

    // Room for about a second of the partner's traffic: a frame that takes long (a load, a hitch)
    // would otherwise lose what arrived meanwhile to the default 64 KB buffer.
    int s_Buffer = 1 << 20;
    setsockopt(s_Socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&s_Buffer), sizeof(s_Buffer));
    setsockopt(s_Socket, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&s_Buffer), sizeof(s_Buffer));

    sockaddr_in s_Addr {};
    s_Addr.sin_family = AF_INET;
    s_Addr.sin_addr.s_addr = htonl(INADDR_ANY);
    s_Addr.sin_port = htons(p_Port);

    if (bind(s_Socket, reinterpret_cast<sockaddr*>(&s_Addr), sizeof(s_Addr)) != 0) {
        p_Error = "Could not bind UDP port " + std::to_string(p_Port) + " (error " + std::to_string(WSAGetLastError()) +
            "). Is another copy already hosting?";
        return nullptr;
    }

    int s_Len = sizeof(s_Addr);
    getsockname(s_Socket, reinterpret_cast<sockaddr*>(&s_Addr), &s_Len);
    s_Result->m_Port = ntohs(s_Addr.sin_port);

    return s_Result;
}

bool UdpSocket::SendTo(const Endpoint& p_To, const uint8_t* p_Data, size_t p_Size) {
    sockaddr_in s_Addr {};
    s_Addr.sin_family = AF_INET;
    s_Addr.sin_addr.s_addr = htonl(p_To.ip);
    s_Addr.sin_port = htons(p_To.port);

    const int s_Sent = sendto(static_cast<SOCKET>(m_Socket), reinterpret_cast<const char*>(p_Data),
        static_cast<int>(p_Size), 0, reinterpret_cast<sockaddr*>(&s_Addr), sizeof(s_Addr));

    return s_Sent == static_cast<int>(p_Size);
}

int UdpSocket::RecvFrom(Endpoint& p_From, uint8_t* p_Buffer, size_t p_Capacity) {
    for (;;) {
        sockaddr_in s_Addr {};
        int s_AddrLen = sizeof(s_Addr);

        const int s_Received = recvfrom(static_cast<SOCKET>(m_Socket), reinterpret_cast<char*>(p_Buffer),
            static_cast<int>(p_Capacity), 0, reinterpret_cast<sockaddr*>(&s_Addr), &s_AddrLen);

        if (s_Received < 0) {
            // WSAEMSGSIZE means an oversized datagram; drop it and keep reading.
            if (WSAGetLastError() == WSAEMSGSIZE)
                continue;

            return -1;
        }

        p_From.ip = ntohl(s_Addr.sin_addr.s_addr);
        p_From.port = ntohs(s_Addr.sin_port);
        return s_Received;
    }
}

bool ResolveEndpoint(const std::string& p_Address, uint16_t p_DefaultPort, Endpoint& p_Out, std::string& p_Error) {
    if (!EnsureWinsock()) {
        p_Error = "WSAStartup failed";
        return false;
    }

    std::string s_Host = p_Address;
    uint16_t s_Port = p_DefaultPort;

    if (const auto s_Colon = p_Address.rfind(':'); s_Colon != std::string::npos) {
        s_Host = p_Address.substr(0, s_Colon);
        const std::string s_PortStr = p_Address.substr(s_Colon + 1);

        char* s_End = nullptr;
        const long s_Value = std::strtol(s_PortStr.c_str(), &s_End, 10);

        if (s_PortStr.empty() || *s_End != '\0' || s_Value <= 0 || s_Value > 65535) {
            p_Error = "Invalid port: " + s_PortStr;
            return false;
        }

        s_Port = static_cast<uint16_t>(s_Value);
    }

    while (!s_Host.empty() && s_Host.front() == ' ')
        s_Host.erase(s_Host.begin());

    while (!s_Host.empty() && s_Host.back() == ' ')
        s_Host.pop_back();

    if (s_Host.empty()) {
        p_Error = "No address given";
        return false;
    }

    addrinfo s_Hints {};
    s_Hints.ai_family = AF_INET;
    s_Hints.ai_socktype = SOCK_DGRAM;

    addrinfo* s_Result = nullptr;

    if (getaddrinfo(s_Host.c_str(), nullptr, &s_Hints, &s_Result) != 0 || !s_Result) {
        p_Error = "Could not resolve " + s_Host;
        return false;
    }

    const auto* s_Addr = reinterpret_cast<const sockaddr_in*>(s_Result->ai_addr);
    p_Out.ip = ntohl(s_Addr->sin_addr.s_addr);
    p_Out.port = s_Port;

    freeaddrinfo(s_Result);
    return true;
}

std::string LocalLanAddress() {
    if (!EnsureWinsock())
        return {};

    const SOCKET s_Socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (s_Socket == INVALID_SOCKET)
        return {};

    // Connecting a UDP socket sends nothing; it only makes Windows pick the outgoing interface.
    sockaddr_in s_Remote {};
    s_Remote.sin_family = AF_INET;
    s_Remote.sin_port = htons(53);
    s_Remote.sin_addr.s_addr = htonl(0x08080808);

    std::string s_Result;
    sockaddr_in s_Local {};
    int s_Len = sizeof(s_Local);

    if (connect(s_Socket, reinterpret_cast<const sockaddr*>(&s_Remote), sizeof(s_Remote)) == 0 &&
        getsockname(s_Socket, reinterpret_cast<sockaddr*>(&s_Local), &s_Len) == 0 && s_Local.sin_addr.s_addr != 0)
        s_Result = Endpoint { ntohl(s_Local.sin_addr.s_addr), 0 }.ToString();

    closesocket(s_Socket);

    // ToString appends ":port".
    if (const auto s_Colon = s_Result.rfind(':'); s_Colon != std::string::npos)
        s_Result.resize(s_Colon);

    return s_Result;
}

std::string LocalTailscaleAddress() {
    if (!EnsureWinsock())
        return {};

    char s_Name[256] {};

    if (gethostname(s_Name, sizeof(s_Name)) != 0)
        return {};

    addrinfo s_Hints {};
    s_Hints.ai_family = AF_INET;
    addrinfo* s_Result = nullptr;

    if (getaddrinfo(s_Name, nullptr, &s_Hints, &s_Result) != 0 || !s_Result)
        return {};

    std::string s_Found;

    for (const addrinfo* s_It = s_Result; s_It && s_Found.empty(); s_It = s_It->ai_next) {
        const uint32_t s_Ip = ntohl(reinterpret_cast<const sockaddr_in*>(s_It->ai_addr)->sin_addr.s_addr);

        // Tailscale hands out addresses from the carrier-grade NAT block 100.64.0.0/10.
        if ((s_Ip & 0xFFC00000u) == 0x64400000u) {
            s_Found = Endpoint { s_Ip, 0 }.ToString();
            s_Found.resize(s_Found.rfind(':'));
        }
    }

    freeaddrinfo(s_Result);
    return s_Found;
}

} // namespace coop
