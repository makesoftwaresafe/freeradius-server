/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or (at
 *   your option) any later version.
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
 * @file rlm_redis_ippool_tool.c
 * @brief IP population tool.
 *
 * @author Arran Cudbard-Bell
 *
 * @copyright 2015 Arran Cudbard-Bell (a.cudbardb@freeradius.org)
 * @copyright 2015 The FreeRADIUS server project
 */
RCSID("$Id$")
#include <freeradius-devel/server/cf_parse.h>
#include <freeradius-devel/util/debug.h>

#include "base.h"
#include "cluster.h"
#include "redis_ippool.h"

#define MAX_PIPELINED 100000

/** Pool management actions
 *
 */
typedef enum ippool_tool_action {
	IPPOOL_TOOL_NOOP = 0,			//!< Do nothing.
	IPPOOL_TOOL_ADD,			//!< Add one or more IP addresses.
	IPPOOL_TOOL_REMOVE,			//!< Remove one or more IP addresses.
	IPPOOL_TOOL_RELEASE,			//!< Release one or more IP addresses.
	IPPOOL_TOOL_SHOW,			//!< Show one or more IP addresses.
	IPPOOL_TOOL_MODIFY,			//!< Modify attributes of one or more IP addresses.
	IPPOOL_TOOL_ASSIGN,			//!< Assign a static IP address to a device.
	IPPOOL_TOOL_UNASSIGN			//!< Remove static IP address assignment.
} ippool_tool_action_t;

/** A single pool operation
 *
 */
typedef struct {
	char const		*name;		//!< Original range or CIDR string.

	uint8_t const		*pool;		//!< Pool identifier.
	size_t			pool_len;	//!< Length of the pool identifier.

	uint8_t const		*range;		//!< Range identifier.
	size_t			range_len;	//!< Length of the range identifier.

	fr_ipaddr_t		start;		//!< Start address.
	fr_ipaddr_t		end;		//!< End address.
	uint8_t			prefix;		//!< Prefix - The bits between the address mask, and the prefix
						//!< form the addresses to be modified in the pool.
	ippool_tool_action_t	action;		//!< What to do to the leases described by net/prefix.
} ippool_tool_operation_t;

typedef struct {
	fr_ipaddr_t		ipaddr;		//!< Prefix or address.
	time_t			next_event;	//!< Last state change.
	uint8_t const		*range;		//!< Range the lease belongs to.
	size_t			range_len;
	uint8_t	const		*device;	//!< Last device id.
	size_t			device_len;
	uint8_t const		*gateway;	//!< Last gateway id.
	size_t			gateway_len;
} ippool_tool_lease_t;

typedef struct {
	uint64_t		total;		//!< Addresses available.
	uint64_t		free;		//!< Addresses in use.
	uint64_t		expiring_1m;	//!< Addresses that expire in the next minute.
	uint64_t		expiring_30m;	//!< Addresses that expire in the next 30 minutes.
	uint64_t		expiring_1h;	//!< Addresses that expire in the next hour.
	uint64_t		expiring_1d;	//!< Addresses that expire in the next day.
	uint64_t		static_tot;	//!< Static assignments configured.
	uint64_t		static_free;	//!< Static leases that have not been requested.
	uint64_t		static_1m;	//!< Static leases that should renew in the next minute.
	uint64_t		static_30m;	//!< Static leases that should renew in the next 30 minutes.
	uint64_t		static_1h;	//!< Static leases that should renew in the next hour.
	uint64_t		static_1d;	//!< Static leases that should renew in the next day.
} ippool_tool_stats_t;

static conf_parser_t redis_config[] = {
	REDIS_COMMON_CONFIG,
	CONF_PARSER_TERMINATOR
};

typedef struct {
	fr_redis_conf_t		conf;		//!< Connection parameters for the Redis server.
	fr_redis_cluster_t	*cluster;
} redis_driver_conf_t;

typedef struct {
	void			*driver;
	CONF_SECTION		*cs;
} ippool_tool_t;

typedef struct {
	char const		*owner;
} ippool_tool_owner_t;

typedef int (*redis_ippool_queue_t)(redis_driver_conf_t *inst, fr_redis_conn_t *conn,
				    uint8_t const *key_prefix, size_t key_prefix_len,
				    uint8_t const *range, size_t range_len,
				    fr_ipaddr_t *ipaddr, uint8_t prefix, void *uctx);

typedef int (*redis_ippool_process_t)(void *out, fr_ipaddr_t const *ipaddr, redisReply const *reply);

#define IPPOOL_BUILD_IP_KEY_FROM_STR(_buff, _p, _key, _key_len, _ip_str) \
do { \
	ssize_t _slen; \
	*_p++ = '{'; \
	memcpy(_p, _key, _key_len); \
	_p += _key_len; \
	_slen = strlcpy((char *)_p, "}:"IPPOOL_ADDRESS_KEY":", sizeof(_buff) - (_p - _buff)); \
	if (is_truncated((size_t)_slen, sizeof(_buff) - (_p - _buff))) { \
		ERROR("IP key too long"); \
		return 0;\
	} \
	_p += (size_t)_slen;\
	_p += strlcpy((char *)_p, _ip_str, sizeof(_buff) - (_p - _buff)); \
} while (0)

#if 0
#define IPPOOL_BUILD_OWNER_KEY(_buff, _p, _key, _key_len, _owner) \
do { \
	ssize_t _slen; \
	*_p++ = '{'; \
	memcpy(_p, _key, _key_len); \
	_p += _key_len; \
	_slen = strlcpy((char *)_p, "}:"IPPOOL_OWNER_KEY":", sizeof(_buff) - (_p - _buff)); \
	if (is_truncated((size_t)_slen, sizeof(_buff) - (_p - _buff))) { \
		ERROR("Owner key too long"); \
		return 0;\
	} \
	_p += (size_t)_slen;\
	_p += strlcpy((char *)_p, _owner, sizeof(_buff) - (_p - _buff)); \
} while (0)
#endif

#define EOL "\n"

static char const *name;
/** Lua script for releasing a lease
 *
 * - KEYS[1] The pool name.
 * - ARGV[1] IP address to release.
 *
 * Removes the IP entry in the ZSET, then removes the address hash, and the device key
 * if one exists.
 *
 * Will do nothing if the lease is not found in the ZSET.
 *
 * Returns
 * - 0 if no ip addresses were removed.
 * - 1 if an ip address was removed.
 */
static char lua_release_cmd[] =
	"local found" EOL								/* 1 */
	"local ret" EOL									/* 2 */

	/*
	 *	Set expiry time to 0
	 */
	"ret = redis.call('ZADD', '{' .. KEYS[1] .. '}:"IPPOOL_POOL_KEY"', 'XX', 'CH', 0, ARGV[1])" EOL	/* 3 */
	"if ret == 0 then" EOL								/* 4 */
	"  return 0" EOL								/* 5 */
	"end" EOL									/* 6 */
	"found = redis.call('HGET', '{' .. KEYS[1] .. '}:"IPPOOL_ADDRESS_KEY":'"
			    " .. ARGV[1], 'device')" EOL				/* 7 */
	"if not found then" EOL								/* 8 */
	"  return ret"	EOL								/* 9 */
	"end" EOL									/* 10 */

	/*
	 *	Remove the association between the device and a lease
	 */
	"redis.call('DEL', '{' .. KEYS[1] .. '}:"IPPOOL_OWNER_KEY":' .. found)" EOL	/* 11 */
	"return 1";									/* 12 */

/** Lua script for assigning a static lease
 *
 * - KEYS[1] The pool name.
 * - ARGV[1] THE ip address to create a static assignment for.
 * - ARGV[2] The owner to assign the static lease to.
 * - ARGV[3] The range identifier.
 * - ARGV[4] Wall time (seconds since epoch)
 *
 * Checks whether the IP already has a static assignment, and
 * whether the owner is already associated with a different IP.
 *
 * If check pass, sets the static flag on the IP entry in the ZSET and
 * creates the association between the IP and the owner.
 *
 * Returns
 *  - 0 if no assignment is made.
 *  - 1 if the IP assignment is made.
 */
static char lua_assign_cmd[] =
	"local pool_key = '{' .. KEYS[1] .. '}:"IPPOOL_POOL_KEY"'" EOL			/* 1 */
	"local owner_key = '{' .. KEYS[1] .. '}:"IPPOOL_OWNER_KEY":' .. ARGV[2]" EOL	/* 2 */
	"local ip_key = '{' .. KEYS[1]..'}:"IPPOOL_ADDRESS_KEY":' .. ARGV[1]" EOL	/* 3 */

	/*
	 *	Check the address doesn't already have a static assignment.
	 */
	"local expires = tonumber(redis.call('ZSCORE', pool_key, ARGV[1]))" EOL		/* 4 */
	"if expires and expires >= " STRINGIFY(IPPOOL_STATIC_BIT) " then" EOL		/* 5 */
	"  return 0" EOL								/* 6 */
	"end" EOL									/* 7 */

	/*
	 *	Check current assignment for device.
	 */
	"local found = redis.call('GET', owner_key)" EOL				/* 8 */
	"if found and found ~= ARGV[1] then" EOL					/* 9 */
	"  return 0" EOL								/* 10 */
	"end" EOL									/* 11 */

	/*
	 *	If expires is in the future, check it is not
	 *	another owner.
	 */
	"if expires and expires > tonumber(ARGV[4]) then" EOL				/* 12 */
	"  found = redis.call('HGET', ip_key, 'device')"				/* 13 */
	"  if found and found ~= ARGV[2] then" EOL					/* 14 */
	"    return 0" EOL								/* 15 */
	"  end" EOL									/* 16 */
	"end" EOL									/* 17 */

	/*
	 *	All checks passed - set the assignment.
	 */
	"expires = (expires or 0) + " STRINGIFY(IPPOOL_STATIC_BIT) EOL			/* 18 */
	"redis.call('ZADD', pool_key, 'CH', expires, ARGV[1])" EOL			/* 19 */
	"redis.call('SET', owner_key, ARGV[1])" EOL					/* 20 */
	"redis.call('HSET', ip_key, 'device', ARGV[2], 'counter', 0)" EOL		/* 21 */
	"if ARGV[3] then" EOL								/* 22 */
	"  redis.call('HSET', ip_key, 'range', ARGV[3])" EOL				/* 23 */
	"end" EOL									/* 24 */
	"return 1";									/* 25 */

/** Lua script for un-assigning a static lease
 *
 * - KEYS[1] The pool name.
 * - ARGV[1] IP address to remove static lease from.
 * - ARGV[2] The owner the static lease should be removed from.
 * - ARGV[3] Wall time (seconds since epoch).
 *
 * Removes the static flag from the IP entry in the ZSET, then, depending on the remaining time
 * determined by the ZSCORE removes the address hash, and the device key.
 *
 * Will do nothing if the static assignment does not exist or the IP and device do not match.
 *
 * Returns
 * - 0 if no ip addresses were unassigned.
 * - 1 if an ip address was unassigned.
 */
static char lua_unassign_cmd[] =
	"local found" EOL								/* 1 */
	"local pool_key = '{' .. KEYS[1] .. '}:"IPPOOL_POOL_KEY"'" EOL			/* 2 */
	"local owner_key = '{' .. KEYS[1] .. '}:"IPPOOL_OWNER_KEY":' .. ARGV[2]" EOL	/* 3 */

	/*
	 *	Check that the device hash exists and points at the correct IP
	 */
	"found = redis.call('GET', owner_key)" EOL					/* 4 */
	"if not found or found ~= ARGV[1] then" EOL					/* 5 */
	"  return 0" EOL								/* 6 */
	"end"	 EOL									/* 7 */

	/*
	 *	Check the assignment is actually static
	 */
	"local expires = tonumber(redis.call('ZSCORE', pool_key, ARGV[1]))" EOL		/* 8 */
	"local static = expires >= " STRINGIFY(IPPOOL_STATIC_BIT) EOL			/* 9 */
	"if not static then" EOL							/* 10 */
	" return 0" EOL									/* 11 */
	"end" EOL									/* 12 */

	/*
	 *	Remove static bit from ZSCORE
	 */
	"expires = expires - " STRINGIFY(IPPOOL_STATIC_BIT) EOL				/* 13 */
	"redis.call('ZADD', pool_key, 'XX', expires, ARGV[1])" EOL			/* 14 */

	/*
	 *	If the lease still has time left, set an expiry on the device key.
	 *	otherwise delete it.
	 */
	"if expires > tonumber(ARGV[3]) then" EOL					/* 15 */
	"  redis.call('EXPIRE', owner_key, expires - tonumber(ARGV[3]))" EOL		/* 16 */
	"else" EOL									/* 17 */
	"  redis.call('DEL', owner_key)" EOL						/* 18 */
	"end" EOL									/* 19 */
	"return 1";									/* 20 */

/** Lua script for removing a lease
 *
 * - KEYS[1] The pool name.
 * - ARGV[1] IP address to remove.
 *
 * Removes the IP entry in the ZSET, then removes the address hash, and the device key
 * if one exists.
 *
 * Will work with partially removed IP addresses (where the ZSET entry is absent but other
 * elements weren't cleaned up).
 *
 * Returns
 * - 0 if no ip addresses were removed.
 * - 1 if an ip address was removed.
 */
static char lua_remove_cmd[] =
	"local found" EOL								/* 1 */
	"local ret" EOL									/* 2 */
	"local address_key" EOL								/* 3 */

	"ret = redis.call('ZREM', '{' .. KEYS[1] .. '}:"IPPOOL_POOL_KEY"', ARGV[1])" EOL	/* 4 */
	"address_key = '{' .. KEYS[1] .. '}:"IPPOOL_ADDRESS_KEY":' .. ARGV[1]" EOL	/* 5 */
	"found = redis.call('HGET', address_key, 'device')" EOL				/* 6 */
	"redis.call('DEL', address_key)" EOL						/* 7 */
	"if not found then" EOL								/* 8 */
	"  return ret"	EOL								/* 9 */
	"end" EOL									/* 10 */

	/*
	 *	Remove the association between the device and a lease
	 */
	"redis.call('DEL', '{' .. KEYS[1] .. '}:"IPPOOL_OWNER_KEY":' .. found)" EOL	/* 11 */
	"return 1" EOL;									/* 12 */

static NEVER_RETURNS void usage(int ret) {
	INFO("Usage: %s -adrsm range... [-p prefix_len]... [-x]... [-oShf] server[:port] [pool] [range id]", name);
	INFO("Pool management:");
	INFO("  -a range               Add address(es)/prefix(es) to the pool.");
	INFO("  -d range               Delete address(es)/prefix(es) in this range.");
	INFO("  -r range               Release address(es)/prefix(es) in this range.");
	INFO("  -s range               Show addresses/prefix in this range.");
	INFO("  -A address/prefix      Assign a static lease.");
	INFO("  -O owner               To use when assigning a static lease.");
	INFO("  -U address/prefix      Un-assign a static lease");
	INFO("  -p prefix_len          Length of prefix to allocate (defaults to 32/128)");
	INFO("                         This is used primarily for IPv6 where a prefix is");
	INFO("                         allocated to an intermediary router, which in turn");
	INFO("                         allocates sub-prefixes to the devices it serves.");
	INFO("                         This argument changes the prefix_len for the previous");
	INFO("                         instance of an -adrsm argument, only.");
	INFO("  -m range               Change the range id to the one specified for addresses");
	INFO("                         in this range.");
	INFO("  -l                     List available pools.");
//	INFO("  -L                     List available ranges in pool [NYI]");
//	INFO("  -i file                Import entries from ISC lease file [NYI]");
	INFO(" ");	/* -Werror=format-zero-length */
//	INFO("Pool status:");
//	INFO("  -I                     Output active entries in ISC lease file format [NYI]");
	INFO("  -S                     Print pool statistics");
	INFO(" ");	/* -Werror=format-zero-length */
	INFO("Configuration:");
	INFO("  -h                     Print this help message and exit");
	INFO("  -x                     Increase the verbosity level");
//	INFO("  -o attr=value          Set option, these are specific to the backends [NYI]");
	INFO("  -f file                Load connection options from a FreeRADIUS format config file");
	INFO("                         This file should contain a pool { ... } section and one or more");
	INFO("                         `server = <fqdn>` pairs`");
	INFO(" ");
	INFO("<range> is range \"127.0.0.1-127.0.0.254\" or CIDR network \"127.0.0.1/24\" or host \"127.0.0.1\"");
	INFO("CIDR host bits set start address, e.g. 127.0.0.200/24 -> 127.0.0.200-127.0.0.254");
	fr_exit_now(ret);
}

static uint32_t uint32_gen_mask(uint8_t bits)
{
	if (bits >= 32) return 0xffffffff;
	return (1 << bits) - 1;
}

/** Iterate over range of IP addresses
 *
 * Mutates the ipaddr passed in, adding one to the prefix bits on each call.
 *
 * @param[in,out] ipaddr to increment.
 * @param[in] end ipaddr to stop at.
 * @param[in] prefix Length of the prefix.
 * @return
 *	- true if the prefix bits are not high (continue).
 *	- false if the prefix bits are high (stop).
 */
static bool ipaddr_next(fr_ipaddr_t *ipaddr, fr_ipaddr_t const *end, uint8_t prefix)
{
	switch (ipaddr->af) {
	default:
	case AF_UNSPEC:
		fr_assert(0);
		return false;

	case AF_INET6:
	{
		uint128_t ip_curr, ip_end;

		if (!fr_cond_assert((prefix > 0) && (prefix <= 128))) return false;

		/* Don't be tempted to cast */
		memcpy(&ip_curr, ipaddr->addr.v6.s6_addr, sizeof(ip_curr));
		memcpy(&ip_end, end->addr.v6.s6_addr, sizeof(ip_curr));

		ip_curr = ntohlll(ip_curr);
		ip_end = ntohlll(ip_end);

		/* We're done */
		if (uint128_eq(ip_curr, ip_end)) return false;

		/* Increment the prefix */
		ip_curr = uint128_add(ip_curr, uint128_lshift(uint128_new(0, 1), (128 - prefix)));
		ip_curr = htonlll(ip_curr);
		memcpy(&ipaddr->addr.v6.s6_addr, &ip_curr, sizeof(ipaddr->addr.v6.s6_addr));
		return true;
	}

	case AF_INET:
	{
		uint32_t ip_curr, ip_end;

		if (!fr_cond_assert((prefix > 0) && (prefix <= 32))) return false;

		ip_curr = ntohl(ipaddr->addr.v4.s_addr);
		ip_end = ntohl(end->addr.v4.s_addr);

		/* We're done */
		if (ip_curr == ip_end) return false;

		/* Increment the prefix */
		ip_curr += 1 << (32 - prefix);
		ipaddr->addr.v4.s_addr = htonl(ip_curr);
		return true;
	}
	}
}

/** Add a net to the pool
 *
 * @return the number of new addresses added.
 */
static int driver_do_lease(void *out, void *instance, ippool_tool_operation_t const *op,
			   redis_ippool_queue_t enqueue, redis_ippool_process_t process, void *uctx)
{
	redis_driver_conf_t		*inst = talloc_get_type_abort(instance, redis_driver_conf_t);

	int				i;
	bool				more = true;
	fr_redis_conn_t			*conn;

	fr_redis_cluster_state_t	state;
	fr_redis_rcode_t		status;

	fr_ipaddr_t			ipaddr = op->start;
	fr_redis_rcode_t		s_ret = REDIS_RCODE_SUCCESS;
	redisReply			**replies = NULL;

	unsigned int			pipelined = 0;

	while (more) {
		fr_ipaddr_t	acked = ipaddr; 	/* Record our progress */
		size_t		reply_cnt = 0;

		for (s_ret = fr_redis_cluster_state_init(&state, &conn, inst->cluster, NULL,
							 op->pool, op->pool_len, false);
		     s_ret == REDIS_RCODE_TRY_AGAIN;
		     s_ret = fr_redis_cluster_state_next(&state, &conn, inst->cluster, NULL, status, &replies[0])) {
		     	more = true;	/* Reset to true, may have errored last loop */
			status = REDIS_RCODE_SUCCESS;

			/*
			 *	If we got a redirect, start back at the beginning of the block.
			 */
			ipaddr = acked;

			for (i = 0; (i < MAX_PIPELINED) && more; i++, more = ipaddr_next(&ipaddr, &op->end,
											 op->prefix)) {
				int enqueued;

				enqueued = enqueue(inst, conn, op->pool, op->pool_len,
						   op->range, op->range_len, &ipaddr, op->prefix, uctx);
				if (enqueued < 0) break;
				pipelined += enqueued;
			}

			if (!replies) replies = talloc_zero_array(inst, redisReply *, pipelined);
			if (!replies) return -1;

			reply_cnt = fr_redis_pipeline_result(&pipelined, &status, replies,
							     talloc_array_length(replies), conn);
			for (i = 0; (size_t)i < reply_cnt; i++) fr_redis_reply_print(L_DBG_LVL_3,
										     replies[i], NULL, i, status);
		}
		if (s_ret != REDIS_RCODE_SUCCESS) {
			fr_redis_pipeline_free(replies, reply_cnt);
			talloc_free(replies);
			return -1;
		}

		if (process) {
			fr_ipaddr_t to_process = acked;

			for (i = 0; (size_t)i < reply_cnt; i++) {
				int ret;

				ret = process(out, &to_process, replies[i]);
				if (ret < 0) continue;
				ipaddr_next(&to_process, &op->end, op->prefix);
			}
		}
		fr_redis_pipeline_free(replies, reply_cnt);
		TALLOC_FREE(replies);
	}

	return 0;
}

/** Enqueue commands to retrieve lease information
 *
 */
static int _driver_show_lease_process(void *out, fr_ipaddr_t const *ipaddr, redisReply const *reply)
{
	size_t existing;
	ippool_tool_lease_t ***modified = out;
	ippool_tool_lease_t *lease;

	if (!*modified) *modified = talloc_array(NULL, ippool_tool_lease_t *, 1);

	/*
	 *	The exec command is the only one that produces an array.
	 */
	if (reply->type != REDIS_REPLY_ARRAY) return -1;
	if (reply->elements < 4) return -1;

	if (reply->element[0]->type == REDIS_REPLY_NIL) return 0;	/* A nil result (IP didn't exist) */
	if (reply->element[0]->type != REDIS_REPLY_STRING) return -1;	/* Something bad */
	lease = talloc_zero(*modified, ippool_tool_lease_t);
	lease->ipaddr = *ipaddr;
	lease->next_event = (time_t)strtoull(reply->element[0]->str, NULL, 10);

	if (reply->element[1]->type == REDIS_REPLY_STRING) {
		lease->device = talloc_memdup(lease, reply->element[1]->str, reply->element[1]->len);
		lease->device_len = reply->element[1]->len;
	}
	if (reply->element[2]->type == REDIS_REPLY_STRING) {
		lease->gateway = talloc_memdup(lease, reply->element[2]->str, reply->element[2]->len);
		lease->gateway_len = reply->element[2]->len;
	}
	if (reply->element[3]->type == REDIS_REPLY_STRING) {
		lease->range = talloc_memdup(lease, reply->element[3]->str, reply->element[3]->len);
		lease->range_len = reply->element[3]->len;
	}

	/*
	 *	Grow the result array...
	 */
	existing = talloc_array_length(*modified);
	MEM(*modified = talloc_realloc(NULL, *modified, ippool_tool_lease_t *, existing + 1));
	(*modified)[existing - 1] = lease;

	return 0;
}

/** Enqueue commands to retrieve lease information
 *
 */
static int _driver_show_lease_enqueue(UNUSED redis_driver_conf_t *inst, fr_redis_conn_t *conn,
				      uint8_t const *key_prefix, size_t key_prefix_len,
				      UNUSED uint8_t const *range, UNUSED size_t range_len,
				      fr_ipaddr_t *ipaddr, uint8_t prefix, UNUSED void *uctx)
{
	uint8_t		key[IPPOOL_MAX_POOL_KEY_SIZE];
	uint8_t		*key_p = key;
	char		ip_buff[FR_IPADDR_PREFIX_STRLEN];

	uint8_t		ip_key[IPPOOL_MAX_IP_KEY_SIZE];
	uint8_t		*ip_key_p = ip_key;

	IPPOOL_BUILD_KEY(key, key_p, key_prefix, key_prefix_len);
	IPPOOL_SPRINT_IP(ip_buff, ipaddr, prefix);
	IPPOOL_BUILD_IP_KEY_FROM_STR(ip_key, ip_key_p, key_prefix, key_prefix_len, ip_buff);

	DEBUG("Retrieving lease info for %s from pool %pV", ip_buff,
	      fr_box_strvalue_len((char const *)key_prefix, key_prefix_len));

	redisAppendCommand(conn->handle, "MULTI");
	redisAppendCommand(conn->handle, "ZSCORE %b %s", key, key_p - key, ip_buff);
	redisAppendCommand(conn->handle, "HGET %b device", ip_key, ip_key_p - ip_key);
	redisAppendCommand(conn->handle, "HGET %b gateway", ip_key, ip_key_p - ip_key);
	redisAppendCommand(conn->handle, "HGET %b range", ip_key, ip_key_p - ip_key);
	redisAppendCommand(conn->handle, "EXEC");
	return 6;
}

/** Show information about leases
 *
 */
static inline int driver_show_lease(void *out, void *instance, ippool_tool_operation_t const *op)
{
	return driver_do_lease(out, instance, op, _driver_show_lease_enqueue, _driver_show_lease_process, NULL);
}

/** Count the number of leases we released
 *
 */
static int _driver_release_lease_process(void *out, UNUSED fr_ipaddr_t const *ipaddr, redisReply const *reply)
{
	uint64_t *modified = out;
	/*
	 *	Record the actual number of addresses released.
	 *	Leases with a score of zero shouldn't be included,
	 *	in this count.
	 */
	if (reply->type != REDIS_REPLY_INTEGER) return -1;

	*modified += reply->integer;

	return 0;
}

/** Release a lease by setting its score back to zero
 *
 */
static int _driver_release_lease_enqueue(UNUSED redis_driver_conf_t *inst, fr_redis_conn_t *conn,
					 uint8_t const *key_prefix, size_t key_prefix_len,
					 UNUSED uint8_t const *range, UNUSED size_t range_len,
					 fr_ipaddr_t *ipaddr, uint8_t prefix, UNUSED void *uctx)
{
	char		ip_buff[FR_IPADDR_PREFIX_STRLEN];

	IPPOOL_SPRINT_IP(ip_buff, ipaddr, prefix);

	DEBUG("Releasing %pV to pool \"%pV\"", ip_buff,
	      fr_box_strvalue_len((char const *)key_prefix, key_prefix_len));
	redisAppendCommand(conn->handle, "EVAL %s 1 %b %s", lua_release_cmd, key_prefix, key_prefix_len, ip_buff);
	return 1;
}

/** Release a range of leases
 *
 */
static inline int driver_release_lease(void *out, void *instance, ippool_tool_operation_t const *op)
{
	return driver_do_lease(out, instance, op,
			       _driver_release_lease_enqueue, _driver_release_lease_process, NULL);
}

/** Count the number of leases we removed
 *
 * Because the ZREM and DEL have to occur in a transaction, we need
 * some fancier processing to just count the number of ZREMs.
 */
static int _driver_remove_lease_process(void *out, UNUSED fr_ipaddr_t const *ipaddr, redisReply const *reply)
{
	uint64_t *modified = out;
	/*
	 *	Record the actual number of addresses released.
	 *	Leases with a score of zero shouldn't be included,
	 *	in this count.
	 */
	if (reply->type != REDIS_REPLY_INTEGER) return -1;

	*modified += reply->integer;

	return 0;
}

/** Enqueue lease removal commands
 *
 * This removes the lease from the expiry heap, and the data associated with
 * the lease.
 */
static int _driver_remove_lease_enqueue(UNUSED redis_driver_conf_t *inst, fr_redis_conn_t *conn,
					uint8_t const *key_prefix, size_t key_prefix_len,
					UNUSED uint8_t const *range, UNUSED size_t range_len,
					fr_ipaddr_t *ipaddr, uint8_t prefix, UNUSED void *uctx)
{
	char		ip_buff[FR_IPADDR_PREFIX_STRLEN];

	IPPOOL_SPRINT_IP(ip_buff, ipaddr, prefix);

	DEBUG("Removing %s from pool \"%pV\"", ip_buff,
	      fr_box_strvalue_len((char const *)key_prefix, key_prefix_len));
	redisAppendCommand(conn->handle, "EVAL %s 1 %b %s", lua_remove_cmd, key_prefix, key_prefix_len, ip_buff);
	return 1;
}

/** Remove a range of leases
 *
 */
static int driver_remove_lease(void *out, void *instance, ippool_tool_operation_t const *op)
{
	return driver_do_lease(out, instance, op,
			       _driver_remove_lease_enqueue, _driver_remove_lease_process, NULL);
}

/** Count the number of leases we actually added
 *
 * This isn't necessarily the same as the number of ZADDs, as leases may
 * already exist.
 */
static int _driver_add_lease_process(void *out, UNUSED fr_ipaddr_t const *ipaddr, redisReply const *reply)
{
	uint64_t *modified = out;
	/*
	 *	Record the actual number of addresses modified.
	 *	Existing addresses won't be included in this
	 *	count.
	 */
	if (reply->type != REDIS_REPLY_ARRAY) return -1;

	if ((reply->elements > 0) && (reply->element[0]->type == REDIS_REPLY_INTEGER)) {
		*modified += reply->element[0]->integer;
	}
	return 0;
}

/** Enqueue lease addition commands
 *
 */
static int _driver_add_lease_enqueue(UNUSED redis_driver_conf_t *inst, fr_redis_conn_t *conn,
				     uint8_t const *key_prefix, size_t key_prefix_len,
				     uint8_t const *range, size_t range_len,
				     fr_ipaddr_t *ipaddr, uint8_t prefix, UNUSED void *uctx)
{
	uint8_t		key[IPPOOL_MAX_POOL_KEY_SIZE];
	uint8_t		*key_p = key;
	char		ip_buff[FR_IPADDR_PREFIX_STRLEN];

	uint8_t		ip_key[IPPOOL_MAX_IP_KEY_SIZE];
	uint8_t		*ip_key_p = ip_key;

	int		enqueued = 0;

	IPPOOL_BUILD_KEY(key, key_p, key_prefix, key_prefix_len);
	IPPOOL_SPRINT_IP(ip_buff, ipaddr, prefix);
	IPPOOL_BUILD_IP_KEY_FROM_STR(ip_key, ip_key_p, key_prefix, key_prefix_len, ip_buff);

	DEBUG("Adding %s to pool \"%pV\" (%zu)", ip_buff, fr_box_strvalue_len((char *)key, (key_p - key)), key_p - key);
	redisAppendCommand(conn->handle, "MULTI");
	enqueued++;
	redisAppendCommand(conn->handle, "ZADD %b NX %u %s", key, key_p - key, 0, ip_buff);
	enqueued++;

	/*
	 *	Only add range if it's not NULL.
	 *
	 *	Zero length ranges are allowed, and should be preserved.
	 */
	if (range) {
		redisAppendCommand(conn->handle, "HSET %b range %b", ip_key, ip_key_p - ip_key, range, range_len);
		enqueued++;
	}
	redisAppendCommand(conn->handle, "EXEC");
	enqueued++;

	return enqueued;
}

/** Add a range of prefixes
 *
 */
static int driver_add_lease(void *out, void *instance, ippool_tool_operation_t const *op)
{
	return driver_do_lease(out, instance, op, _driver_add_lease_enqueue, _driver_add_lease_process, NULL);
}

/** Count the number of leases we modified
 */
static int _driver_modify_lease_process(void *out, UNUSED fr_ipaddr_t const *ipaddr, redisReply const *reply)
{
	uint64_t *modified = out;
	/*
	 *	Record the actual number of addresses released.
	 *	Leases with a score of zero shouldn't be included,
	 *	in this count.
	 */
	if (reply->type != REDIS_REPLY_INTEGER) return -1;

	/*
	 *	return code is 0 or 1 depending on if its a new
	 *	field, neither are useful
	 */
	*modified += 1;

	return 0;
}

/** Enqueue lease removal commands
 *
 * This modifys the lease from the expiry heap, and the data associated with
 * the lease.
 */
static int _driver_modify_lease_enqueue(UNUSED redis_driver_conf_t *inst, fr_redis_conn_t *conn,
					uint8_t const *key_prefix, size_t key_prefix_len,
					uint8_t const *range, size_t range_len,
					fr_ipaddr_t *ipaddr, uint8_t prefix, UNUSED void *uctx)
{
	uint8_t		key[IPPOOL_MAX_POOL_KEY_SIZE];
	uint8_t		*key_p = key;
	char		ip_buff[FR_IPADDR_PREFIX_STRLEN];

	uint8_t		ip_key[IPPOOL_MAX_IP_KEY_SIZE];
	uint8_t		*ip_key_p = ip_key;

	IPPOOL_BUILD_KEY(key, key_p, key_prefix, key_prefix_len);
	IPPOOL_SPRINT_IP(ip_buff, ipaddr, prefix);
	IPPOOL_BUILD_IP_KEY_FROM_STR(ip_key, ip_key_p, key_prefix, key_prefix_len, ip_buff);

	DEBUG("Modifying %s in pool \"%pV\"", ip_buff, fr_box_strvalue_len((char const *)key_prefix, key_prefix_len));
	redisAppendCommand(conn->handle, "HSET %b range %b", ip_key, ip_key_p - ip_key, range, range_len);

	return 1;
}

/** Remove a range of leases
 *
 */
static int driver_modify_lease(void *out, void *instance, ippool_tool_operation_t const *op)
{
	return driver_do_lease(out, instance, op,
			       _driver_modify_lease_enqueue, _driver_modify_lease_process, NULL);
}

static int _driver_assign_lease_process(void *out, UNUSED fr_ipaddr_t const *ipaddr, redisReply const *reply)
{
	uint64_t *modified = out;
	if (reply->type != REDIS_REPLY_INTEGER) return -1;

	*modified += reply->integer;
	return 0;
}

/** Enqueue static lease assignment commands
 *
 */
static int _driver_assign_lease_enqueue(UNUSED redis_driver_conf_t *inst, fr_redis_conn_t *conn,
				     uint8_t const *key_prefix, size_t key_prefix_len,
				     uint8_t const *range, size_t range_len,
				     fr_ipaddr_t *ipaddr, uint8_t prefix, void *uctx)
{
	ippool_tool_owner_t	*owner = uctx;
	char		ip_buff[FR_IPADDR_PREFIX_STRLEN];
	fr_time_t	now;

	IPPOOL_SPRINT_IP(ip_buff, ipaddr, prefix);
	now = fr_time();

	DEBUG("Assigning address %s to owner %s", ip_buff, owner->owner);

	redisAppendCommand(conn->handle, "EVAL %s 1 %b %s %b %b %i", lua_assign_cmd, key_prefix, key_prefix_len,
			   ip_buff, owner->owner, strlen(owner->owner), range, range_len, fr_time_to_sec(now));
	return 1;
}

/** Add static lease assignments
 *
 */
static int driver_assign_lease(void *out, void *instance, ippool_tool_operation_t const *op, char const *owner)
{
	return driver_do_lease(out, instance, op,
			       _driver_assign_lease_enqueue, _driver_assign_lease_process,
			       &(ippool_tool_owner_t){ .owner = owner });
}

static int _driver_unassign_lease_process(void *out, UNUSED fr_ipaddr_t const *ipaddr, redisReply const *reply)
{
	uint64_t *modified = out;
	/*
	 *	Record the actual number of addresses unassigned.
	 */
	if (reply->type != REDIS_REPLY_INTEGER) return -1;

	*modified += reply->integer;

	return 0;
}

/** Enqueue static lease un-assignment commands
 *
 */
static int _driver_unassign_lease_enqueue(UNUSED redis_driver_conf_t *inst, fr_redis_conn_t *conn,
				     uint8_t const *key_prefix, size_t key_prefix_len,
				     UNUSED uint8_t const *range, UNUSED size_t range_len,
				     fr_ipaddr_t *ipaddr, uint8_t prefix, void *uctx)
{
	ippool_tool_owner_t	*owner = uctx;
	char			ip_buff[FR_IPADDR_PREFIX_STRLEN];
	fr_time_t		now;

	IPPOOL_SPRINT_IP(ip_buff, ipaddr, prefix);
	now = fr_time();

	DEBUG("Un-assigning address %s from owner %s", ip_buff, owner->owner);
	redisAppendCommand(conn->handle, "EVAL %s 1 %b %s %b %i", lua_unassign_cmd, key_prefix, key_prefix_len,
			   ip_buff, owner->owner, strlen(owner->owner), fr_time_to_sec(now));
	return 1;
}

/** Unassign static lease assignments
 *
 */
static int driver_unassign_lease(void *out, void *instance, ippool_tool_operation_t const *op, char const *owner)
{
	return driver_do_lease(out, instance, op,
			       _driver_unassign_lease_enqueue, _driver_unassign_lease_process,
			       &(ippool_tool_owner_t){ .owner = owner });
}

/** Compare two pool names
 *
 */
static int8_t pool_cmp(void const *a, void const *b)
{
	size_t len_a;
	size_t len_b;
	int ret;

	len_a = talloc_array_length((uint8_t const *)a);
	len_b = talloc_array_length((uint8_t const *)b);

	ret = CMP(len_a, len_b);
	if (ret != 0) return ret;

	ret = memcmp(a, b, len_a);
	return CMP(ret, 0);
}

/** Return the pools available across the cluster
 *
 * @param[in] ctx to allocate range names in.
 * @param[out] out Array of pool names.
 * @param[in] instance Driver specific instance data.
 * @return
 *	- < 0 on failure.
 *	- >= 0 the number of ranges in the array we allocated.
 */
static ssize_t driver_get_pools(TALLOC_CTX *ctx, uint8_t **out[], void *instance)
{
	fr_socket_t		*master;
	size_t			k;
	ssize_t			ret, i, used = 0;
	fr_redis_conn_t		*conn = NULL;
	redis_driver_conf_t	*inst = talloc_get_type_abort(instance, redis_driver_conf_t);
	uint8_t			key[IPPOOL_MAX_POOL_KEY_SIZE];
	uint8_t			*key_p = key;
	uint8_t 		**result;

	IPPOOL_BUILD_KEY(key, key_p, "*}:"IPPOOL_POOL_KEY, 1);

	*out = NULL;	/* Initialise output pointer */

	/*
	 *	Get the addresses of all masters in the pool
	 */
	ret = fr_redis_cluster_node_addr_by_role(ctx, &master, inst->cluster, true, false);
	if (ret <= 0) {
		result = NULL;
		return ret;
	}

	result = talloc_zero_array(ctx, uint8_t *, 1);
	if (!result) {
		ERROR("Failed allocating array of pool names");
		talloc_free(master);
		return -1;
	}

	/*
	 *	Iterate over the masters, getting the pools on each
	 */
	for (i = 0; i < ret; i++) {
		fr_pool_t	*pool;
		redisReply		*reply;
		char const		*p;
		size_t			len;
		char			cursor[19] = "0";
		fr_redis_rcode_t	status;

		if (fr_redis_cluster_pool_by_node_addr(&pool, inst->cluster, &master[i], false) < 0) {
			ERROR("Failed retrieving pool for node");
		error:
			TALLOC_FREE(result);
			talloc_free(master);
			return -1;
		}

		conn = fr_pool_connection_get(pool, NULL);
		if (!conn) goto error;
		do {
			/*
			 *	Break up the scan so we don't block any single
			 *	Redis node too long.
			 */
			reply = redisCommand(conn->handle, "SCAN %s MATCH %b COUNT 20", cursor, key, key_p - key);
			if (!reply) {
				ERROR("Failed reading reply");
				fr_pool_connection_release(pool, NULL, conn);
				goto error;
			}
			status = fr_redis_command_status(conn, reply);
			fr_redis_reply_print(L_DBG_LVL_3, reply, NULL, 0, status);
			if (status != REDIS_RCODE_SUCCESS) {
				PERROR("Error retrieving keys %s", cursor);

			reply_error:
				fr_pool_connection_release(pool, NULL, conn);
				fr_redis_reply_free(&reply);
				goto error;
			}

			if (reply->type != REDIS_REPLY_ARRAY) {
				ERROR("Failed retrieving result, expected array got %s",
				      fr_table_str_by_value(redis_reply_types, reply->type, "<UNKNOWN>"));

				goto reply_error;
			}

			if (reply->elements != 2) {
				ERROR("Failed retrieving result, expected array with two elements, got %zu elements",
				      reply->elements);
				fr_redis_reply_free(&reply);
				goto reply_error;
			}

			if (reply->element[0]->type != REDIS_REPLY_STRING) {
				ERROR("Failed retrieving result, expected string got %s",
				      fr_table_str_by_value(redis_reply_types, reply->element[0]->type, "<UNKNOWN>"));
				goto reply_error;
			}

			if (reply->element[1]->type != REDIS_REPLY_ARRAY) {
				ERROR("Failed retrieving result, expected array got %s",
				      fr_table_str_by_value(redis_reply_types, reply->element[1]->type, "<UNKNOWN>"));
				goto reply_error;
			}

			if ((talloc_array_length(result) - used) < reply->element[1]->elements) {
				MEM(result = talloc_realloc(ctx, result, uint8_t *,
							    used + reply->element[1]->elements));
				if (!result) {
					ERROR("Failed expanding array of pool names");
					goto reply_error;
				}
			}
			strlcpy(cursor, reply->element[0]->str, sizeof(cursor));

			for (k = 0; k < reply->element[1]->elements; k++) {
				redisReply *pool_key = reply->element[1]->element[k];

				/*
				 *	Skip over things which are not pool names
				 */
				if (pool_key->len < 7) continue; /* { + [<name>] + }:pool */

				if ((pool_key->str[0]) != '{') continue;
				p = memchr(pool_key->str + 1, '}', pool_key->len - 1);
				if (!p) continue;

				len = (pool_key->len - ((p + 1) - pool_key->str));
				if (len != (sizeof(IPPOOL_POOL_KEY) - 1) + 1) continue;
				if (memcmp(p + 1, ":" IPPOOL_POOL_KEY, (sizeof(IPPOOL_POOL_KEY) - 1) + 1) != 0) {
					continue;
				}

				/*
				 *	String between the curly braces is the pool name
				 */
				result[used++] = talloc_memdup(result, pool_key->str + 1, (p - pool_key->str) - 1);
			}

			fr_redis_reply_free(&reply);
		} while (!((cursor[0] == '0') && (cursor[1] == '\0')));	/* Cursor value of 0 means no more results */

		fr_pool_connection_release(pool, NULL, conn);
	}

	if (used == 0) {
		*out = NULL;
		talloc_free(result);
		return 0;
	}

	/*
	 *	Sort the results
	 */
	if (used > 1) {
		uint8_t const **to_sort;

		memcpy(&to_sort, &result, sizeof(to_sort));

		fr_quick_sort((void const **)to_sort, 0, used - 1, pool_cmp);
	}

	*out = talloc_array(ctx, uint8_t *, used);
	if (!*out) {
		ERROR("Failed allocating file pool name array");
		talloc_free(result);
		return -1;
	}

	/*
	 *	SCAN can produce duplicates, remove them here
	 */
	i = 0;
	k = 0;
	do {	/* stop before last entry */
		(*out)[k++] = talloc_steal(*out, result[i++]);
		while ((i < used) && (pool_cmp(result[i - 1], result[i]) == 0)) i++;
	} while (i < used);

	talloc_free(result);

	return used;
}

static int driver_get_stats(ippool_tool_stats_t *out, void *instance, uint8_t const *key_prefix, size_t key_prefix_len)
{
	redis_driver_conf_t		*inst = talloc_get_type_abort(instance, redis_driver_conf_t);
	uint8_t				key[IPPOOL_MAX_POOL_KEY_SIZE];
	uint8_t				*key_p = key;

	fr_redis_conn_t			*conn;

	fr_redis_cluster_state_t	state;
	fr_redis_rcode_t		status;
	fr_time_t			now;

	int				s_ret = REDIS_RCODE_SUCCESS;
	redisReply			**replies = NULL, *reply;
	unsigned int			pipelined = 0;		/* Update if additional commands added */

	size_t				reply_cnt = 0, i = 0;

#define STATS_COMMANDS_TOTAL 14

	IPPOOL_BUILD_KEY(key, key_p, key_prefix, key_prefix_len);

	MEM(replies = talloc_zero_array(inst, redisReply *, STATS_COMMANDS_TOTAL));

	now = fr_time();

	for (s_ret = fr_redis_cluster_state_init(&state, &conn, inst->cluster, NULL, key, key_p - key, false);
	     s_ret == REDIS_RCODE_TRY_AGAIN;
	     s_ret = fr_redis_cluster_state_next(&state, &conn, inst->cluster, NULL, status, &replies[0])) {
		status = REDIS_RCODE_SUCCESS;

		redisAppendCommand(conn->handle, "MULTI");
		redisAppendCommand(conn->handle, "ZCARD %b", key, key_p - key);		/* Total */
		redisAppendCommand(conn->handle, "ZCOUNT %b -inf %i",
				   key, key_p - key, fr_time_to_sec(now));		/* Free */
		redisAppendCommand(conn->handle, "ZCOUNT %b -inf %i",
				   key, key_p - key, fr_time_to_sec(now) + 60);		/* Free in next 60s */
		redisAppendCommand(conn->handle, "ZCOUNT %b -inf %i",
				   key, key_p - key, fr_time_to_sec(now) + (60 * 30));	/* Free in next 30 mins */
		redisAppendCommand(conn->handle, "ZCOUNT %b -inf %i",
				   key, key_p - key, fr_time_to_sec(now) + (60 * 60));	/* Free in next 60 mins */
		redisAppendCommand(conn->handle, "ZCOUNT %b -inf %i",
				   key, key_p - key, fr_time_to_sec(now) + (60 * 60 * 24));	/* Free in next day */
		redisAppendCommand(conn->handle, "ZCOUNT %b " STRINGIFY(IPPOOL_STATIC_BIT) " inf",
				   key, key_p - key);					/* Total static */
		redisAppendCommand(conn->handle, "ZCOUNT %b " STRINGIFY(IPPOOL_STATIC_BIT) " %"PRIu64,
				   key, key_p - key, IPPOOL_STATIC_BIT + fr_time_to_sec(now));	/* Static assignments 'free' */
		redisAppendCommand(conn->handle, "ZCOUNT %b " STRINGIFY(IPPOOL_STATIC_BIT) " %"PRIu64,
				   key, key_p - key,
				   IPPOOL_STATIC_BIT + fr_time_to_sec(now) + 60);	/* Static renew in 60s */
		redisAppendCommand(conn->handle, "ZCOUNT %b " STRINGIFY(IPPOOL_STATIC_BIT) " %"PRIu64,
				   key, key_p - key,
				   IPPOOL_STATIC_BIT + fr_time_to_sec(now) + (60 * 30));	/* Static renew in 30 mins */
		redisAppendCommand(conn->handle, "ZCOUNT %b " STRINGIFY(IPPOOL_STATIC_BIT) " %"PRIu64,
				   key, key_p - key,
				   IPPOOL_STATIC_BIT + fr_time_to_sec(now) + (60 * 60));	/* Static renew in 60 mins */
		redisAppendCommand(conn->handle, "ZCOUNT %b " STRINGIFY(IPPOOL_STATIC_BIT) " %"PRIu64,
				   key, key_p - key,
				   IPPOOL_STATIC_BIT + fr_time_to_sec(now) + (60 * 60 * 24));	/* Static renew in 1 day */
		redisAppendCommand(conn->handle, "EXEC");
		if (!replies) return -1;

		pipelined = STATS_COMMANDS_TOTAL;
		reply_cnt = fr_redis_pipeline_result(&pipelined, &status, replies,
						     talloc_array_length(replies), conn);
		for (i = 0; (size_t)i < reply_cnt; i++) fr_redis_reply_print(L_DBG_LVL_3,
									     replies[i], NULL, i, status);
	}
	if (s_ret != REDIS_RCODE_SUCCESS) {
	error:
		fr_redis_pipeline_free(replies, reply_cnt);
		talloc_free(replies);
		return -1;
	}

	if (reply_cnt != STATS_COMMANDS_TOTAL) {
		ERROR("Failed retrieving pool stats: Expected %i replies, got %zu", pipelined, reply_cnt);
		goto error;
	}

	reply = replies[reply_cnt - 1];

	if (reply->type != REDIS_REPLY_ARRAY) {
		ERROR("Failed retrieving pool stats: Expected array got %s",
		      fr_table_str_by_value(redis_reply_types, reply->element[1]->type, "<UNKNOWN>"));
		goto error;
	}

	if (reply->elements != (reply_cnt - 2)) {
		ERROR("Failed retrieving pool stats: Expected %zu results, got %zu", reply_cnt - 2, reply->elements);
		goto error;
	}

	if (reply->element[0]->integer == 0) {
		ERROR("Pool not found");
		goto error;
	}

	out->total = reply->element[0]->integer;
	out->free = reply->element[1]->integer;
	out->expiring_1m = reply->element[2]->integer - out->free;
	out->expiring_30m = reply->element[3]->integer - out->free;
	out->expiring_1h = reply->element[4]->integer - out->free;
	out->expiring_1d = reply->element[5]->integer - out->free;
	out->static_tot = reply->element[6]->integer;
	out->static_free = reply->element[7]->integer;
	out->static_1m = reply->element[8]->integer - out->static_free;
	out->static_30m = reply->element[9]->integer - out->static_free;
	out->static_1h = reply->element[10]->integer - out->static_free;
	out->static_1d = reply->element[11]->integer - out->static_free;

	fr_redis_pipeline_free(replies, reply_cnt);
	talloc_free(replies);

	return 0;
}

/** Driver initialization function
 *
 */
static int driver_init(TALLOC_CTX *ctx, CONF_SECTION *conf, void **instance)
{
	redis_driver_conf_t	*this;
	int			ret;

	*instance = NULL;

	if (cf_section_rules_push(conf, redis_config) < 0) return -1;

	this = talloc_zero(ctx, redis_driver_conf_t);
	if (!this) return -1;

	ret = cf_section_parse(this, &this->conf, conf);
	if (ret < 0) {
		talloc_free(this);
		return -1;
	}

	this->cluster = fr_redis_cluster_alloc(this, conf, &this->conf, false,
					       "rlm_redis_ippool_tool", NULL, NULL);
	if (!this->cluster) {
		talloc_free(this);
		return -1;
	}
	*instance = this;

	return 0;
}

/** Convert an IP range or CIDR mask to a start and stop address
 *
 * @param[out] start_out Where to write the start address.
 * @param[out] end_out Where to write the end address.
 * @param[in] ip_str Unparsed IP string.
 * @param[in] prefix length of prefixes we'll be allocating.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
static int parse_ip_range(fr_ipaddr_t *start_out, fr_ipaddr_t *end_out, char const *ip_str, uint8_t prefix)
{
	fr_ipaddr_t	start, end;
	bool		ex_broadcast;
	char const	*p;

	p = strchr(ip_str, '-');
	if (p) {
		char	start_buff[INET6_ADDRSTRLEN + 4];
		char	end_buff[INET6_ADDRSTRLEN + 4];
		size_t	len;

		if ((size_t)(p - ip_str) >= sizeof(start_buff)) {
			ERROR("Start address too long");
			return -1;
		}

		len = strlcpy(start_buff, ip_str, (p - ip_str) + 1);
		if (is_truncated(len, sizeof(start_buff))) {
			ERROR("Start address too long");
			return -1;
		}

		len = strlcpy(end_buff, p + 1, sizeof(end_buff));
		if (is_truncated(len, sizeof(end_buff))) {
			ERROR("End address too long");
			return -1;
		}

		if (fr_inet_pton(&start, start_buff, -1, AF_UNSPEC, false, true) < 0) {
			PERROR("Failed parsing \"%s\" as start address", start_buff);
			return -1;
		}

		if (fr_inet_pton(&end, end_buff, -1, AF_UNSPEC, false, true) < 0) {
			PERROR("Failed parsing \"%s\" end address", end_buff);
			return -1;
		}

		if (start.af != end.af) {
			ERROR("Start and end address must be of the same address family");
			return -1;
		}

		if (!prefix) prefix = IPADDR_LEN(start.af);

		/*
		 *	IPv6 addresses
		 */
		if (start.af == AF_INET6) {
			uint128_t start_int, end_int;

			memcpy(&start_int, start.addr.v6.s6_addr, sizeof(start_int));
			memcpy(&end_int, end.addr.v6.s6_addr, sizeof(end_int));
			if (uint128_gt(ntohlll(start_int), ntohlll(end_int))) {
				ERROR("End address must be greater than or equal to start address");
				return -1;
			}
		/*
		 *	IPv4 addresses
		 */
		} else {
			if (ntohl((uint32_t)(start.addr.v4.s_addr)) >
			    ntohl((uint32_t)(end.addr.v4.s_addr))) {
			 	ERROR("End address must be greater than or equal to start address");
			 	return -1;
			}
		}

		/*
		 *	Mask start and end so we can do prefix ranges too
		 */
		fr_ipaddr_mask(&start, prefix);
		fr_ipaddr_mask(&end, prefix);
		start.prefix = prefix;
		end.prefix = prefix;

		*start_out = start;
		*end_out = end;

		return 0;
	}

	if (fr_inet_pton(&start, ip_str, -1, AF_UNSPEC, false, false) < 0) {
		ERROR("Failed parsing \"%s\" as IPv4/v6 subnet", ip_str);
		return -1;
	}

	if (!prefix) prefix = IPADDR_LEN(start.af);

	if (prefix < start.prefix) {
		ERROR("-p must be greater than or equal to /<mask> (%u)", start.prefix);
		return -1;
	}
	if (prefix > IPADDR_LEN(start.af)) {
		ERROR("-p must be less than or equal to address length (%u)", IPADDR_LEN(start.af));
		return -1;
	}

	if ((prefix - start.prefix) > 64) {
		ERROR("-p must be less than or equal to %u", start.prefix + 64);
		return -1;
	}

	/*
	 *	Exclude the broadcast address only if we're dealing with IPv4 addresses
	 *	if we're allocating IPv6 addresses or prefixes we don't need to.
	 */
	ex_broadcast = (start.af == AF_INET) && (IPADDR_LEN(start.af) == prefix);

	/*
	 *	Excluding broadcast, 31/32 or 127/128 start/end are the same
	 */
	if (ex_broadcast && (start.prefix >= (IPADDR_LEN(start.af) - 1))) {
		*start_out = start;
		*end_out = start;
		return 0;
	}

	/*
	 *	Set various fields (we only overwrite the IP later)
	 */
	end = start;

	if (start.af == AF_INET6) {
		uint128_t ip, p_mask;

		/* cond assert to satisfy clang scan */
		if (!fr_cond_assert((prefix > 0) && (prefix <= 128))) return -1;

		/* Don't be tempted to cast */
		memcpy(&ip, start.addr.v6.s6_addr, sizeof(ip));
		ip = ntohlll(ip);

		/* Generate a mask that covers the prefix bits, and sets them high */
		p_mask = uint128_lshift(uint128_gen_mask(prefix - start.prefix), (128 - prefix));
		ip = htonlll(uint128_bor(p_mask, ip));

		/* Decrement by one */
		if (ex_broadcast) ip = uint128_sub(ip, uint128_new(0, 1));
		memcpy(&end.addr.v6.s6_addr, &ip, sizeof(end.addr.v6.s6_addr));
	} else {
		uint32_t ip;

		/* cond assert to satisfy clang scan */
		if (!fr_cond_assert((prefix > 0) && (prefix <= 32))) return -1;

		ip = ntohl(start.addr.v4.s_addr);

		/* Generate a mask that covers the prefix bits and sets them high */
		ip |= uint32_gen_mask(prefix - start.prefix) << (32 - prefix);

		/* Decrement by one */
		if (ex_broadcast) ip--;
		end.addr.v4.s_addr = htonl(ip);
	}

	*start_out = start;
	*end_out = end;

	return 0;
}

int main(int argc, char *argv[])
{
	static ippool_tool_operation_t	ops[128];
	ippool_tool_operation_t		*p = ops, *end = ops + (NUM_ELEMENTS(ops));

	int				c;

	uint8_t				*range_arg = NULL;
	uint8_t				*pool_arg = NULL;
	bool				do_export = false, print_stats = false, list_pools = false;
	bool				need_pool = false;
	char				*do_import = NULL;
	char const			*filename = NULL;
	char const			*owner = NULL;

	CONF_SECTION			*pool_cs;
	CONF_PAIR			*cp;
	ippool_tool_t			*conf;

	fr_debug_lvl = 0;
	name = argv[0];

	conf = talloc_zero(NULL, ippool_tool_t);
	conf->cs = cf_section_alloc(conf, NULL, "main", NULL);
	if (!conf->cs) fr_exit_now(EXIT_FAILURE);

#define ADD_ACTION(_action) \
do { \
	if (p >= end) { \
		ERROR("Too many actions, max is " STRINGIFY(sizeof(ops))); \
		usage(64); \
	} \
	p->action = _action; \
	p->name = optarg; \
	p++; \
	need_pool = true; \
} while (0)

	while ((c = getopt(argc, argv, "a:d:r:s:Sm:A:U:O:p:ilLhxo:f:")) != -1) switch (c) {
		case 'a':
			ADD_ACTION(IPPOOL_TOOL_ADD);
			break;

		case 'd':
			ADD_ACTION(IPPOOL_TOOL_REMOVE);
			break;

		case 'r':
			ADD_ACTION(IPPOOL_TOOL_RELEASE);
			break;

		case 's':
			ADD_ACTION(IPPOOL_TOOL_SHOW);
			break;

		case 'm':
			ADD_ACTION(IPPOOL_TOOL_MODIFY);
			break;

		case 'A':
			ADD_ACTION(IPPOOL_TOOL_ASSIGN);
			break;

		case 'U':
			ADD_ACTION(IPPOOL_TOOL_UNASSIGN);
			break;

		case 'O':
			owner = optarg;
			break;

		case 'p':
		{
			unsigned long tmp;
			char *q;

			if (p == ops) {
				ERROR("Prefix may only be specified after a pool management action");
				usage(64);
			}

			tmp = strtoul(optarg, &q, 10);
			if (q != (optarg + strlen(optarg))) {
				ERROR("Prefix must be an integer value");

			}

			(p - 1)->prefix = (uint8_t)tmp & 0xff;
		}
			break;

		case 'i':
			do_import = optarg;
			break;

		case 'I':
			do_export = true;
			break;

		case 'l':
			if (list_pools) usage(1);	/* Only allowed once */
			list_pools = true;
			break;

		case 'S':
			print_stats = true;
			break;

		case 'h':
			usage(0);

		case 'x':
			fr_debug_lvl++;
			break;

		case 'o':
			break;

		case 'f':
			filename = optarg;
			break;

		default:
			usage(1);
	}
	argc -= optind;
	argv += optind;

	if (argc == 0) {
		ERROR("Need server address/port");
		usage(64);
	}
	if ((argc == 1) && need_pool) {
		ERROR("Need pool to operate on");
		usage(64);
	}
	if (argc > 3) usage(64);

	/*
	 *	Read configuration files if necessary.
	 */
	if (filename && (cf_file_read(conf->cs, filename) < 0 || (cf_section_pass2(conf->cs) < 0))) {
		fr_exit_now(EXIT_FAILURE);
	}

	cp = cf_pair_alloc(conf->cs, "server", argv[0], T_OP_EQ, T_BARE_WORD, T_DOUBLE_QUOTED_STRING);
	if (!cp) {
		ERROR("Failed creating server pair");
		fr_exit_now(EXIT_FAILURE);
	}

	/*
	 *	Unescape sequences in the pool name
	 */
	if (argv[1] && (argv[1][0] != '\0')) {
		fr_sbuff_t		out;
		fr_sbuff_uctx_talloc_t	tctx;

		MEM(fr_sbuff_init_talloc(conf, &out, &tctx, strlen(argv[1]) + 1, SIZE_MAX));
		(void) fr_value_str_unescape(&out,
					     &FR_SBUFF_IN(argv[1], strlen(argv[1])), SIZE_MAX, '"');
		talloc_realloc(conf, out.buff, uint8_t, fr_sbuff_used(&out));
		pool_arg = (uint8_t *)out.buff;
	}

	if (argc >= 3 && (argv[2][0] != '\0')) {
		fr_sbuff_t		out;
		fr_sbuff_uctx_talloc_t	tctx;

		MEM(fr_sbuff_init_talloc(conf, &out, &tctx, strlen(argv[1]) + 1, SIZE_MAX));
		(void) fr_value_str_unescape(&out,
					     &FR_SBUFF_IN(argv[2], strlen(argv[2])), SIZE_MAX, '"');
		talloc_realloc(conf, out.buff, uint8_t, fr_sbuff_used(&out));
		range_arg = (uint8_t *)out.buff;
	}

	if (!do_import && !do_export && !list_pools && !print_stats && (p == ops)) {
		ERROR("Nothing to do!");
		fr_exit_now(EXIT_FAILURE);
	}

	/*
	 *	Set some alternative default pool settings
	 */
	pool_cs = cf_section_find(conf->cs, "pool", NULL);
	if (!pool_cs) {
		pool_cs = cf_section_alloc(conf->cs, conf->cs, "pool", NULL);
	}
	cp = cf_pair_find(pool_cs, "start");
	if (!cp) {
		/*
		 *	Start should always default to 1
		 *	else the cluster code doesn't
		 *	map the cluster.
		 */
		(void) cf_pair_alloc(pool_cs, "start", "1", T_OP_EQ, T_BARE_WORD, T_BARE_WORD);
	}
	cp = cf_pair_find(pool_cs, "spare");
	if (!cp) {
		(void) cf_pair_alloc(pool_cs, "spare", "0", T_OP_EQ, T_BARE_WORD, T_BARE_WORD);
	}
	cp = cf_pair_find(pool_cs, "min");
	if (!cp) {
		(void) cf_pair_alloc(pool_cs, "min", "0", T_OP_EQ, T_BARE_WORD, T_BARE_WORD);
	}
	cp = cf_pair_find(pool_cs, "max");
	if (!cp) {
		/*
		 *	Set a safe default for "max" - as this is a stand alone tool,
		 *	it can't use automatic value from the worker thread count.
		 */
		(void) cf_pair_alloc(pool_cs, "max", "10", T_OP_EQ, T_BARE_WORD, T_BARE_WORD);
	}

	if (driver_init(conf, conf->cs, &conf->driver) < 0) {
		ERROR("Driver initialisation failed");
		fr_exit_now(EXIT_FAILURE);
	}

	if (do_import) {
		ERROR("NOT YET IMPLEMENTED");
	}

	if (do_export) {
		ERROR("NOT YET IMPLEMENTED");
	}

	if (print_stats) {
		ippool_tool_stats_t	stats;
		uint8_t			**pools;
		ssize_t			slen;
		size_t			i;

		if (pool_arg) {
			pools = talloc_zero_array(conf, uint8_t *, 1);
			slen = 1;
			pools[0] = pool_arg;
		} else {
			slen = driver_get_pools(conf, &pools, conf->driver);
			if (slen < 0) fr_exit_now(EXIT_FAILURE);
		}

		for (i = 0; i < (size_t)slen; i++) {
			if (driver_get_stats(&stats, conf->driver,
					     pools[i], talloc_array_length(pools[i])) < 0) fr_exit_now(EXIT_FAILURE);

			INFO("pool                : %pV", fr_box_strvalue_len((char *)pools[i],
									   talloc_array_length(pools[i])));
			INFO("total               : %" PRIu64, stats.total);
			INFO("dynamic total       : %" PRIu64, stats.total - stats.static_tot);
			INFO("dynamic free        : %" PRIu64, stats.free);
			INFO("dynamic used        : %" PRIu64, stats.total - stats.free - stats.static_tot);
			if ((stats.total - stats.static_tot) > 0) {
				INFO("dynamic used (%%)    : %.2Lf",
				     ((long double)(stats.total - stats.free - stats.static_tot) /
				      (long double)(stats.total - stats.static_tot)) * 100);
			} else {
				INFO("used (%%)            : 0");
			}
			INFO("expiring 0-1m       : %" PRIu64, stats.expiring_1m);
			INFO("expiring 1-30m      : %" PRIu64, stats.expiring_30m - stats.expiring_1m);
			INFO("expiring 30m-1h     : %" PRIu64, stats.expiring_1h - stats.expiring_30m);
			INFO("expiring 1h-1d      : %" PRIu64, stats.expiring_1d - stats.expiring_1h);
			INFO("static total        : %" PRIu64, stats.static_tot);
			INFO("static 'free'       : %" PRIu64, stats.static_free);
			INFO("static issued       : %" PRIu64, stats.static_tot - stats.static_free);
			if (stats.static_tot) {
				INFO("static issued (%%)   : %.2Lf",
				     ((long double)(stats.static_tot - stats.static_free) /
				      (long double)(stats.static_tot)) * 100);
			} else {
				INFO("static issued (%%)   : 0");
			}
			INFO("static renew 0-1m   : %" PRIu64, stats.static_1m);
			INFO("static renew 1-30m  : %" PRIu64, stats.static_30m - stats.static_1m);
			INFO("static renew 30m-1h : %" PRIu64, stats.static_1h - stats.static_30m);
			INFO("static renew 1h-1d  : %" PRIu64, stats.static_1d - stats.static_1h);
			INFO("--");
		}
	}

	if (list_pools) {
		ssize_t		slen;
		size_t		i;
		uint8_t 	**pools;

		slen = driver_get_pools(conf, &pools, conf->driver);
		if (slen < 0) fr_exit_now(EXIT_FAILURE);
		if (slen > 0) {
			for (i = 0; i < (size_t)slen; i++) {
				INFO("%pV", fr_box_strvalue_len((char *)pools[i], talloc_array_length(pools[i])));
			}
			INFO("--");
		}

		talloc_free(pools);
	}

	/*
	 *	Fixup the operations without specific pools or ranges
	 *	and parse the IP ranges.
	 */
	end = p;
	for (p = ops; p < end; p++) {
		if (parse_ip_range(&p->start, &p->end, p->name, p->prefix) < 0) usage(64);
		if (!p->prefix) p->prefix = IPADDR_LEN(p->start.af);

		if (!p->pool) {
			p->pool = pool_arg;
			p->pool_len = talloc_array_length(pool_arg);
		}
		if (!p->range && range_arg) {
			p->range = range_arg;
			p->range_len = talloc_array_length(range_arg);
		}
	}

	for (p = ops; (p < end) && (p->start.af != AF_UNSPEC); p++) switch (p->action) {
	case IPPOOL_TOOL_ADD:
	{
		uint64_t count = 0;

		if (driver_add_lease(&count, conf->driver, p) < 0) {
			fr_exit_now(EXIT_FAILURE);
		}
		INFO("Added %" PRIu64 " address(es)/prefix(es)", count);
	}
		break;

	case IPPOOL_TOOL_REMOVE:
	{
		uint64_t count = 0;

		if (driver_remove_lease(&count, conf->driver, p) < 0) {
			fr_exit_now(EXIT_FAILURE);
		}
		INFO("Removed %" PRIu64 " address(es)/prefix(es)", count);
	}
		continue;

	case IPPOOL_TOOL_RELEASE:
	{
		uint64_t count = 0;

		if (driver_release_lease(&count, conf->driver, p) < 0) {
			fr_exit_now(EXIT_FAILURE);
		}
		INFO("Released %" PRIu64 " address(es)/prefix(es)", count);
	}
		continue;

	case IPPOOL_TOOL_SHOW:
	{
		ippool_tool_lease_t **leases = NULL;
		size_t len, i;

		if (driver_show_lease(&leases, conf->driver, p) < 0) {
			fr_exit_now(EXIT_FAILURE);
		}
		if (!fr_cond_assert(leases)) continue;

		len = talloc_array_length(leases);
		INFO("Retrieved information for %zu address(es)/prefix(es)", len - 1);
		for (i = 0; i < (len - 1); i++) {
			char	ip_buff[FR_IPADDR_PREFIX_STRLEN];
			char	time_buff[30];
			struct	tm tm;
			struct	timeval now;
			char	*device = NULL;
			char	*gateway = NULL;
			char	*range = NULL;
			bool	is_active;

			leases[i] = talloc_get_type_abort(leases[i], ippool_tool_lease_t);

			now = fr_time_to_timeval(fr_time());
			is_active = now.tv_sec <= leases[i]->next_event;
			if (leases[i]->next_event) {
				strftime(time_buff, sizeof(time_buff), "%b %e %Y %H:%M:%S %Z",
					 localtime_r(&(leases[i]->next_event), &tm));
			} else {
				time_buff[0] = '\0';
			}
			IPPOOL_SPRINT_IP(ip_buff, &(leases[i]->ipaddr), leases[i]->ipaddr.prefix);

			if (leases[i]->range) {
				range = fr_asprint(leases, (char const *)leases[i]->range,
						   leases[i]->range_len, '\0');
			}

			INFO("--");
			if (range) INFO("range           : %s", range);
			INFO("address/prefix  : %s", ip_buff);
			INFO("active          : %s", is_active ? "yes" : "no");

			if (leases[i]->device) {
				device = fr_asprint(leases, (char const *)leases[i]->device,
						    leases[i]->device_len, '\0');
			}
			if (leases[i]->gateway) {
				gateway = fr_asprint(leases, (char const *)leases[i]->gateway,
						     leases[i]->gateway_len, '\0');
			}
			if (is_active) {
				if (*time_buff) INFO("lease expires   : %s", time_buff);
				if (device) INFO("device id       : %s", device);
				if (gateway) INFO("gateway id      : %s", gateway);
			} else {
				if (*time_buff) INFO("lease expired   : %s", time_buff);
				if (device) INFO("last device id  : %s", device);
				if (gateway) INFO("last gateway id : %s", gateway);
			}
		}
		talloc_free(leases);
	}
		continue;

	case IPPOOL_TOOL_MODIFY:
	{
		uint64_t count = 0;

		if (driver_modify_lease(&count, conf->driver, p) < 0) {
			fr_exit_now(EXIT_FAILURE);
		}
		INFO("Modified %" PRIu64 " address(es)/prefix(es)", count);
	}
		continue;

	case IPPOOL_TOOL_ASSIGN:
	{
		uint64_t count = 0;

		if (fr_ipaddr_cmp(&p->start, &p->end) != 0) {
			ERROR("Static assignment requires a single IP");
			fr_exit_now(EXIT_FAILURE);
		}
		if (!owner) {
			ERROR("Static assignment requires an owner");
			fr_exit_now(EXIT_FAILURE);
		}

		if (driver_assign_lease(&count, conf->driver, p, owner) < 0) {
			fr_exit_now(EXIT_FAILURE);
		}
		INFO("Assigned %" PRIu64 " address(es)/prefix(es)", count);
	}
		continue;

	case IPPOOL_TOOL_UNASSIGN:
	{
		uint64_t count = 0;

		if (fr_ipaddr_cmp(&p->start, &p->end) != 0) {
			ERROR("Static lease un-assignment requires a single IP");
			fr_exit_now(EXIT_FAILURE);
		}
		if (!owner) {
			ERROR("Static lease un-assignment requires an owner");
			fr_exit_now(EXIT_FAILURE);
		}

		if (driver_unassign_lease(&count, conf->driver, p, owner) < 0) {
			fr_exit_now(EXIT_FAILURE);
		}
		INFO("Un-assigned %" PRIu64 " address(es)/prefix(es)", count);
	}
		continue;

	case IPPOOL_TOOL_NOOP:
		break;
	}

	talloc_free(conf);

	return 0;
}
