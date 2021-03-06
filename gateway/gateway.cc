//
// async_tcp_echo_server.cpp
// ~~~~~~~~~~~~~~~~~~~~~~~~~
//
// Copyright (c) 2003-2019 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include <ctime>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <queue>
#include "asio.hpp"
#include "ThostFtdcTraderApi.h"
#include "ThostFtdcMdApi.h"
#include "ctppb/ctp.pb.h"
#include "gateway/helper.hpp"
#include "gateway/database.hpp"
#include "spdlog/spdlog.h"
#include "spdlog/sinks/basic_file_sink.h"

using asio::ip::tcp;

template <typename T>
class create_api_helper
{
	static T *create_api()
	{
	}
};

template <>
class create_api_helper<CThostFtdcTraderApi>
{
public:
	static CThostFtdcTraderApi *create()
	{
		return CThostFtdcTraderApi::CreateFtdcTraderApi();
	}
};
template <>
class create_api_helper<CThostFtdcMdApi>
{
public:
	static CThostFtdcMdApi *create()
	{
		return CThostFtdcMdApi::CreateFtdcMdApi();
	}
};

template <typename T>
class session
	: public std::enable_shared_from_this<session<T>>,
	  public handler<T>
{
public:
	session(tcp::socket socket, binding::database *db)
		: socket_(std::move(socket)), db_(db)
	{
		db_->incr_session_count();
		api_ = create_api_helper<T>::create();
		api_->RegisterSpi(this);
	}
	~session()
	{
		db_->decr_session_count();
		api_->RegisterSpi(NULL);
		api_->Release();
	}

	void start()
	{
		this->wptr_ = this->shared_from_this();
		do_read_header();
	}

private:
	void do_read_header()
	{
		auto self(this->shared_from_this());
		asio::async_read(socket_,
						 asio::buffer((char *)(&header_), sizeof(header_)),
						 [this, self](std::error_code ec, std::size_t /*length*/) {
							 if (!ec)
							 {
								 do_read_body();
							 }
							 else
							 {
								 spdlog::error("Read header error: {}", ec.message());
							 }
						 });
	}
	void do_read_body()
	{
		if (header_.body_length_ > sizeof(data_))
		{
			return;
		}
		auto self(this->shared_from_this());
		asio::async_read(socket_,
						 asio::buffer(data_, header_.body_length_),
						 [this, self](std::error_code ec, std::size_t /*length*/) {
							 if (!ec)
							 {
								 auto err = parse_body();
								 if (err == 0)
								 {
									 std::time_t result = std::time(nullptr);
									 spdlog::info("OnMessage: msg_type:{}, requestid:{}, body_length:{}, is_last:{}", ctp::CtpMessageType_Name(header_.msg_type_),
												  header_.request_id_, header_.body_length_, header_.is_last_);
									 switch (header_.msg_type_)
									 {
									 case ctp::HEARTBEAT:
									 {
										 send(ctp::HEARTBEAT, NULL, NULL, 0, true);
									 }
									 break;
									 default:
										 onMessage(api_, header_.msg_type_, header_.request_id_, p1_.data_, p1_.length_);
									 }
									 do_read_header();
								 }
								 else
								 {
									 spdlog::error("Parse body error {}", err);
								 }
							 }
							 else
							 {
								 spdlog::error("Read body completed err : {}", ec.message());
							 }
						 });
	}
	int parse_body()
	{
		if (header_.body_length_ == 0)
		{
			return 0;
		}
		if (header_.body_length_ < 4)
		{
			return -1;
		}
		p1_.length_ = *(int32_t *)&data_;
		if (p1_.length_ + 4 != header_.body_length_)
		{
			return -2;
		}
		p1_.data_ = &data_[4];
		return 0;
	}
	void send(ctp::CtpMessageType msgType, std::string *d1, std::string *d2, int nRequestID, bool bIsLasst)
	{
		auto spt = wptr_.lock();
		if (!spt)
		{
			spdlog::info("Session has expired, send has been canceled {}", msgType);
			return;
		}
		std::string *pkt = new (std::string);
		int body_size = 0;
		if (d1)
		{
			body_size += (d1->length() + 4);
		}
		if (d2)
		{
			body_size += (d2->length() + 4);
		}
		pkt->resize(body_size + 16);
		int32_t *offset = (int32_t *)(&pkt->data()[0]);
		*offset = msgType;
		offset++;
		*offset = nRequestID;
		offset++;
		*offset = bIsLasst;
		offset++;
		*offset = body_size;
		offset++;
		if (d1)
		{
			*offset = d1->length();
			offset++;
			memcpy(offset, d1->c_str(), d1->length());
			offset = (int32_t *)((char *)(offset) + d1->length());
		}
		if (d2)
		{
			*offset = d2->length();
			offset++;
			memcpy(offset, d2->c_str(), d2->length());
		}
		bool is_writting = false;
		{
			std::lock_guard<std::mutex> lk(out_q_lock_);
			out_q_.push(pkt);
			is_writting = is_writting_;
		}
		if (!is_writting)
		{
			do_write();
		}
	}

	void do_write()
	{
		auto self(this->shared_from_this());
		std::lock_guard<std::mutex> lk(out_q_lock_);
		if (out_q_.size() > 0)
		{
			std::string *pkt = out_q_.front();
			out_q_.pop();
			is_writting_ = true;
			asio::async_write(socket_, asio::buffer(*pkt), [this, self](std::error_code ec, std::size_t) {
				if (!ec)
				{
					do_write();
				}
			});
		}
		else
		{
			is_writting_ = false;
		}
	}

	struct packet_header
	{
		int32_t msg_type_;
		int32_t request_id_;
		int32_t is_last_;
		int32_t body_length_;
	};
	struct param
	{
		char *data_;
		int32_t length_;
	};

	tcp::socket socket_;
	packet_header header_;
	char data_[1024 * 1024];
	param p1_;
	std::queue<std::string *> out_q_;
	bool is_writting_;
	std::mutex out_q_lock_;

	T *api_;
	binding::database *db_;
	std::weak_ptr<session<T>> wptr_;
};

template <typename T>
class server
{
public:
	server(asio::io_context &io_context, short port)
		: acceptor_(io_context, tcp::endpoint(tcp::v4(), port))
	{
		do_accept();
	}

private:
	void do_accept()
	{
		acceptor_.async_accept(
			[this](std::error_code ec, tcp::socket socket) {
				if (!ec)
				{
					std::make_shared<session<T>>(std::move(socket), &db_)->start();
				}
				do_accept();
			});
	}

	tcp::acceptor acceptor_;
	binding::database db_;
};

int main(int argc, char *argv[])
{
	spdlog::info("Start!");
	try
	{
		if (argc != 3)
		{
			std::cerr << "Usage: gateway <port> <type>\n";
			return 1;
		}

		if (!strcmp(argv[2], "trade"))
		{
			asio::io_context io_context;
			server<CThostFtdcTraderApi> s(io_context, std::atoi(argv[1]));
			spdlog::info("Running at {} ", argv[1]);
			io_context.run();
		}
		else if (!strcmp(argv[2], "md"))
		{
			asio::io_context io_context;
			server<CThostFtdcMdApi> s(io_context, std::atoi(argv[1]));
			spdlog::info("Running at {} ", argv[1]);
			io_context.run();
		}
	}
	catch (std::exception &e)
	{
		spdlog::error("Exception: {}", e.what());
	}

	spdlog::info("Exit!");
	return 0;
}
