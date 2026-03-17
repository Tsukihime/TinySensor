/*
 * MQTT Gateway Fragmented Protocol
 * =================================
 *
 * Overview:
 * This protocol enables transmission of MQTT topic/payload messages over
 * a constrained transport layer by fragmenting large messages into fixed-size packets.
 *
 * Packet Header (1 Byte):
 * [7:3] Reserved
 * [2]   Retained Flag (MQTT retained message flag)
 * [1:0] Marker (Fragmentation state)
 *       0b00 = START      (First fragment)
 *       0b01 = NEXT       (Middle fragment)
 *       0b10 = STOP       (Last fragment)
 *       0b11 = START_STOP (Single packet message)
 *
 * First Packet Structure:
 * +----------------+----------------+----------------+----------------+
 * | Packet Header  | Topic Len (1B) | Payload Len (2B)| Data ...       |
 * +----------------+----------------+----------------+----------------+
 *
 * Subsequent Packet Structure:
 * +----------------+----------------+
 * | Packet Header  | Data ...       |
 * +----------------+----------------+
 *
 * Data Stream:
 * 1. Topic bytes are transmitted first.
 * 2. Payload bytes follow immediately after topic bytes.
 * 3. Receiver inserts a null-terminator ('\0') between topic and payload
 *    in the reassembly buffer before invoking the receive callback.
 *
 * Constraints:
 * - Maximum Topic Length: 255 bytes (uint8_t)
 * - Maximum Payload Length: 65535 bytes (uint16_t)
 */

#ifndef MQTTGATEWAY_H_
#define MQTTGATEWAY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifdef __AVR__
#include <avr/pgmspace.h>
#else
#ifndef pgm_read_byte
#define pgm_read_byte(ptr) (*(ptr))
#endif
#ifndef strlen_P
#define strlen_P strlen
#endif
#endif

namespace MQTTGateway {

enum Marker : uint8_t {
    START = 0,
    NEXT = 1,
    STOP = 2,
    START_STOP = 3
};

struct PacketHeader {
    uint8_t marker : 2;
    uint8_t retained : 1;
    uint8_t reserved : 5;
} __attribute__((packed));

struct FirstPacketHeader : public PacketHeader {
    uint8_t topic_length;
    uint16_t payload_length;
} __attribute__((packed));

template <size_t PacketSize = 32>
struct FirstPacket : public FirstPacketHeader {
    uint8_t data[PacketSize - sizeof(FirstPacketHeader)];
} __attribute__((packed));

template <size_t PacketSize = 32>
struct NextPacket : public PacketHeader {
    uint8_t data[PacketSize - sizeof(PacketHeader)];
} __attribute__((packed));

template <size_t PacketSize = 32>
struct Packet {
    union {
        PacketHeader header;
        FirstPacket<PacketSize> first;
        NextPacket<PacketSize> next;
    };
} __attribute__((packed));

using WriteCallback = bool (*)(const void *data, size_t length);
using GetDataCallback = uint8_t (*)(const char *, uintptr_t index);
using MessageCallback = void (*)(const char *topic,
                                 uint8_t *payload,
                                 uint16_t payload_length,
                                 bool retained);

template <size_t PacketSize = 32>
class Transmitter {
private:
    WriteCallback write_callback = nullptr;

public:
    explicit Transmitter(WriteCallback cb = nullptr) : write_callback(cb) {}

    bool send(const char *topic, uint8_t topic_len,
              const char *payload, uint16_t payload_len, bool retained,
              GetDataCallback getdata = nullptr) {
        Packet<PacketSize> packet;
        uint16_t total_size = topic_len + payload_len;
        uint16_t sent = 0;
        bool success = false;

        while (sent < total_size) {
            uint16_t bytes_to_copy;
            uint8_t *data;

            if (sent == 0) {
                packet.header.marker = Marker::START;
                packet.header.retained = retained;
                packet.first.topic_length = topic_len;
                packet.first.payload_length = payload_len;
                data = packet.first.data;
                bytes_to_copy = sizeof(decltype(packet.first.data));
            } else {
                packet.header.marker = Marker::NEXT;
                data = packet.next.data;
                bytes_to_copy = sizeof(decltype(packet.next.data));
            }

            if ((total_size - sent) <= bytes_to_copy) {
                bytes_to_copy = total_size - sent;
                packet.header.marker = (sent == 0) ? Marker::START_STOP : Marker::STOP;
            }

            for (uint16_t i = 0; i < bytes_to_copy; i++) {
                uint16_t offset = sent + i;
                if (offset < topic_len) {
                    data[i] = getdata ? getdata(topic, offset) : topic[offset];
                } else {
                    offset -= topic_len;
                    data[i] = getdata ? getdata(payload, offset) : payload[offset];
                }
            }

            size_t header_size = (sent == 0) ? sizeof(FirstPacketHeader) : sizeof(PacketHeader);
            sent += bytes_to_copy;
            success = write_callback ? write_callback(&packet, header_size + bytes_to_copy) : false;
            if (!success) {
                break;
            }
        }
        return success;
    }

    bool publish(const char *topic, const char *payload, bool retained = false) {
        return send(topic, static_cast<uint8_t>(strlen(topic)),
                    payload, static_cast<uint16_t>(strlen(payload)), retained);
    }

    bool publish_P(const char *topic, const char *payload_P, bool retained = false) {
        return send(topic, static_cast<uint8_t>(strlen_P(topic)),
                    payload_P, static_cast<uint16_t>(strlen_P(payload_P)), retained,
                    [](const char *ptr, uintptr_t index) -> uint8_t {
                        return pgm_read_byte(ptr + index);
                    });
    }
};

template <size_t BufferSize = 1024>
class Receiver {
private:
    MessageCallback callback;

    uint8_t topic_length;
    uint16_t payload_length;
    bool retained;
    size_t pos;
    bool assembling;

    uint8_t buffer[BufferSize];

public:
    explicit Receiver(MessageCallback cb) : callback(cb), assembling(false) {}

    bool isAssembling() const {
        return assembling;
    }

    bool parsePacket(const uint8_t *packet, size_t payloadSize) {
        if (payloadSize < sizeof(FirstPacketHeader)) {
            return false;
        }

        const uint8_t *data;
        size_t block_sz;

        const FirstPacketHeader *pk = reinterpret_cast<const FirstPacketHeader *>(packet);

        if (pk->marker == Marker::START ||
            pk->marker == Marker::START_STOP) {
            pos = 0;
            assembling = true;

            topic_length = pk->topic_length;
            payload_length = pk->payload_length;
            retained = pk->retained;
            data = packet + sizeof(FirstPacketHeader);
            block_sz = payloadSize - sizeof(FirstPacketHeader);
        } else {
            if (!assembling) {
                return false;
            }
            data = packet + sizeof(PacketHeader);
            block_sz = payloadSize - sizeof(PacketHeader);
        }

        if ((pos + block_sz + 1 /* topic nullterm */) > sizeof(buffer)) {
            assembling = false;
            return false;
        }

        // Copy data with null-terminator insertion at topic/payload boundary
        if (pos < topic_length && pos + block_sz >= topic_length) {
            // This block crosses the topic/payload boundary
            // Copy remaining topic bytes
            uint16_t topic_remaining = topic_length - pos;
            memcpy(&buffer[pos], data, topic_remaining);
            pos += topic_remaining;

            // Insert null-terminator after topic
            buffer[pos] = '\0';
            pos++;

            // Copy remaining block bytes as payload
            uint16_t payload_remaining = block_sz - topic_remaining;
            if (payload_remaining > 0) {
                memcpy(&buffer[pos], data + topic_remaining, payload_remaining);
                pos += payload_remaining;
            }
        } else {
            // Block is entirely within topic or payload - copy normally
            memcpy(&buffer[pos], data, block_sz);
            pos += block_sz;
        }

        if (pk->marker == Marker::STOP ||
            pk->marker == Marker::START_STOP) {

            if ((payload_length + topic_length + 1U /* topic nullterm */) != pos) {
                assembling = false;
                return false;
            }

            uint8_t *payload_ptr = &buffer[topic_length + 1 /* topic nullterm */];

            if (callback != nullptr) {
                callback(reinterpret_cast<const char *>(buffer),
                         payload_ptr,
                         payload_length,
                         retained);
            }

            assembling = false;
        }
        return true;
    }
};

} // namespace MQTTGateway

template <size_t PacketSize = 32>
using MQTTGatewayTransmitter = MQTTGateway::Transmitter<PacketSize>;

template <size_t BufferSize = 1024>
using MQTTGatewayReceiver = MQTTGateway::Receiver<BufferSize>;

#endif /* MQTTGATEWAY_H_ */
