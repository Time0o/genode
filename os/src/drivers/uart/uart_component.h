/*
 * \brief  UART LOG component
 * \author Christian Helmuth
 * \date   2011-05-30
 */

/*
 * Copyright (C) 2011-2013 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU General Public License version 2.
 */

#ifndef _UART_COMPONENT_H_
#define _UART_COMPONENT_H_

/* Genode includes */
#include <base/rpc_server.h>
#include <util/arg_string.h>
#include <os/session_policy.h>
#include <os/attached_ram_dataspace.h>
#include <root/component.h>
#include <uart_session/uart_session.h>

/* local includes */
#include "uart_driver.h"

namespace Uart {

	using namespace Genode;

	class Session_component : public Rpc_object<Uart::Session,
	                                            Session_component>
	{
		private:

			/*
			 * XXX Do not use hard-coded value, better make it dependent
			 *     on the RAM quota donated by the client.
			 */
			enum { IO_BUFFER_SIZE = 4096 };

			Genode::Attached_ram_dataspace _io_buffer;

			/**
			 * Functor informing the client about new data to read
			 */
			struct Char_avail_callback : Uart::Char_avail_callback
			{
				Genode::Signal_context_capability sigh;

				void operator ()()
				{
					if (sigh.valid())
						Genode::Signal_transmitter(sigh).submit();
				}

			} _char_avail_callback;

			Uart::Driver_factory &_driver_factory;
			Uart::Driver         &_driver;

			Size _size;

			unsigned char _poll_char()
			{
				while (!_driver.char_avail());
				return _driver.get_char();
			}

			void _put_string(char const *s)
			{
				for (; *s; s++)
					_driver.put_char(*s);
			}

			/**
			 * Read ASCII number from UART
			 *
			 * \return character that terminates the sequence of digits
			 */
			unsigned char _read_number(unsigned &result)
			{
				result = 0;

				for (;;) {
					unsigned char c = _poll_char();

					if (!is_digit(c))
						return c;

					result = result*10 + digit(c);
				}
			}

			/**
			 * Try to detect the size of the terminal
			 */
			Size _detect_size()
			{
				/*
				 * Set cursor position to (hopefully) exceed the terminal
				 * dimensions.
				 */
				_put_string("\033[1;199r\033[199;255H");

				/* flush incoming characters */
				for (; _driver.char_avail(); _driver.get_char());

				/* request cursor coordinates */
				_put_string("\033[6n");

				unsigned width = 0, height = 0;

				if (_poll_char()         == 27
				 && _poll_char()         == '['
				 && _read_number(height) == ';'
				 && _read_number(width)  == 'R') {

					PINF("detected terminal size %dx%d", width, height);
					return Size(width, height);
				}

				return Size(0, 0);
			}

		public:

			/**
			 * Constructor
			 */
			Session_component(Uart::Driver_factory &driver_factory,
			                  unsigned index, unsigned baudrate, bool detect_size)
			:
				_io_buffer(Genode::env()->ram_session(), IO_BUFFER_SIZE),
				_driver_factory(driver_factory),
				_driver(*_driver_factory.create(index, baudrate, _char_avail_callback)),
				_size(detect_size ? _detect_size() : Size(0, 0))
			{ }


			/****************************
			 ** Uart session interface **
			 ****************************/

			void baud_rate(Genode::size_t bits_per_second)
			{
				_driver.baud_rate(bits_per_second);
			}


			/********************************
			 ** Terminal session interface **
			 ********************************/

			Size size() { return _size; }

			bool avail() { return _driver.char_avail(); }

			Genode::size_t _read(Genode::size_t dst_len)
			{
				char *io_buf      = _io_buffer.local_addr<char>();
				Genode::size_t sz = Genode::min(dst_len, _io_buffer.size());

				Genode::size_t n = 0;
				while ((n < sz) && _driver.char_avail())
					io_buf[n++] = _driver.get_char();

				return n;
			}

			void _write(Genode::size_t num_bytes)
			{
				/* constain argument to I/O buffer size */
				num_bytes = Genode::min(num_bytes, _io_buffer.size());

				char const *io_buf = _io_buffer.local_addr<char>();
				for (Genode::size_t i = 0; i < num_bytes; i++)
					_driver.put_char(io_buf[i]);
			}

			Genode::Dataspace_capability _dataspace()
			{
				return _io_buffer.cap();
			}

			void connected_sigh(Genode::Signal_context_capability sigh)
			{
				/*
				 * Immediately reflect connection-established signal to the
				 * client because the session is ready to use immediately after
				 * creation.
				 */
				Genode::Signal_transmitter(sigh).submit();
			}

			void read_avail_sigh(Genode::Signal_context_capability sigh)
			{
				_char_avail_callback.sigh = sigh;

				if (_driver.char_avail())
					_char_avail_callback();
			}

			Genode::size_t read(void *, Genode::size_t) { return 0; }
			Genode::size_t write(void const *, Genode::size_t) { return 0; }
	};


	typedef Root_component<Session_component, Multiple_clients> Root_component;


	class Root : public Root_component
	{
		private:

			Driver_factory &_driver_factory;

		protected:

			Session_component *_create_session(const char *args)
			{
				try {
					Session_label  label(args);
					Session_policy policy(label);

					unsigned index = 0;
					policy.attribute("uart").value(&index);

					unsigned baudrate = 0;
					try {
						policy.attribute("baudrate").value(&baudrate);
					} catch (Xml_node::Nonexistent_attribute) { }

					bool detect_size = false;
					try {
						detect_size = policy.attribute("detect_size").has_value("yes");
					} catch (Xml_node::Nonexistent_attribute) { }

					return new (md_alloc())
						Session_component(_driver_factory, index, baudrate, detect_size);

				} catch (Xml_node::Nonexistent_attribute) {
					PERR("Missing \"uart\" attribute in policy definition");
					throw Root::Unavailable();
				} catch (Session_policy::No_policy_defined) {
					PERR("Invalid session request, no matching policy");
					throw Root::Unavailable();
				}
			}

		public:

			/**
			 * Constructor
			 */
			Root(Rpc_entrypoint *ep, Allocator *md_alloc, Driver_factory &driver_factory)
			:
				Root_component(ep, md_alloc), _driver_factory(driver_factory)
			{ }
	};
}

#endif /* _UART_COMPONENT_H_ */
