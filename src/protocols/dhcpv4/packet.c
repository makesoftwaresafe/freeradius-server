/*
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @file protocols/dhcpv4/packet.c
 * @brief Functions to encode/decode DHCP packets.
 *
 * @copyright 2008,2017 The FreeRADIUS server project
 * @copyright 2008 Alan DeKok (aland@deployingradius.com)
 */
#include <freeradius-devel/util/pair.h>
#include <freeradius-devel/util/rand.h>
#include <freeradius-devel/protocol/dhcpv4/rfc2131.h>

#include "dhcpv4.h"
#include "attrs.h"

/** Retrieve a DHCP option from a raw packet buffer
 *
 *
 */
uint8_t const *fr_dhcpv4_packet_get_option(dhcp_packet_t const *packet, size_t packet_size, fr_dict_attr_t const *da)
{
	int overload = 0;
	int field = DHCP_OPTION_FIELD;
	size_t where, size;
	uint8_t const *data;

	if (packet_size < MIN_PACKET_SIZE) return NULL;

	/*
	 *	This is needed for UBSAN on MacOS, that doesn't
	 *	allow misaligned accesses.  Because the packet
	 *	structure is flat, we don't need to deref the
	 *	packet pointer at any point, we just need to
	 *	calculate the offsets relative to the pointer
	 *	value and use those... Whatever actually deals
	 *	with the option is just expecting a uint8_t *.
	 */
#define ALIGNED_ACCESS(packet, field) \
		(uint8_t const *)packet + offsetof(dhcp_packet_t, field)

	where = 0;
	size = packet_size - offsetof(dhcp_packet_t, options);

	/*
	 *	Alignment fix.  We can't just deref a pointer
	 */
	data = ALIGNED_ACCESS(packet, options);
	while (where < size) {
		if (data[0] == 0) { /* padding */
			where++;
			data++;
			continue;
		}

		if (data[0] == 255) { /* end of options */
			if ((field == DHCP_OPTION_FIELD) && (overload & DHCP_FILE_FIELD)) {
				data = ALIGNED_ACCESS(packet, file);
				where = 0;
				size = sizeof(packet->file);
				field = DHCP_FILE_FIELD;
				continue;

			} else if ((field == DHCP_FILE_FIELD || field == DHCP_OPTION_FIELD) && (overload & DHCP_SNAME_FIELD)) {
				data = ALIGNED_ACCESS(packet, sname);
				where = 0;
				size = sizeof(packet->sname);
				field = DHCP_SNAME_FIELD;
				continue;
			}

			return NULL;
		}

		/*
		 *	We MUST have a real option here.
		 */
		if ((where + 2) > size) {
			fr_strerror_printf("Options overflow field at %u",
					   (unsigned int) (data - (uint8_t const *) packet));
			return NULL;
		}

		if ((where + 2 + data[1]) > size) {
			fr_strerror_printf("Option length overflows field at %u",
					   (unsigned int) (data - (uint8_t const *) packet));
			return NULL;
		}

		if (data[0] == da->attr) return data;

		if ((data[0] == 52) && (data[1] > 0)) { /* overload sname and/or file */
			overload = data[2];
		}

		where += data[1] + 2;
		data += data[1] + 2;
	}

	return NULL;
}

int fr_dhcpv4_decode(TALLOC_CTX *ctx, fr_pair_list_t *out, uint8_t const *data, size_t data_len, unsigned int *code)
{
	size_t		i;
	uint8_t const  	*p = data;
	uint32_t	giaddr;
	fr_pair_list_t	tmp;
	fr_pair_t	*vp;
	fr_pair_t	*maxms, *mtu, *netaddr;
	fr_value_box_t	box;
	fr_dhcpv4_ctx_t *packet_ctx;

	fr_pair_list_init(&tmp);

	if (data[1] > 1) {
		fr_strerror_printf("Packet is not Ethernet: %u",
		      data[1]);
		return -1;
	}

	packet_ctx = talloc_zero(ctx, fr_dhcpv4_ctx_t);
	if (!packet_ctx) return -1;
	packet_ctx->tmp_ctx = talloc(packet_ctx, uint8_t);

	/*
	 *	Decode the header.
	 */
	for (i = 0; i < dhcp_header_attrs_len; i++) {
		fr_dict_attr_t const *da = *dhcp_header_attrs[i];

		vp = fr_pair_afrom_da(ctx, da);
		if (!vp) {
			fr_strerror_const_push("Cannot decode packet due to internal error");
		error:
			talloc_free(vp);
			fr_pair_list_free(&tmp);
			talloc_free(packet_ctx);
			return -1;
		}

		switch (vp->vp_type) {
		case FR_TYPE_STRING:
			/*
			 *	According to RFC 2131, these are null terminated strings.
			 *	We don't trust everyone to abide by the RFC, though.
			 */
			if (*p != '\0') {
				uint8_t *q;

				q = memchr(p, '\0', dhcp_header_sizes[i]);
				fr_pair_value_bstrndup(vp, (char const *)p, q ? q - p : dhcp_header_sizes[i], true);
			} else {
				TALLOC_FREE(vp);
			}
			break;

			/*
			 *	The DHCP header size for CHADDR is not
			 *	6, so the value_box function doesn't
			 *	like it.  Just do the copy manually.
			 */
		case FR_TYPE_ETHERNET:
			if ((data[1] != 1) || (data[2] != 6)) {
				TALLOC_FREE(vp);
				break;
			}

			memcpy(vp->vp_ether, p, sizeof(vp->vp_ether));
			break;

		default:
			if (fr_value_box_from_network(vp, &vp->data, vp->vp_type, vp->da,
						      &FR_DBUFF_TMP(p, (size_t)dhcp_header_sizes[i]),
						      dhcp_header_sizes[i], true) < 0) goto error;
			break;
		}
		p += dhcp_header_sizes[i];

		if (!vp) continue;

		fr_pair_append(&tmp, vp);
	}

	/*
	 * 	Nothing uses tail after this call, if it does in the future
	 *	it'll need to find the new tail...
	 */
	{
		uint8_t const		*end;
		ssize_t			len;

		p = data + 240;
		end = p + (data_len - 240);

		/*
		 *	Loop over all the options data
		 */
		while (p < end) {
			len = fr_dhcpv4_decode_option(ctx, &tmp, p, (end - p), packet_ctx);
			if (len <= 0) {
			fail:
				fr_pair_list_free(&tmp);
				talloc_free(packet_ctx);
				return len;
			}
			p += len;
		}

		if (code) {
			vp = fr_pair_find_by_da(&tmp, NULL, attr_dhcp_message_type);
			if (vp) {
				*code = vp->vp_uint8;
			}
		}

		/*
		 *	If option Overload is present in the 'options' field, then fields 'file' and/or 'sname'
		 *	are used to hold more options. They are partitioned and must be interpreted in sequence.
		 */
		vp = fr_pair_find_by_da(&tmp, NULL, attr_dhcp_overload);
		if (vp) {
			if ((vp->vp_uint8 & 1) == 1) {
				/*
				 *	The 'file' field is used to hold options.
				 *	It must be interpreted before 'sname'.
				 */
				p = data + 44;
				end = p + 64;
				while (p < end) {
					len = fr_dhcpv4_decode_option(ctx, &tmp,
								      p, end - p, packet_ctx);
					if (len <= 0) goto fail;
					p += len;
				}
				fr_pair_delete_by_da(&tmp, attr_dhcp_boot_filename);
			}
			if ((vp->vp_uint8 & 2) == 2) {
				/*
				 *	The 'sname' field is used to hold options.
				 */
				p = data + 108;
				end = p + 128;
				while (p < end) {
					len = fr_dhcpv4_decode_option(ctx, &tmp,
								      p, end - p, packet_ctx);
					if (len <= 0) goto fail;
					p += len;
				}
				fr_pair_delete_by_da(&tmp, attr_dhcp_server_host_name);
			}
		}
	}

	/*
	 *	If DHCP request, set ciaddr to zero.
	 */

	/*
	 *	Set broadcast flag for broken vendors, but only if
	 *	giaddr isn't set.
	 */
	memcpy(&giaddr, data + 24, sizeof(giaddr));
	if (giaddr == htonl(INADDR_ANY)) {
		/*
		 *	DHCP Opcode is request
		 */
		vp = fr_pair_find_by_da(&tmp, NULL, attr_dhcp_opcode);
		if (vp && vp->vp_uint8 == 1) {
			/*
			 *	Vendor is "MSFT 98"
			 */
			vp = fr_pair_find_by_da(&tmp, NULL, attr_dhcp_vendor_class_identifier);
			if (vp && (vp->vp_length == 7) && (memcmp(vp->vp_strvalue, "MSFT 98", 7) == 0)) {
				vp = fr_pair_find_by_da(&tmp, NULL, attr_dhcp_flags);

				/*
				 *	Reply should be broadcast.
				 */
				if (vp) vp->vp_uint16 |= 0x8000;
			}
		}
	}

	/*
	 *	Determine the address to use in looking up which subnet the
	 *	client belongs to based on packet data.  The sequence here
	 *	is based on ISC DHCP behaviour and RFCs 3527 and 3011.  We
	 *	store the found address in an internal attribute of
	 *	Network-Subnet
	 *
	 *
	 *	All of these options / fields are type "ipv4addr", so
	 *	we need to decode them as that.  And then cast it to
	 *	"ipv4prefix".
	 */
	vp = fr_pair_afrom_da(ctx, attr_dhcp_network_subnet);
	if (!vp) return -1;

	/*
	 *	First look for Relay-Link-Selection
	 */
	netaddr = fr_pair_find_by_da_nested(&tmp, NULL, attr_dhcp_relay_link_selection);
	if (!netaddr) {
		/*
		 *	Next try Subnet-Selection-Option
		 */
		netaddr = fr_pair_find_by_da(&tmp, NULL, attr_dhcp_subnet_selection_option);
	}

	if (netaddr) {
		/*
		 *	Store whichever address we found from options and ensure
		 *	the data type matches the pair, i.e address to prefix
		 *	conversion.
		 */
		if (fr_value_box_cast(vp, &vp->data, vp->vp_type, vp->da, &netaddr->data) < 0) return -1;

	} else if (giaddr != htonl(INADDR_ANY)) {
		/*
		 *	Gateway address is set - use that one
		 */
		if (fr_value_box_from_network(vp, &box, FR_TYPE_IPV4_ADDR, NULL,
					  &FR_DBUFF_TMP(data + 24, 4), 4, true) < 0) return -1;
		if (fr_value_box_cast(vp, &vp->data, vp->vp_type, vp->da, &box) < 0) return -1;

	} else {
		/*
		 *	else, store client address whatever it is
		 */
		if (fr_value_box_from_network(vp, &box, FR_TYPE_IPV4_ADDR, NULL,
					  &FR_DBUFF_TMP(data + 12, 4), 4, true) < 0) return -1;
		if (fr_value_box_cast(vp, &vp->data, vp->vp_type, vp->da, &box) < 0) return -1;
	}

	fr_pair_append(&tmp, vp);

	/*
	 *	Client can request a LARGER size, but not a smaller
	 *	one.  They also cannot request a size larger than MTU.
	 */
	maxms = fr_pair_find_by_da(&tmp, NULL, attr_dhcp_dhcp_maximum_msg_size);
	mtu = fr_pair_find_by_da(&tmp, NULL, attr_dhcp_interface_mtu_size);

	if (mtu && (mtu->vp_uint16 < DEFAULT_PACKET_SIZE)) {
		fr_strerror_const("Client says MTU is smaller than minimum permitted by the specification");
		return -1;
	}

	/*
	 *	Client says maximum message size is smaller than minimum permitted
	 *	by the specification: fixing it.
	 */
	if (maxms && (maxms->vp_uint16 < DEFAULT_PACKET_SIZE)) maxms->vp_uint16 = DEFAULT_PACKET_SIZE;

	/*
	 *	Client says MTU is smaller than maximum message size: fixing it
	 */
	if (maxms && mtu && (maxms->vp_uint16 > mtu->vp_uint16)) maxms->vp_uint16 = mtu->vp_uint16;

	/*
	 *	FIXME: Nuke attributes that aren't used in the normal
	 *	header for discover/requests.
	 */
	fr_pair_list_append(out, &tmp);

	return 0;
}

int fr_dhcpv4_packet_encode(fr_packet_t *packet, fr_pair_list_t *list)
{
	ssize_t		len;
	fr_pair_t	*vp;

	if (packet->data) return 0;

	packet->data_len = MAX_PACKET_SIZE;
	packet->data = talloc_zero_array(packet, uint8_t, packet->data_len);

	/* XXX Ugly ... should be set by the caller */
	if (packet->code == 0) packet->code = FR_DHCP_NAK;

	/* store xid */
	if ((vp = fr_pair_find_by_da(list, NULL, attr_dhcp_transaction_id))) {
		packet->id = vp->vp_uint32;
	} else {
		packet->id = fr_rand();
	}

	len = fr_dhcpv4_encode(packet->data, packet->data_len, NULL, packet->code, packet->id, list);
	if (len < 0) return -1;

	packet->data_len = len;

	return 0;
}

fr_packet_t *fr_dhcpv4_packet_alloc(uint8_t const *data, ssize_t data_len)
{
	fr_packet_t *packet;
	uint32_t	magic;
	uint8_t const	*code;

	code = fr_dhcpv4_packet_get_option((dhcp_packet_t const *) data, data_len, attr_dhcp_message_type);
	if (!code) return NULL;

	if (data_len < MIN_PACKET_SIZE) return NULL;

	/* Now that checks are done, allocate packet */
	packet = fr_packet_alloc(NULL, false);
	if (!packet) {
		fr_strerror_const("Failed allocating packet");
		return NULL;
	}

	/*
	 *	Get XID.
	 */
	memcpy(&magic, data + 4, 4);

	packet->data_len = data_len;
	packet->code = code[2];
	packet->id = ntohl(magic);

	/*
	 *	FIXME: for DISCOVER / REQUEST: src_port == dst_port + 1
	 *	FIXME: for OFFER / ACK       : src_port = dst_port - 1
	 */

	/*
	 *	Unique keys are xid, client mac, and client ID?
	 */
	return packet;
}
