#pragma once
#include <memory>
#include <unordered_map>
#include <string>
#include <iostream>

namespace SL {
	namespace Remote_Access_Library {
		namespace Network {

			enum class PACKET_TYPES : unsigned int {
				INVALID,
				HTTP_MSG,
				SCREENIMAGE,
				SCREENIMAGEDIF,
				MOUSEPOS,
				MOUSEIMAGE,
				KEYEVENT,
				MOUSEEVENT,
				CLIPBOARDTEXTEVENT,
				//use LAST_PACKET_TYPE as the starting point of your custom packet types. Everything before this is used internally by the library
				LAST_PACKET_TYPE
			};
			//used in the header for http/websocket requests


			class Packet {
				//no copy allowed
				Packet(const Packet&) = delete;
				Packet& operator=(const Packet&) = delete;
				bool _owns_Payload;
				void _clearaftermove() {//makes sure to clear all data after a move
					_owns_Payload = false;
					Packet_Type = 0;
					Payload_Length = 0;
					Payload = nullptr;
				}
			public:
				Packet(unsigned int packet_type) : _owns_Payload(true), Packet_Type(packet_type), Payload(nullptr) {}
				Packet(unsigned int packet_type, unsigned int payloadsize) : _owns_Payload(true),Packet_Type(packet_type), Payload_Length(payloadsize), Payload(new char[payloadsize]){}
				Packet(unsigned int packet_type, unsigned int payloadsize, char* buf, bool take_ownership = true) : _owns_Payload(take_ownership), Packet_Type(packet_type), Payload_Length(payloadsize), Payload(buf) {}
				Packet(unsigned int packet_type, unsigned int payloadsize, std::unordered_map<std::string, std::string>&& header, char* buf, bool take_ownership = true) : _owns_Payload(take_ownership), Packet_Type(packet_type), Payload_Length(payloadsize), Payload(buf), Header(std::move(header)) {}
				Packet(unsigned int packet_type, unsigned int payloadsize, std::unordered_map<std::string, std::string>&& header) :_owns_Payload(true), Packet_Type(packet_type), Payload_Length(payloadsize), Payload(new char[payloadsize]), Header(std::move(header)) {}
				Packet(Packet&& other) {
					operator=(std::move(other));
				}
				Packet& operator=(Packet&& other) {
					Packet_Type = other.Packet_Type;
					Payload_Length = other.Payload_Length;
					Payload = other.Payload;
					Header = std::move(other.Header);
					_owns_Payload = other._owns_Payload;
					other._clearaftermove();
					return *this;
				}
				~Packet() {
					if (_owns_Payload) delete[] Payload;
				}
				std::string get_HeaderValue(const std::string& key) const {
					auto k = Header.find(key);
					if (k != Header.end()) return k->second;
					else return "";//NO THROW PLEASE!
				}
				unsigned int Packet_Type;
				unsigned int Payload_Length;
				char* Payload;
				std::unordered_map<std::string, std::string> Header;
			};
			inline std::ostream& operator<<(std::ostream& os, const Packet& r)
			{
				os << " Packet_Type=" << r.Packet_Type << ", Payload_Length=" << r.Payload_Length;
				return os;
			}

			struct OutgoingPacket {
				std::shared_ptr<Network::Packet> Pack;
				unsigned int UncompressedLength;
			};
			struct PacketHeader {
				unsigned int Packet_Type;
				unsigned int Payload_Length;
				unsigned int UncompressedLength;
			};
		}
	}
}