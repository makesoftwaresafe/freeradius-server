/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file proto_load.c
 * @brief Load master protocol handler.
 *
 * @copyright 2017 Arran Cudbard-Bell (a.cudbardb@freeradius.org)
 * @copyright 2016 Alan DeKok (aland@freeradius.org)
 */
#include <freeradius-devel/io/application.h>
#include <freeradius-devel/io/listen.h>
#include <freeradius-devel/io/schedule.h>
#include <freeradius-devel/radius/radius.h>

#include "proto_load.h"

extern fr_app_t proto_load;
static int type_parse(TALLOC_CTX *ctx, void *out, UNUSED void *parent, CONF_ITEM *ci, conf_parser_t const *rule);
static int transport_parse(TALLOC_CTX *ctx, void *out, void *parent, CONF_ITEM *ci, conf_parser_t const *rule);

/** How to parse a Load listen section
 *
 */
static conf_parser_t const proto_load_config[] = {
	{ FR_CONF_OFFSET_TYPE_FLAGS("type", FR_TYPE_VOID, CONF_FLAG_NOT_EMPTY | CONF_FLAG_REQUIRED, proto_load_t,
			  type), .func = type_parse },
	{ FR_CONF_OFFSET_TYPE_FLAGS("transport", FR_TYPE_VOID, 0, proto_load_t, io.submodule),
	  .func = transport_parse, .dflt = "step" },

	/*
	 *	Add this as a synonym so normal humans can understand it.
	 */
	{ FR_CONF_OFFSET("max_entry_size", proto_load_t, max_packet_size) } ,

	/*
	 *	For performance tweaking.  NOT for normal humans.
	 */
	{ FR_CONF_OFFSET("max_packet_size", proto_load_t, max_packet_size) } ,
	{ FR_CONF_OFFSET("num_messages", proto_load_t, num_messages) } ,

	{ FR_CONF_OFFSET("priority", proto_load_t, priority) },

	CONF_PARSER_TERMINATOR
};

/** Translates the packet-type into a submodule name
 *
 * @param[in] ctx	to allocate data in (instance of proto_load).
 * @param[out] out	Where to write a module_instance_t containing the module handle and instance.
 * @param[in] parent	Base structure address.
 * @param[in] ci	#CONF_PAIR specifying the name of the type module.
 * @param[in] rule	unused.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
static int type_parse(UNUSED TALLOC_CTX *ctx, void *out, void *parent, CONF_ITEM *ci, UNUSED conf_parser_t const *rule)
{
	proto_load_t		*inst = talloc_get_type_abort(parent, proto_load_t);
	fr_dict_enum_value_t const	*type_enum;
	CONF_PAIR		*cp = cf_item_to_pair(ci);
	char const		*value = cf_pair_value(cp);

	*((char const **) out) = value;

	inst->dict = virtual_server_dict_by_child_ci(ci);
	if (!inst->dict) {
		cf_log_err(ci, "Please define 'namespace' in this virtual server");
		return -1;
	}

	inst->attr_packet_type = fr_dict_attr_by_name(NULL, fr_dict_root(inst->dict), "Packet-Type");
	if (!inst->attr_packet_type) {
		cf_log_err(ci, "Failed to find 'Packet-Type' attribute");
		return -1;
	}

	if (!value) {
		cf_log_err(ci, "No value given for 'type'");
		return -1;
	}

	type_enum = fr_dict_enum_by_name(inst->attr_packet_type, value, -1);
	if (!type_enum) {
		cf_log_err(ci, "Invalid type \"%s\"", value);
		return -1;
	}

	inst->code = type_enum->value->vb_uint32;
	return 0;
}

static int transport_parse(TALLOC_CTX *ctx, void *out, void *parent, CONF_ITEM *ci, conf_parser_t const *rule)
{
	proto_load_t		*inst = talloc_get_type_abort(parent, proto_load_t);
	module_instance_t	*mi;

	if (unlikely(virtual_server_listen_transport_parse(ctx, out, parent, ci, rule) < 0)) {
		return -1;
	}

	mi = talloc_get_type_abort(*(void **)out, module_instance_t);
	inst->io.app_io = (fr_app_io_t const *)mi->exported;
	inst->io.app_io_instance = mi->data;
	inst->io.app_io_conf = mi->conf;

	return 0;
}

/** Decode the packet, and set the request->process function
 *
 */
static int mod_decode(void const *instance, request_t *request, uint8_t *const data, size_t data_len)
{
	proto_load_t const	*inst = talloc_get_type_abort_const(instance, proto_load_t);

	request->packet->code = inst->code;

	/*
	 *	Set default addresses
	 */
	request->packet->socket.fd = -1;
	request->packet->socket.inet.src_ipaddr.af = AF_INET;
	request->packet->socket.inet.src_ipaddr.addr.v4.s_addr = htonl(INADDR_NONE);
	request->packet->socket.inet.dst_ipaddr = request->packet->socket.inet.src_ipaddr;

	request->reply->socket.inet.src_ipaddr = request->packet->socket.inet.src_ipaddr;
	request->reply->socket.inet.dst_ipaddr = request->packet->socket.inet.src_ipaddr;

	/*
	 *	The app_io is responsible for decoding all of the data.
	 */
	return inst->io.app_io->decode(inst->io.app_io_instance, request, data, data_len);
}

/*
 *	We don't need to encode any of the replies.  We just go "yeah, it's fine".
 */
static ssize_t mod_encode(UNUSED void const *instance, request_t *request, uint8_t *buffer, size_t buffer_len)
{
	if (buffer_len < 2) return -1;

	buffer[0] = request->reply->code;
	buffer[1] = 0x00;

	/*
	 *	For some reason the rest of the code treats
	 *	returning one byte of data as a magic
	 *	"do not respond" value... It's unclear why.
	 *
	 *	Return 2 to indicate we actually want our
	 *	write function to run.
	 */
	return 2;
}

/** Open listen sockets/connect to external event source
 *
 * @param[in] instance	Ctx data for this application.
 * @param[in] sc	to add our file descriptor to.
 * @param[in] conf	Listen section parsed to give us instance.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
static int mod_open(void *instance, fr_schedule_t *sc, UNUSED CONF_SECTION *conf)
{
	proto_load_t 	*inst = talloc_get_type_abort(instance, proto_load_t);

	inst->io.app = &proto_load;
	inst->io.app_instance = instance;

	/*
	 *	io.app_io should already be set
	 */
	return fr_master_io_listen(&inst->io, sc,
				   inst->max_packet_size, inst->num_messages);
}


/** Instantiate the application
 *
 * Instantiate I/O and type submodules.
 *
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
static int mod_instantiate(module_inst_ctx_t const *mctx)
{
	proto_load_t		*inst = talloc_get_type_abort(mctx->mi->data, proto_load_t);
	CONF_SECTION		*conf = mctx->mi->conf;

	/*
	 *	Ensure that the server CONF_SECTION is always set.
	 */
	inst->io.server_cs = cf_item_to_section(cf_parent(conf));

	/*
	 *	No IO module, it's an empty listener.
	 */
	if (!inst->io.submodule) {
		cf_log_err(conf, "The load generator MUST have a 'transport = ...' set");
		return -1;
	}

	/*
	 *	Tell the master handler about the main protocol instance.
	 */
	inst->io.app = &proto_load;
	inst->io.app_instance = inst;

	/*
	 *	The listener is inside of a virtual server.
	 */
	inst->server_cs = cf_item_to_section(cf_parent(conf));
	inst->cs = conf;
	inst->self = &proto_load;


	/*
	 *	We will need this for dynamic clients and connected sockets.
	 */
	inst->io.mi = mctx->mi;

	/*
	 *	These configuration items are not printed by default,
	 *	because normal people shouldn't be touching them.
	 */
	if (!inst->max_packet_size && inst->io.app_io) inst->max_packet_size = inst->io.app_io->default_message_size;

	if (!inst->num_messages) inst->num_messages = 256;

	FR_INTEGER_BOUND_CHECK("num_messages", inst->num_messages, >=, 32);
	FR_INTEGER_BOUND_CHECK("num_messages", inst->num_messages, <=, 65535);

	FR_INTEGER_BOUND_CHECK("max_packet_size", inst->max_packet_size, >=, 1024);
	FR_INTEGER_BOUND_CHECK("max_packet_size", inst->max_packet_size, <=, 65535);

	/*
	 *	Instantiate the transport module before calling the
	 *	common instantiation function.
	 */
	if (module_instantiate(inst->io.submodule) < 0) return -1;

	/*
	 *	Instantiate the master io submodule
	 */
	return fr_master_app_io.common.instantiate(MODULE_INST_CTX(inst->io.mi));
}

fr_app_t proto_load = {
	.common = {
		.magic			= MODULE_MAGIC_INIT,
		.name			= "load",
		.config			= proto_load_config,
		.inst_size		= sizeof(proto_load_t),

		.instantiate		= mod_instantiate
	},
	.open			= mod_open,
	.decode			= mod_decode,
	.encode			= mod_encode,
};
