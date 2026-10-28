#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include "Protocol.h"
#include "Socket.h"

namespace coop {

// Packet layout (little endian):
//   u16 magic, u16 seq, u16 ack, u32 ackBits, u8 messageCount,
//   then per message: u8 type, u8 flags, [u16 reliableId if flags & 1], u16 length, payload.
// Reliable messages are resent until a packet carrying them is acked, and delivered in order.
// Unreliable messages are sent once and delivered only from packets newer than any seen before.
constexpr uint16_t kPacketMagic = 0x4348;

struct Message {
    MsgType type = MsgType::Hello;
    std::vector<uint8_t> payload;
};

struct ConnectionStats {
    double rttMs = 0.0;
    uint64_t packetsSent = 0;
    uint64_t packetsReceived = 0;
    uint64_t packetsMalformed = 0;
    uint64_t resends = 0;
    uint64_t bytesSent = 0;
    uint64_t bytesReceived = 0;
    uint64_t unreliableDropped = 0;
    size_t reliablePending = 0;
    // Of the partner's packets, the share that never arrived (recent, smoothed), and how much the
    // round trip varies.
    double lossPercent = 0.0;
    double jitterMs = 0.0;
};

class Connection {
public:
    static constexpr uint16_t kReliableWindow = 1024;
    static constexpr size_t kMaxPendingReliable = 8192;
    static constexpr size_t kMaxPacketsPerFlush = 16;

    explicit Connection(Endpoint p_Remote, uint16_t p_InitialSeq = 0, uint16_t p_InitialReliableId = 0);

    // Returns false if the payload is too large to ever send.
    bool SendReliable(MsgType p_Type, std::vector<uint8_t> p_Payload);
    bool SendUnreliable(MsgType p_Type, std::vector<uint8_t> p_Payload);

    // Builds the datagrams that should go out now.
    void Flush(double p_Now, std::vector<std::vector<uint8_t>>& p_Out);

    // Processes one datagram from the remote. Returns false if it was malformed (and ignored).
    bool Receive(const uint8_t* p_Data, size_t p_Size, double p_Now);

    bool PopMessage(Message& p_Out);

    const Endpoint& Remote() const { return m_Remote; }
    double LastReceiveTime() const { return m_LastReceiveTime; }
    bool HasReceived() const { return m_HasRemoteSeq; }
    bool Overflowed() const { return m_Overflowed; }
    ConnectionStats Stats() const;

    // Cheap check that a datagram carries our magic and a full header.
    static bool IsPacket(const uint8_t* p_Data, size_t p_Size);

private:
    struct PendingReliable {
        uint16_t id;
        MsgType type;
        std::vector<uint8_t> payload;
        double lastSent = -1.0;
        bool acked = false;
    };

    struct SentPacket {
        uint16_t seq = 0;
        bool valid = false;
        bool acked = false;
        double sendTime = 0.0;
        std::vector<uint16_t> reliableIds;
    };

    struct OutMessage {
        MsgType type;
        bool reliable;
        uint16_t reliableId;
        const std::vector<uint8_t>* payload;
    };

    void ProcessAcks(uint16_t p_Ack, uint32_t p_AckBits, double p_Now);
    void OnPacketAcked(SentPacket& p_Packet, double p_Now);
    void DeliverReliable(uint16_t p_Id, MsgType p_Type, std::vector<uint8_t> p_Payload);
    double ResendInterval() const;
    void WritePacket(const std::vector<OutMessage>& p_Messages, double p_Now, std::vector<std::vector<uint8_t>>& p_Out);

    Endpoint m_Remote;

    uint16_t m_LocalSeq;
    std::array<SentPacket, 1024> m_SentPackets {};

    bool m_HasRemoteSeq = false;
    uint16_t m_RemoteSeq = 0;
    uint32_t m_RecvBits = 0;
    bool m_AckDirty = false;

    uint16_t m_NextReliableId;
    std::deque<PendingReliable> m_PendingReliable;

    uint16_t m_NextExpectedReliable;
    std::map<uint16_t, Message> m_ReliableBuffer;

    std::vector<Message> m_UnreliableOut;
    std::deque<Message> m_Inbox;

    double m_LastSendTime = -1.0;
    double m_LastReceiveTime = 0.0;
    double m_SmoothedRtt = -1.0;
    double m_RttDeviation = 0.0;
    bool m_Overflowed = false;
    // Loss: the partner's sequence numbers seen against those that should have come, by windows.
    uint32_t m_LossExpected = 0;
    uint32_t m_LossReceived = 0;
    bool m_LossKnown = false;
    void CountArrival(uint32_t p_Expected);

    ConnectionStats m_Stats;
};

// True if a is newer than b with 16-bit wraparound.
inline bool SeqGreater(uint16_t a, uint16_t b) {
    return ((a > b) && (a - b <= 32768)) || ((a < b) && (b - a > 32768));
}

} // namespace coop
