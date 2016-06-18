#include "stdafx.h"
#include "WebSocket.h"
#include "Packet.h"
#include "Compression/zstd.h"
#include "Logging.h"
#include "HttpHeader.h"
#include "crypto.h"
#include "Logging.h"
#include "Server_Config.h"
#include "Client_Config.h"
#include "ISocket.h"
#include "ICrypoLoader.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/deadline_timer.hpp>

#include <deque>
#include <unordered_map>
#include <string>
#include <memory>
#include <random>



namespace SL {
	namespace Remote_Access_Library {
		namespace Network {
			const std::string ws_magic_string = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
			class WSSAsio_Context {

			public:

				WSSAsio_Context() : work(io_service) {
					io_servicethread = std::thread([this]() {
						SL_RAT_LOG(Utilities::Logging_Levels::INFO_log_level, "Starting io_service Factory");

						boost::system::error_code ec;
						this->io_service.run(ec);
						if (ec) {
							SL_RAT_LOG(Utilities::Logging_Levels::ERROR_log_level, ec.message());
						}

						SL_RAT_LOG(Utilities::Logging_Levels::INFO_log_level, "stopping io_service Factory");
					});
				}
				~WSSAsio_Context()
				{
					Stop();
				}
				void Stop() {
					io_service.stop();
					if (io_servicethread.joinable()) io_servicethread.join();
				}

				boost::asio::io_service io_service;
				std::thread io_servicethread;
				boost::asio::io_service::work work;


			};


			class WSSocketImpl : public ISocket, public std::enable_shared_from_this<WSSocketImpl> {
			private:

			public:

				WSSocketImpl(IBaseNetworkDriver* netdriver, boost::asio::io_service& io_service, std::shared_ptr<boost::asio::ssl::context> sslcontext) :
					_socket(io_service, *sslcontext),
					_IBaseNetworkDriver(netdriver),
					_read_deadline(io_service),
					_write_deadline(io_service)
				{
					_read_deadline.expires_at(boost::posix_time::pos_infin);
					_write_deadline.expires_at(boost::posix_time::pos_infin);
					memset(&_SocketStats, 0, sizeof(_SocketStats));
					_Closed = false;
					_readtimeout = _writetimeout = 5;
				}


				virtual ~WSSocketImpl() {
					SL_RAT_LOG(Utilities::Logging_Levels::INFO_log_level, "~WSSocketImpl");
					CancelTimers();
					close("~WSSocketImpl");
				}
				std::shared_ptr<boost::asio::ssl::context> _ssl_context;
				boost::asio::ssl::stream<boost::asio::ip::tcp::socket> _socket;

				IBaseNetworkDriver* _IBaseNetworkDriver;

				std::deque<OutgoingPacket> _OutgoingPackets;
				SocketStats _SocketStats;


				std::vector<char> _IncomingBuffer;
				boost::asio::deadline_timer _read_deadline;
				boost::asio::deadline_timer _write_deadline;
				PacketHeader _WritePacketHeader;
				PacketHeader _ReadPacketHeader;
				std::unordered_map<std::string, std::string> _Header;
				bool _Server = false;
				std::string _Host;
				std::string _Port;
				bool _Closed = true;
				bool _Writing = false;
				int _readtimeout = 5;
				int _writetimeout = 5;
				unsigned char _recv_fin_rsv_opcode = 0;
				unsigned char _readheaderbuffer[8];
#define MASKSIZE 4
				unsigned char _writeheaderbuffer[sizeof(char)/*type*/ + sizeof(char)/*extra*/ + sizeof(unsigned long long)/*largest size*/ + MASKSIZE/*mask*/];



				void CancelTimers()
				{
					_read_deadline.cancel();
					_write_deadline.cancel();
				}
				void UpdateReadStats()
				{
					_SocketStats.TotalPacketReceived += 1;
					_SocketStats.NetworkBytesReceived += _ReadPacketHeader.Payload_Length;
					_SocketStats.TotalBytesReceived += _ReadPacketHeader.UncompressedLength;
				}

				void UpdateWriteStats(Packet & packet, size_t beforesize)
				{
					_SocketStats.TotalPacketSent += 1;
					_SocketStats.TotalBytesSent += beforesize;
					_SocketStats.NetworkBytesSent += packet.Payload_Length;
				}
				Packet GetNextReadPacket()
				{
					//dont free the memory on this... Owned internally
					Packet p(_ReadPacketHeader.Packet_Type, static_cast<size_t>(_ReadPacketHeader.Payload_Length), std::move(_Header), _IncomingBuffer.data(), false);
					_Header.clear();//reset the header after move
					return p;
				}

				Packet compress(Packet& packet) {
					auto maxsize = ZSTD_compressBound(packet.Payload_Length);
					Packet p(packet.Packet_Type, static_cast<unsigned int>(maxsize), std::move(packet.Header));
					p.Payload_Length = static_cast<unsigned int>(ZSTD_compress(p.Payload, maxsize, packet.Payload, static_cast<unsigned int>(packet.Payload_Length), 3));
					UpdateWriteStats(p, packet.Payload_Length);
					return p;
				}
				Packet decompress(Packet& packet) {
					Packet p(packet.Packet_Type, _ReadPacketHeader.UncompressedLength, std::move(packet.Header));
					p.Payload_Length = static_cast<unsigned int>(ZSTD_decompress((p.Payload, _ReadPacketHeader.UncompressedLength, packet.Payload, packet.Payload_Length));
					if (ZSTD_isError(p.Payload_Length) > 0) {
						SL_RAT_LOG(Utilities::Logging_Levels::ERROR_log_level, ZSTD_getErrorName(p.Payload_Length));
						return Packet(static_cast<unsigned int>(PACKET_TYPES::INVALID));//empty packet
					}
					UpdateReadStats();
					return p;
				}

				virtual void send(Packet& pack) override {
					auto self(shared_from_this());
					auto beforesize = pack.Payload_Length;
					auto compack(std::make_shared<Packet>(std::move(compress(pack))));
					_socket.get_io_service().post([self, compack, beforesize]()
					{

						self->_OutgoingPackets.push_back({ compack, beforesize });
						self->writeWSheader();
					});
				}
				virtual void close(std::string reason) override {
					SL_RAT_LOG(Utilities::Logging_Levels::INFO_log_level, "Closing socket: " << reason);
					if (closed()) return;
					_Closed = true;
					CancelTimers();
					_IBaseNetworkDriver->OnClose(this);
					boost::system::error_code ec;
					_socket.lowest_layer().shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
					_socket.lowest_layer().close();
					if (ec) {
						SL_RAT_LOG(Utilities::Logging_Levels::ERROR_log_level, ec.message());
					}
				}
				virtual bool closed() override {
					return _Closed || !_socket.lowest_layer().is_open();
				}

				//Get the statstics for this socket
				virtual SocketStats get_SocketStats() const override {
					return _SocketStats;
				}

				//s in in seconds
				virtual void set_ReadTimeout(int s)  override {
					assert(s > 0);
					_readtimeout = s;

				}
				//s in in seconds
				virtual void set_WriteTimeout(int s) override {
					assert(s > 0);
					_writetimeout = s;

				}

				virtual std::string get_ipv4_address() const  override {
					return _socket.lowest_layer().remote_endpoint().address().to_string();
				}
				virtual unsigned short get_port() const  override {
					return _socket.lowest_layer().remote_endpoint().port();
				}

				void handshake() {
					if (_Server) receivehandshake();
					else sendHandshake();
				}

				void receivehandshake()
				{
					readexpire_from_now(_readtimeout);
					auto self(shared_from_this());
					std::shared_ptr<boost::asio::streambuf> read_buffer(new boost::asio::streambuf);

					boost::asio::async_read_until(_socket, *read_buffer, "\r\n\r\n", [self, read_buffer](const boost::system::error_code& ec, size_t bytes_transferred) {
						UNUSED(bytes_transferred);
						if (!ec) {
							SL_RAT_LOG(Utilities::Logging_Levels::INFO_log_level, "Read Handshake bytes " << bytes_transferred);

							std::istream stream(read_buffer.get());
							self->_Header = std::move(Parse("1.1", stream));

							if (self->_Header.count(HTTP_SECWEBSOCKETKEY) == 0) return self->close("handshake async_read_until Sec-WebSocket-Key not present");//close socket and get out malformed
							auto write_buffer(std::make_shared<boost::asio::streambuf>());
							std::ostream handshake(write_buffer.get());

							handshake << "HTTP/1.1 101 Web Socket Protocol Handshake" << HTTP_ENDLINE;
							handshake << "Upgrade: websocket" << HTTP_ENDLINE;
							handshake << "Connection: Upgrade" << HTTP_ENDLINE;

							handshake << HTTP_SECWEBSOCKETACCEPT << HTTP_KEYVALUEDELIM << Crypto::Base64::encode(Crypto::SHA1(self->_Header[HTTP_SECWEBSOCKETKEY] + ws_magic_string)) << HTTP_ENDLINE << HTTP_ENDLINE;
							boost::asio::async_write(self->_socket, *write_buffer, [self, write_buffer](const boost::system::error_code& ec, size_t bytes_transferred) {
								if (!ec) {
									SL_RAT_LOG(Utilities::Logging_Levels::INFO_log_level, "Sent Handshake bytes " << bytes_transferred);
									self->_IBaseNetworkDriver->OnConnect(self);
									self->readheader();
								}
								else {
									self->close(std::string("handshake async_write ") + ec.message());
								}
							});
						}
						else {
							self->close(std::string("handshake async_read_until ") + ec.message());
						}
					});
				}

				void sendHandshake()
				{
					writeexpire_from_now(_writetimeout);

					std::shared_ptr<boost::asio::streambuf> write_buffer(new boost::asio::streambuf);
					auto self(shared_from_this());
					std::ostream request(write_buffer.get());

					request << "GET /rdpenpoint/ HTTP/1.1" << HTTP_ENDLINE;
					request << "Host: " << _Host << HTTP_ENDLINE;
					request << "Upgrade: websocket" << HTTP_ENDLINE;
					request << "Connection: Upgrade" << HTTP_ENDLINE;

					//Make random 16-byte nonce
					std::string nonce;
					nonce.resize(16);
					std::uniform_int_distribution<unsigned short> dist(0, 255);
					std::random_device rd;
					for (int c = 0; c < 16; c++)
						nonce[c] = static_cast<unsigned char>(dist(rd));

					std::string nonce_base64 = Crypto::Base64::encode(nonce);
					request << HTTP_SECWEBSOCKETKEY << HTTP_KEYVALUEDELIM << nonce_base64 << HTTP_ENDLINE;
					request << "Sec-WebSocket-Version: 13" << HTTP_ENDLINE << HTTP_ENDLINE;
					std::string accept_sha1(Crypto::SHA1(nonce_base64 + ws_magic_string));

					boost::asio::async_write(_socket, *write_buffer, [write_buffer, accept_sha1, self](const boost::system::error_code& ec, size_t bytes_transferred) {
						if (!ec) {
							SL_RAT_LOG(Utilities::Logging_Levels::INFO_log_level, "Sent Handshake bytes " << bytes_transferred);
							std::shared_ptr<boost::asio::streambuf> read_buffer(new boost::asio::streambuf);
							boost::asio::async_read_until(self->_socket, *read_buffer, "\r\n\r\n", [read_buffer, accept_sha1, self](const boost::system::error_code& ec, size_t bytes_transferred) {
								if (!ec) {
									self->writeexpire_from_now(0);//make sure to reset the write timmer
									SL_RAT_LOG(Utilities::Logging_Levels::INFO_log_level, "Read Handshake bytes " << bytes_transferred);
									std::istream stream(read_buffer.get());
									self->_Header = std::move(Parse("1.1", stream));
									if (Crypto::Base64::decode(self->_Header[HTTP_SECWEBSOCKETACCEPT]) == accept_sha1) {
										self->_IBaseNetworkDriver->OnConnect(self);
										self->readheader();
									}
									else {
										self->close(std::string("WebSocket handshake failed ") + ec.message());
									}
								}
							});
						}
						else {
							self->close(std::string("Failed sending handshake ") + ec.message());
						}
					});
				}

				void readheader()
				{
					readexpire_from_now(0);
					auto self(shared_from_this());
					boost::asio::async_read(_socket, boost::asio::buffer(_readheaderbuffer, 2), [self](const boost::system::error_code& ec, size_t bytes_transferred) {
						if (!ec) {
							assert(bytes_transferred == 2);
							self->_ReadPacketHeader.Payload_Length = 0;
							self->_recv_fin_rsv_opcode = self->_readheaderbuffer[0];
							//Close connection if unmasked message from client (protocol error)
							if (self->_Server) {
								if (self->_readheaderbuffer[1] < 128) return self->send_close(1002, "Closing connection because mask was not received");
							}
							else {
								if (self->_readheaderbuffer[1] >= 128) return self->send_close(1002, "Closing connection because the message was masked");
							}
							auto readbytes = (self->_readheaderbuffer[1] & 127) == 126 ? 2 : ((self->_readheaderbuffer[1] & 127) == 127 ? 8 : 0);
							if (readbytes != 0) {
								boost::asio::async_read(self->_socket, boost::asio::buffer(self->_readheaderbuffer, readbytes), [self, readbytes](const boost::system::error_code& ec, size_t bytes_transferred) {
									UNUSED(bytes_transferred);
									if (!ec) {
										assert(static_cast<size_t>(readbytes) == bytes_transferred);
										for (int c = 0; c < readbytes; c++) {
											self->_ReadPacketHeader.Payload_Length += self->_readheaderbuffer[c] << (8 * (readbytes - 1 - c));
										}
										self->readbody();
									}
									else {
										return self->close(std::string("readheader async_read 2 ") + ec.message());
									}
								});
							}
							else {
								self->_ReadPacketHeader.Payload_Length = self->_readheaderbuffer[1] & 127;
								self->readbody();
							}
						}
						else {
							return self->close(std::string("readheader async_read 1 ") + ec.message());
						}
					});
				}
				void readbody()
				{
					readexpire_from_now(_readtimeout);
					auto self(shared_from_this());

					_ReadPacketHeader.Payload_Length += _Server ? MASKSIZE : 0;//if this is a server, it receives 4 extra bytes
					_IncomingBuffer.reserve(_ReadPacketHeader.Payload_Length);

					auto p(_IncomingBuffer.data());
					auto size(_ReadPacketHeader.Payload_Length);
					boost::asio::async_read(_socket, boost::asio::buffer(p, size), [self](const boost::system::error_code& ec, size_t bytes_transferred) {

						if (!ec) {
							assert(self->_ReadPacketHeader.Payload_Length == bytes_transferred);
							auto packet(self->GetNextReadPacket());
							//If connection close
							if ((self->_recv_fin_rsv_opcode & 0x0f) == 8) {
								int status = 0;
								if (bytes_transferred >= 2) {
									unsigned char byte1 = packet.Payload[0];
									unsigned char byte2 = packet.Payload[1];
									status = (byte1 << 8) + byte2;
								}
								std::string msg(packet.Payload + 2);
								self->send_close(status, msg);
							}
							else if ((self->_recv_fin_rsv_opcode & 0x0f) == 9) {//ping
								SL_RAT_LOG(Utilities::Logging_Levels::INFO_log_level, "Ping Received");
								auto writeheader(std::make_shared<std::vector<unsigned char>>());
								writeheader->resize(2);
								auto p(writeheader->data());
								*p++ = 0x1A;
								*p++ = 0;
								boost::asio::async_write(self->_socket, boost::asio::buffer(writeheader->data(), writeheader->size()), [self, writeheader](const boost::system::error_code& ec, std::size_t)
								{
									if (ec) self->close(std::string("ping send failed ") + ec.message());
									self->readheader();
								});
							}
							else {

								if (self->_Server) {
									assert(bytes_transferred > 4);

									//servers receive data masked, so it needs to be unmasked
									unsigned char mask[MASKSIZE];
									memcpy(mask, packet.Payload, sizeof(mask));
									auto startpack = packet.Payload;
									std::vector<char> test;
									test.resize(packet.Payload_Length - MASKSIZE);

									for (size_t c = 0; c < packet.Payload_Length - MASKSIZE; c++) {
										test[c] = startpack[c] = startpack[c + MASKSIZE] ^ mask[c % MASKSIZE];
									}
								}

								memcpy(&self->_ReadPacketHeader, packet.Payload, sizeof(self->_ReadPacketHeader));
								memmove(packet.Payload, packet.Payload + sizeof(self->_ReadPacketHeader), self->_ReadPacketHeader.Payload_Length);

								packet.Packet_Type = self->_ReadPacketHeader.Packet_Type;
								packet.Payload_Length = self->_ReadPacketHeader.Payload_Length;
								auto spac(std::make_shared<Packet>(std::move(self->decompress(packet))));

								self->_IBaseNetworkDriver->OnReceive(self, spac);
								self->readheader();
							}
						}
						else {
							self->close(std::string("readheader async_read ") + ec.message());
						}
					});
				}
				void writePacketheader()
				{
					writeexpire_from_now(_writetimeout);
					auto self(shared_from_this());
					boost::asio::async_write(_socket, boost::asio::buffer(&_WritePacketHeader, sizeof(_WritePacketHeader)), [self](const boost::system::error_code& ec, std::size_t byteswritten)
					{
						if (!ec && !self->closed())
						{
							assert(byteswritten == sizeof(self->_WritePacketHeader));
							self->writebody();
						}
						else self->close(std::string("writeheader async_write ") + ec.message());
					});

				}
				void writeWSheader()
				{
					if (_Writing) return;// already writing
					_Writing = true;
					writeexpire_from_now(_writetimeout);
					auto& pac = _OutgoingPackets.front();
					//setup the header for sending
					_WritePacketHeader.Packet_Type = pac.Pack->Packet_Type;
					_WritePacketHeader.Payload_Length = pac.Pack->Payload_Length;
					_WritePacketHeader.UncompressedLength = pac.UncompressedLength;

					auto p(_writeheaderbuffer);
					*p++ = 130;//binary payload type
					size_t length = pac.Pack->Payload_Length + sizeof(_WritePacketHeader);
					if (length >= 126) {
						if (length > 0xffff) {
							*p++ = 127 + (_Server ? 0 : 128);
							auto s = static_cast<unsigned long long>(length);
							for (int c = sizeof(s) - 1; c >= 0; c--) {
								*p++ = (s >> (8 * c)) % 256;
							}
						}
						else {
							*p++ = 126 + (_Server ? 0 : 128);
							auto s = static_cast<unsigned short int>(length);
							int c = sizeof(s) - 1;
							int counter = 0;
							for (; c >= 0; c--, counter++) {
								*p++ = (s >> (8 * c)) % 256;
							}

						}
					}
					else {
						*p++ = static_cast<unsigned char>(length + (_Server ? 0 : 128));
					}
					if (!_Server) {//if this is a client, mask the data
						std::uniform_int_distribution<unsigned short> dist(0, 255);
						std::random_device rd;
						unsigned char mask[MASKSIZE];
						for (size_t c = 0; c < sizeof(mask); c++) {
							mask[c] = *p++ = static_cast<unsigned char>(dist(rd));
						}
						size_t c = 0;
						auto pheader = reinterpret_cast<unsigned char*>(&_WritePacketHeader);
						for (size_t i = 0; i < sizeof(_WritePacketHeader); i++, c++) {
							*pheader++ ^= mask[c % sizeof(mask)];
						}
						pheader = reinterpret_cast<unsigned char*>(pac.Pack->Payload);
						for (size_t i = 0; i < pac.Pack->Payload_Length; i++, c++) {
							*pheader++ ^= mask[c % sizeof(mask)];
						}
					}

					auto self(shared_from_this());
					boost::asio::async_write(_socket, boost::asio::buffer(_writeheaderbuffer, p - _writeheaderbuffer), [self](const boost::system::error_code& ec, std::size_t byteswritten)
					{
						UNUSED(byteswritten);
						if (!ec && !self->closed())
						{
							self->writePacketheader();
						}
						else self->close(std::string("writeheader async_write ") + ec.message());
					});

				}
				void writebody()
				{
					writeexpire_from_now(_writetimeout);
					auto packet = _OutgoingPackets.front().Pack;
					_OutgoingPackets.pop_front();//remove the packet

					auto self(shared_from_this());
					boost::asio::async_write(_socket, boost::asio::buffer(packet->Payload, packet->Payload_Length), [self, packet](const boost::system::error_code& ec, std::size_t byteswritten)
					{
						if (!ec && !self->closed())
						{
							self->_Writing = false;// done writing
							assert(byteswritten == packet->Payload_Length);
							if (!self->_OutgoingPackets.empty()) {
								self->writeWSheader();
							}
							else {
								self->writeexpire_from_now(0);
							}
						}
						else self->close(std::string("writebody async_write ") + ec.message());
					});
				}
				void readexpire_from_now(int seconds)
				{
					if (seconds <= 0) _read_deadline.expires_at(boost::posix_time::pos_infin);
					else  _read_deadline.expires_from_now(boost::posix_time::seconds(seconds));

					if (seconds >= 0) {
						auto self(shared_from_this());

						_read_deadline.async_wait([self, seconds](const boost::system::error_code& ec) {
							if (ec != boost::asio::error::operation_aborted) {
								self->close("read timer expired. Time waited: ");
							}
						});
					}
				}
				void writeexpire_from_now(int seconds)
				{
					if (seconds <= 0) _write_deadline.expires_at(boost::posix_time::pos_infin);
					else  _write_deadline.expires_from_now(boost::posix_time::seconds(seconds));
					if (seconds >= 0) {
						auto self(shared_from_this());
						_write_deadline.async_wait([self, seconds](const boost::system::error_code& ec) {
							if (ec != boost::asio::error::operation_aborted) {
								//close("write timer expired. Time waited: " + std::to_string(seconds));
								self->close("write timer expired. Time waited: ");
							}
						});
					}
				}

				void send_close(int status_code, std::string reason)
				{
					auto writeheader(std::make_shared<std::vector<unsigned char>>());
					writeheader->resize(2 + reason.size());
					auto p(writeheader->data());

					*(unsigned short int*)p = htons(static_cast<unsigned short int>(status_code));
					memcpy(p + 2, reason.c_str(), reason.size());
					auto self(shared_from_this());

					boost::asio::async_write(_socket, boost::asio::buffer(writeheader->data(), writeheader->size()), [self, writeheader](const boost::system::error_code& ec, std::size_t)
					{
						if (ec) self->close(std::string("send_close ") + ec.message());
					});
				}
				bool verify_certificate(bool preverified, boost::asio::ssl::verify_context& ctx)
				{
					// The verify callback can be used to check whether the certificate that is
					// being presented is valid for the peer. For example, RFC 2818 describes
					// the steps involved in doing this for HTTPS. Consult the OpenSSL
					// documentation for more details. Note that the callback is called once
					// for each certificate in the certificate chain, starting from the root
					// certificate authority.

					// In this example we will simply print the certificate's subject name.
					char subject_name[256];
					X509* cert = X509_STORE_CTX_get_current_cert(ctx.native_handle());
					X509_NAME_oneline(X509_get_subject_name(cert), subject_name, 256);
					std::cout << "Verifying " << subject_name << "\n";

					return preverified;
				}

			};

		}
	}
}

void SL::Remote_Access_Library::Network::WebSocket::Connect(Client_Config* config, IBaseNetworkDriver* driver, const char * host)
{
	static std::unique_ptr<WSSAsio_Context> io_runner;
	if (!io_runner) io_runner = std::make_unique<WSSAsio_Context>();

	auto sslcontext = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tlsv11);
	boost::asio::const_buffer cert(config->Public_Certficate->get_buffer(), config->Public_Certficate->get_size());
	boost::system::error_code ec;
	sslcontext->add_certificate_authority(cert, ec);
	if (ec) {
		SL_RAT_LOG(Utilities::Logging_Levels::ERROR_log_level, "add_certificate_authority error " << ec.message());
		return;
	}
	ec.clear();

	sslcontext->set_default_verify_paths(ec);
	if (ec) {
		SL_RAT_LOG(Utilities::Logging_Levels::ERROR_log_level, "set_default_verify_paths error " << ec.message());
		return;
	}
	ec.clear();

	auto sock = std::make_shared<WSSocketImpl>(driver, io_runner->io_service, sslcontext);

	sock->_Host = host != nullptr ? host : "";
	sock->_Port = std::to_string(config->WebSocketTLSLPort);
	assert(sock->_Host.size() > 2);
	assert(sock->_Port.size() > 0);
	sock->_Server = false;

	boost::asio::ip::tcp::resolver resolver(io_runner->io_service);

	boost::asio::ip::tcp::resolver::query query(sock->_Host, sock->_Port);
	boost::system::error_code ercode;
	auto endpoint = resolver.resolve(query, ercode);
	if (ercode) {
		sock->close(std::string("async_connect ") + ercode.message());
	}
	else {


		sock->_socket.set_verify_mode(boost::asio::ssl::verify_peer);
		sock->_socket.set_verify_callback(std::bind(&WSSocketImpl::verify_certificate, sock.get(), std::placeholders::_1, std::placeholders::_2));
		boost::asio::async_connect(sock->_socket.lowest_layer(), endpoint, [sock](const boost::system::error_code& ec, boost::asio::ip::tcp::resolver::iterator)
		{
			if (!ec)
			{
				sock->_socket.async_handshake(boost::asio::ssl::stream_base::client, [sock](const boost::system::error_code& ec) {
					if (!ec)
					{
						if (!sock->closed()) {
							sock->handshake();
						}
						else sock->close(std::string("!sock->closed() "));
					}
					else sock->close(std::string("async_handshake ") + ec.message());
				});
			}
			else sock->close(std::string("async_connect ") + ec.message());
		});
	}
}




namespace SL {
	namespace Remote_Access_Library {
		namespace Network {
			namespace WebSocket {
				class ListinerImpl : public std::enable_shared_from_this<ListinerImpl> {
				public:

					boost::asio::ip::tcp::acceptor _acceptor;
					std::shared_ptr<Network::Server_Config> _config;
					std::shared_ptr<WSSAsio_Context> _WSSAsio_Context;
					std::shared_ptr<boost::asio::ssl::context> sslcontext;
					IBaseNetworkDriver* _IBaseNetworkDriver;

					ListinerImpl(IBaseNetworkDriver* netevent, std::shared_ptr<WSSAsio_Context> asiocontext, std::shared_ptr<Network::Server_Config> config) :
						_acceptor(asiocontext->io_service, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), config->WebSocketTLSLPort)),
						_config(config),
						_WSSAsio_Context(asiocontext),
						sslcontext(std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tlsv11)),
						_IBaseNetworkDriver(netevent)
					{
						sslcontext->set_options(
							boost::asio::ssl::context::default_workarounds
							| boost::asio::ssl::context::no_sslv2 | boost::asio::ssl::context::no_sslv3
							| boost::asio::ssl::context::single_dh_use);
						boost::system::error_code ec;
						sslcontext->set_password_callback(bind(&ListinerImpl::get_password, this), ec);
						if (ec) SL_RAT_LOG(Utilities::Logging_Levels::ERROR_log_level, "set_password_callback error " << ec.message());
						ec.clear();
						boost::asio::const_buffer dhparams(Crypto::dhparams.data(), Crypto::dhparams.size());

						sslcontext->use_tmp_dh(dhparams, ec);
						if (ec) SL_RAT_LOG(Utilities::Logging_Levels::ERROR_log_level, "use_tmp_dh error " << ec.message());
						ec.clear();

						boost::asio::const_buffer cert(config->Public_Certficate->get_buffer(), config->Public_Certficate->get_size());
						sslcontext->use_certificate_chain(cert, ec);
						if (ec) SL_RAT_LOG(Utilities::Logging_Levels::ERROR_log_level, "use_certificate_chain_file error " << ec.message());
						ec.clear();

						sslcontext->set_default_verify_paths(ec);
						if (ec) SL_RAT_LOG(Utilities::Logging_Levels::ERROR_log_level, "set_default_verify_paths error " << ec.message());
						ec.clear();

						boost::asio::const_buffer privkey(config->Private_Key->get_buffer(), config->Private_Key->get_size());
						sslcontext->use_private_key(privkey, boost::asio::ssl::context::pem, ec);
						if (ec) SL_RAT_LOG(Utilities::Logging_Levels::ERROR_log_level, "use_private_key_file error " << ec.message());
						ec.clear();
					}
					virtual ~ListinerImpl() {
						Stop();
					}
					std::string get_password() const
					{
						return _config->PasswordToPrivateKey;
					}


					void Start() {
						SL_RAT_LOG(Utilities::Logging_Levels::INFO_log_level, "Starting Accept");
						auto self(shared_from_this());
						auto sock = std::make_shared<WSSocketImpl>(_IBaseNetworkDriver, _WSSAsio_Context->io_service, sslcontext);
						sock->_Server = true;
						_acceptor.async_accept(sock->_socket.lowest_layer(), [self, sock](const boost::system::error_code& ec)
						{
							if (!ec)
							{
								sock->_socket.async_handshake(boost::asio::ssl::stream_base::server, [self, sock](const boost::system::error_code& ec) {
									if (!ec) {
										sock->handshake();
									}
									else {
										SL_RAT_LOG(Utilities::Logging_Levels::INFO_log_level, "async_handshake Error: " << ec.message());
									}
									self->Start();
								});
							}
							else {
								SL_RAT_LOG(Utilities::Logging_Levels::INFO_log_level, "Exiting asyncaccept " << ec.message());
							}
						});
					}
					void Stop() {
						_acceptor.close();
						_WSSAsio_Context->Stop();
					}
				};
			}
		}
	}
}


SL::Remote_Access_Library::Network::WebSocket::Listener::Listener(IBaseNetworkDriver* netevent, std::shared_ptr<Network::Server_Config> config)
{
	_ListinerImpl = std::make_shared<ListinerImpl>(netevent, std::make_shared<WSSAsio_Context>(), config);
	_ListinerImpl->Start();
}

SL::Remote_Access_Library::Network::WebSocket::Listener::~Listener()
{
	_ListinerImpl->Stop();
}
