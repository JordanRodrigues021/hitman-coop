#include "coop/Connection.h"

#include <algorithm>
#include <cmath>

namespace coop {

static constexpr size_t c_HeaderSize = 2 + 2 + 2 + 4 + 1;
static constexpr size_t c_MessageHeaderMax = 1 + 1 + 2 + 2;
static constexpr uint8_t c_FlagReliable = 1;

Connection::Connection(Endpoint p_Remote, uint16_t p_InitialSeq, uint16_t p_InitialReliableId) :
    m_Remote(p_Remote),
    m_LocalSeq(p_InitialSeq),
    m_NextReliableId(p_InitialReliableId),
    m_NextExpectedReliable(p_InitialReliableId) {}

bool Connection::SendReliable(MsgType p_Type, std::vector<uint8_t> p_Payload) {
    if (p_Payload.size() > kMaxMessageSize)
        return false;

    if (m_PendingReliable.size() >= kMaxPendingReliable) {
        m_Overflowed = true;
        return false;
    }

    m_PendingReliable.push_back({ m_NextReliableId++, p_Type, std::move(p_Payload) });
    return true;
}

bool Connection::SendUnreliable(MsgType p_Type, std::vector<uint8_t> p_Payload) {
    if (p_Payload.size() > kMaxMessageSize) {
        ++m_Stats.unreliableDropped;
        return false;
    }

    m_UnreliableOut.push_back({ p_Type, std::move(p_Payload) });
    return true;
}

double Connection::ResendInterval() const {
    if (m_SmoothedRtt < 0.0)
        return 0.2;

    return std::clamp(m_SmoothedRtt * 1.25 + 0.03, 0.05, 1.0);
}

void Connection::Flush(double p_Now, std::vector<std::vector<uint8_t>>& p_Out) {
    std::vector<OutMessage> s_Batch;
    size_t s_BatchBytes = c_HeaderSize;
    size_t s_PacketsBuilt = 0;

    const auto s_Push = [&](const OutMessage& p_Msg) -> bool {
        const size_t s_Size = c_MessageHeaderMax + p_Msg.payload->size();

        if (s_BatchBytes + s_Size > kMaxPacketSize || s_Batch.size() == 255) {
            if (s_PacketsBuilt == kMaxPacketsPerFlush)
                return false;

            WritePacket(s_Batch, p_Now, p_Out);
            ++s_PacketsBuilt;
            s_Batch.clear();
            s_BatchBytes = c_HeaderSize;

            if (s_PacketsBuilt == kMaxPacketsPerFlush)
                return false;
        }

        s_Batch.push_back(p_Msg);
        s_BatchBytes += s_Size;
        return true;
    };

    const double s_Resend = ResendInterval();
    const uint16_t s_WindowStart = m_PendingReliable.empty() ? m_NextReliableId : m_PendingReliable.front().id;
    bool s_Full = false;

    for (auto& s_Pending : m_PendingReliable) {
        if (s_Pending.acked)
            continue;

        // Never run further ahead than the receiver's reorder window.
        if (static_cast<uint16_t>(s_Pending.id - s_WindowStart) >= kReliableWindow)
            break;

        if (s_Pending.lastSent >= 0.0 && p_Now - s_Pending.lastSent < s_Resend)
            continue;

        if (!s_Push({ s_Pending.type, true, s_Pending.id, &s_Pending.payload })) {
            s_Full = true;
            break;
        }

        if (s_Pending.lastSent >= 0.0)
            ++m_Stats.resends;

        s_Pending.lastSent = p_Now;
    }

    for (auto& s_Msg : m_UnreliableOut) {
        if (s_Full || !s_Push({ s_Msg.type, false, 0, &s_Msg.payload })) {
            s_Full = true;
            ++m_Stats.unreliableDropped;
        }
    }

    const bool s_KeepAliveDue = m_LastSendTime < 0.0 || p_Now - m_LastSendTime >= 0.1;

    if (!s_Batch.empty() || (s_PacketsBuilt == 0 && (m_AckDirty || s_KeepAliveDue)))
        WritePacket(s_Batch, p_Now, p_Out);

    m_UnreliableOut.clear();
}

void Connection::WritePacket(
    const std::vector<OutMessage>& p_Messages, double p_Now, std::vector<std::vector<uint8_t>>& p_Out
) {
    ByteWriter s_Writer;
    const uint16_t s_Seq = m_LocalSeq++;

    s_Writer.U16(kPacketMagic);
    s_Writer.U16(s_Seq);
    s_Writer.U16(m_RemoteSeq);
    s_Writer.U32(m_HasRemoteSeq ? m_RecvBits : 0);
    s_Writer.U8(static_cast<uint8_t>(p_Messages.size()));

    auto& s_Sent = m_SentPackets[s_Seq % m_SentPackets.size()];
    s_Sent.seq = s_Seq;
    s_Sent.valid = true;
    s_Sent.acked = false;
    s_Sent.sendTime = p_Now;
    s_Sent.reliableIds.clear();

    for (const auto& s_Msg : p_Messages) {
        s_Writer.U8(static_cast<uint8_t>(s_Msg.type));
        s_Writer.U8(s_Msg.reliable ? c_FlagReliable : 0);

        if (s_Msg.reliable) {
            s_Writer.U16(s_Msg.reliableId);
            s_Sent.reliableIds.push_back(s_Msg.reliableId);
        }

        s_Writer.U16(static_cast<uint16_t>(s_Msg.payload->size()));
        s_Writer.Raw(s_Msg.payload->data(), s_Msg.payload->size());
    }

    m_Stats.bytesSent += s_Writer.Size();
    ++m_Stats.packetsSent;
    m_LastSendTime = p_Now;
    m_AckDirty = false;

    p_Out.push_back(std::move(s_Writer.Data()));
}

bool Connection::IsPacket(const uint8_t* p_Data, size_t p_Size) {
    return p_Size >= c_HeaderSize && p_Size <= kMaxPacketSize && p_Data[0] == (kPacketMagic & 0xFF) &&
        p_Data[1] == (kPacketMagic >> 8);
}

bool Connection::Receive(const uint8_t* p_Data, size_t p_Size, double p_Now) {
    if (!IsPacket(p_Data, p_Size)) {
        ++m_Stats.packetsMalformed;
        return false;
    }

    ByteReader s_Reader(p_Data, p_Size);
    uint16_t s_Magic, s_Seq, s_Ack;
    uint32_t s_AckBits;
    uint8_t s_Count;

    s_Reader.U16(s_Magic);
    s_Reader.U16(s_Seq);
    s_Reader.U16(s_Ack);
    s_Reader.U32(s_AckBits);
    s_Reader.U8(s_Count);

    // Parse every message before acting on any, so a malformed packet changes nothing.
    struct Parsed {
        MsgType type;
        bool reliable;
        uint16_t id;
        const uint8_t* data;
        uint16_t size;
    };

    std::vector<Parsed> s_Messages;
    s_Messages.reserve(s_Count);

    for (uint8_t i = 0; i < s_Count; ++i) {
        uint8_t s_Type, s_Flags;
        uint16_t s_Id = 0, s_Len;

        if (!s_Reader.U8(s_Type) || !s_Reader.U8(s_Flags))
            break;

        if (s_Type == 0 || s_Type >= static_cast<uint8_t>(MsgType::Count) || (s_Flags & ~c_FlagReliable)) {
            s_Reader.Fail();
            break;
        }

        const bool s_Reliable = (s_Flags & c_FlagReliable) != 0;

        if (s_Reliable && !s_Reader.U16(s_Id))
            break;

        if (!s_Reader.U16(s_Len) || s_Len > kMaxMessageSize)
            break;

        const uint8_t* s_Payload = s_Reader.Cursor();

        if (!s_Reader.Skip(s_Len))
            break;

        s_Messages.push_back({ static_cast<MsgType>(s_Type), s_Reliable, s_Id, s_Payload, s_Len });
    }

    if (!s_Reader.AtEnd() || s_Messages.size() != s_Count) {
        ++m_Stats.packetsMalformed;
        return false;
    }

    m_Stats.bytesReceived += p_Size;
    ++m_Stats.packetsReceived;
    m_LastReceiveTime = p_Now;

    // Track the remote's sequence numbers for our acks, and spot duplicates and stale packets.
    bool s_IsNewest = false;
    bool s_IsDuplicate = false;

    if (!m_HasRemoteSeq) {
        m_HasRemoteSeq = true;
        m_RemoteSeq = s_Seq;
        m_RecvBits = 0;
        s_IsNewest = true;
        CountArrival(1);
    }
    else if (SeqGreater(s_Seq, m_RemoteSeq)) {
        const uint16_t s_Shift = static_cast<uint16_t>(s_Seq - m_RemoteSeq);
        CountArrival(s_Shift);

        if (s_Shift > 32)
            m_RecvBits = 0;
        else if (s_Shift == 32)
            m_RecvBits = 1u << 31;
        else
            m_RecvBits = (m_RecvBits << s_Shift) | (1u << (s_Shift - 1));

        m_RemoteSeq = s_Seq;
        s_IsNewest = true;
    }
    else {
        const uint16_t s_Diff = static_cast<uint16_t>(m_RemoteSeq - s_Seq);

        if (s_Diff == 0 || s_Diff > 32) {
            s_IsDuplicate = true;
        }
        else {
            const uint32_t s_Bit = 1u << (s_Diff - 1);

            if (m_RecvBits & s_Bit)
                s_IsDuplicate = true;
            else {
                m_RecvBits |= s_Bit;
                CountArrival(0); // late, but it came: counted when the gap was
            }
        }
    }

    ProcessAcks(s_Ack, s_AckBits, p_Now);

    if (s_IsDuplicate)
        return true;

    m_AckDirty = true;

    for (const auto& s_Msg : s_Messages) {
        std::vector<uint8_t> s_Payload(s_Msg.data, s_Msg.data + s_Msg.size);

        if (s_Msg.reliable)
            DeliverReliable(s_Msg.id, s_Msg.type, std::move(s_Payload));
        else if (s_IsNewest)
            m_Inbox.push_back({ s_Msg.type, std::move(s_Payload) });
    }

    return true;
}

void Connection::ProcessAcks(uint16_t p_Ack, uint32_t p_AckBits, double p_Now) {
    for (uint32_t i = 0; i <= 32; ++i) {
        if (i > 0 && !(p_AckBits & (1u << (i - 1))))
            continue;

        const uint16_t s_Seq = static_cast<uint16_t>(p_Ack - i);
        auto& s_Sent = m_SentPackets[s_Seq % m_SentPackets.size()];

        if (s_Sent.valid && s_Sent.seq == s_Seq && !s_Sent.acked)
            OnPacketAcked(s_Sent, p_Now);
    }

    while (!m_PendingReliable.empty() && m_PendingReliable.front().acked)
        m_PendingReliable.pop_front();
}

void Connection::OnPacketAcked(SentPacket& p_Packet, double p_Now) {
    p_Packet.acked = true;

    const double s_Sample = p_Now - p_Packet.sendTime;

    if (m_SmoothedRtt >= 0.0)
        m_RttDeviation = m_RttDeviation * 0.9 + std::fabs(s_Sample - m_SmoothedRtt) * 0.1;

    m_SmoothedRtt = m_SmoothedRtt < 0.0 ? s_Sample : m_SmoothedRtt * 0.9 + s_Sample * 0.1;

    if (p_Packet.reliableIds.empty() || m_PendingReliable.empty())
        return;

    const uint16_t s_Front = m_PendingReliable.front().id;

    for (const uint16_t s_Id : p_Packet.reliableIds) {
        const uint16_t s_Index = static_cast<uint16_t>(s_Id - s_Front);

        if (s_Index < m_PendingReliable.size() && m_PendingReliable[s_Index].id == s_Id)
            m_PendingReliable[s_Index].acked = true;
    }
}

void Connection::DeliverReliable(uint16_t p_Id, MsgType p_Type, std::vector<uint8_t> p_Payload) {
    const uint16_t s_Ahead = static_cast<uint16_t>(p_Id - m_NextExpectedReliable);

    // Already delivered, or too far ahead to be legitimate.
    if (s_Ahead >= kReliableWindow)
        return;

    if (s_Ahead > 0) {
        m_ReliableBuffer.try_emplace(p_Id, Message { p_Type, std::move(p_Payload) });
        return;
    }

    m_Inbox.push_back({ p_Type, std::move(p_Payload) });
    ++m_NextExpectedReliable;

    for (auto s_It = m_ReliableBuffer.find(m_NextExpectedReliable); s_It != m_ReliableBuffer.end();
         s_It = m_ReliableBuffer.find(m_NextExpectedReliable)) {
        m_Inbox.push_back(std::move(s_It->second));
        m_ReliableBuffer.erase(s_It);
        ++m_NextExpectedReliable;
    }
}

bool Connection::PopMessage(Message& p_Out) {
    if (m_Inbox.empty())
        return false;

    p_Out = std::move(m_Inbox.front());
    m_Inbox.pop_front();
    return true;
}

// Every 100 packets that should have come, the share that did, smoothed over a few windows. A
// gap of more than 32 (the partner's game stalled, or a burst lost) counts as at most 32.
void Connection::CountArrival(uint32_t p_Expected) {
    m_LossExpected += std::min<uint32_t>(p_Expected, 32);
    ++m_LossReceived;

    if (m_LossExpected < 100)
        return;

    const double s_Loss = 100.0 * (1.0 - std::min(1.0, static_cast<double>(m_LossReceived) / m_LossExpected));
    m_Stats.lossPercent = m_LossKnown ? m_Stats.lossPercent * 0.6 + s_Loss * 0.4 : s_Loss;
    m_LossKnown = true;
    m_LossExpected = m_LossReceived = 0;
}

ConnectionStats Connection::Stats() const {
    ConnectionStats s_Stats = m_Stats;
    s_Stats.rttMs = m_SmoothedRtt < 0.0 ? 0.0 : m_SmoothedRtt * 1000.0;
    s_Stats.jitterMs = m_RttDeviation * 1000.0;
    s_Stats.reliablePending = m_PendingReliable.size();
    return s_Stats;
}

} // namespace coop
