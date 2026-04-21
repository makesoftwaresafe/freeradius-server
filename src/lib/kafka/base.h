#pragma once
/*
 *   This program is free software; you can kafkatribute it and/or modify
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
 * @file lib/kafka/base.h
 * @brief Common functions for interacting with kafk
 *
 * @author Arran Cudbard-Bell
 *
 * @copyright 2022 Arran Cudbard-Bell (a.cudbardb@freeradius.org)
 */
RCSIDH(kafka_base_h, "$Id$")

#ifdef HAVE_WDOCUMENTATION
DIAG_OFF(documentation-deprecated-sync)
DIAG_OFF(documentation)
#endif
#include <librdkafka/rdkafka.h>
#ifdef HAVE_WDOCUMENTATION
DIAG_ON(documentation)
DIAG_ON(documentation-deprecated-sync)
#endif

#include <freeradius-devel/server/cf_parse.h>

#ifdef __cplusplus
extern "C" {
#endif

extern conf_parser_t const kafka_base_consumer_config[];
extern conf_parser_t const kafka_base_producer_config[];

typedef struct {
	rd_kafka_conf_t		*conf;
} fr_kafka_conf_t;

typedef struct {
	rd_kafka_topic_conf_t	*conf;
} fr_kafka_topic_conf_t;

/** Retrieve (or lazily allocate) the librdkafka conf cached on a CONF_SECTION.
 *
 * Populated as FR_CONF_FUNC callbacks under kafka_base_producer_config / _consumer_config
 * parse the surrounding section.
 */
fr_kafka_conf_t		*kafka_conf_from_cs(CONF_SECTION *cs);
fr_kafka_topic_conf_t	*kafka_topic_conf_from_cs(CONF_SECTION *cs);

#ifdef __cplusplus
}
#endif
