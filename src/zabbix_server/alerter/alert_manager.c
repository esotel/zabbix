/*
** Zabbix
** Copyright (C) 2001-2017 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "common.h"

#include "daemon.h"
#include "zbxself.h"
#include "log.h"
#include "zbxipcservice.h"
#include "zbxalgo.h"
#include "zbxserver.h"
#include "alerter_protocol.h"
#include "alert_manager.h"

#define ZBX_AM_LOCATION_NOWHERE		0
#define ZBX_AM_LOCATION_QUEUE		1

#define ZBX_UPDATE_STR(dst, src)		if (NULL == dst || 0 != strcmp(dst, src)) dst = zbx_strdup(dst, src);

extern unsigned char	process_type, program_type;
extern int		server_num, process_num;

extern int	CONFIG_ALERTER_FORKS;
extern int	CONFIG_SENDER_FREQUENCY;
extern char	*CONFIG_ALERT_SCRIPTS_PATH;

/* alert data */
typedef struct
{
	zbx_uint64_t	alertid;
	zbx_uint64_t	mediatypeid;
	zbx_uint64_t	alertpoolid;
	int		nextsend;

	/* alert data */
	char		*sendto;
	char		*subject;
	char		*message;
	int		status;
	int		retries;
}
zbx_am_alert_t;

/* Alert pool data.                                                          */
/* Alerts are assigned to pools based on event source, object and objectid.  */
/* While alert pools can be processed in parallel, alerts inside alert pool  */
/* are processed sequentially.                                               */
typedef struct
{
	zbx_uint64_t		id;
	zbx_uint64_t		mediatypeid;

	/* alert queue */
	zbx_binary_heap_t	queue;

	int			location;
}
zbx_am_alertpool_t;

/* media type data */
typedef struct
{
	zbx_uint64_t		mediatypeid;

	int			location;
	int			alerts_num;

	/* alert pool queue */
	zbx_binary_heap_t	queue;

	/* media type data */
	int			type;
	char			*description;
	char			*smtp_server;
	char			*smtp_helo;
	char			*smtp_email;
	char			*exec_path;
	char			*gsm_modem;
	char			*username;
	char			*passwd;
	char			*exec_params;
	unsigned short		smtp_port;
	unsigned char		smtp_security;
	unsigned char		smtp_verify_peer;
	unsigned char		smtp_verify_host;
	unsigned char		smtp_authentication;

	int			maxsessions;
	int			maxattempts;
	int			attempt_interval;
}
zbx_am_mediatype_t;

/* alert status update data */
typedef struct
{
	zbx_uint64_t	alertid;
	int		retries;
	int		status;
	char		*error;

	zbx_uint64_t	flags;
}
zbx_am_alertstatus_t;

/* alerter data */
typedef struct
{
	/* the connected aleter client */
	zbx_ipc_client_t	*client;

	zbx_am_alert_t		*alert;
}
zbx_am_alerter_t;

/* alert manager data */
typedef struct
{
	/* alerter vector, created during manager initialization */
	zbx_vector_ptr_t	alerters;
	zbx_queue_ptr_t		free_alerters;

	/* alerters indexed by IPC service clients */
	zbx_hashset_t		alerters_client;

	/* the next alerter index to be assigned to new IPC service clients */
	int			next_alerter_index;

	zbx_hashset_t		mediatypes;
	zbx_hashset_t		alertpools;
	zbx_hashset_t		alertupdates;

	/* mediatype queue */
	zbx_binary_heap_t	queue;
}
zbx_am_t;

/* alerters client index hashset support */

static zbx_hash_t	alerter_hash_func(const void *d)
{
	const zbx_am_alerter_t	*alerter = *(const zbx_am_alerter_t **)d;

	zbx_hash_t hash =  ZBX_DEFAULT_PTR_HASH_FUNC(&alerter->client);

	return hash;
}

static int	alerter_compare_func(const void *d1, const void *d2)
{
	const zbx_am_alerter_t	*p1 = *(const zbx_am_alerter_t **)d1;
	const zbx_am_alerter_t	*p2 = *(const zbx_am_alerter_t **)d2;

	ZBX_RETURN_IF_NOT_EQUAL(p1->client, p2->client);
	return 0;
}

/* alert pool hashset support */

static zbx_hash_t	am_alertpool_hash_func(const void *data)
{
	const zbx_am_alertpool_t	*pool = (const zbx_am_alertpool_t *)data;

	zbx_hash_t		hash;

	hash = ZBX_DEFAULT_UINT64_HASH_FUNC(&pool->id);
	hash = ZBX_DEFAULT_UINT64_HASH_ALGO(&pool->mediatypeid, sizeof(pool->mediatypeid), hash);

	return hash;
}

static int	am_alertpool_compare_func(const void *d1, const void *d2)
{
	const zbx_am_alertpool_t	*pool1 = (const zbx_am_alertpool_t *)d1;
	const zbx_am_alertpool_t	*pool2 = (const zbx_am_alertpool_t *)d2;

	ZBX_RETURN_IF_NOT_EQUAL(pool1->id, pool2->id);
	ZBX_RETURN_IF_NOT_EQUAL(pool1->mediatypeid, pool2->mediatypeid);

	return 0;
}

/* queue support */

static int	am_alert_compare(const zbx_am_alert_t *alert1, const zbx_am_alert_t *alert2)
{
	ZBX_RETURN_IF_NOT_EQUAL(alert1->nextsend, alert2->nextsend);

	return 0;
}

static int	am_alert_queue_compare(const void *d1, const void *d2)
{
	const zbx_binary_heap_elem_t	*e1 = (const zbx_binary_heap_elem_t *)d1;
	const zbx_binary_heap_elem_t	*e2 = (const zbx_binary_heap_elem_t *)d2;

	return am_alert_compare((const zbx_am_alert_t *)e1->data, (const zbx_am_alert_t *)e2->data);
}

static int	am_alertpool_compare(const zbx_am_alertpool_t *pool1, const zbx_am_alertpool_t *pool2)
{
	zbx_binary_heap_elem_t	*e1, *e2;

	e1 = zbx_binary_heap_find_min((zbx_binary_heap_t *)&pool1->queue);
	e2 = zbx_binary_heap_find_min((zbx_binary_heap_t *)&pool2->queue);

	return am_alert_compare((const zbx_am_alert_t *)e1->data, (const zbx_am_alert_t *)e2->data);
}

static int	am_alertpool_queue_compare(const void *d1, const void *d2)
{
	const zbx_binary_heap_elem_t	*e1 = (const zbx_binary_heap_elem_t *)d1;
	const zbx_binary_heap_elem_t	*e2 = (const zbx_binary_heap_elem_t *)d2;

	return am_alertpool_compare((const zbx_am_alertpool_t *)e1->data, (const zbx_am_alertpool_t *)e2->data);
}

static int	am_mediatype_compare(const zbx_am_mediatype_t *media1, const zbx_am_mediatype_t *media2)
{
	zbx_binary_heap_elem_t	*e1, *e2;

	e1 = zbx_binary_heap_find_min((zbx_binary_heap_t *)&media1->queue);
	e2 = zbx_binary_heap_find_min((zbx_binary_heap_t *)&media2->queue);

	return am_alertpool_compare((const zbx_am_alertpool_t *)e1->data, (const zbx_am_alertpool_t *)e2->data);
}

static int	am_mediatype_queue_compare(const void *d1, const void *d2)
{
	const zbx_binary_heap_elem_t	*e1 = (const zbx_binary_heap_elem_t *)d1;
	const zbx_binary_heap_elem_t	*e2 = (const zbx_binary_heap_elem_t *)d2;

	return am_mediatype_compare((const zbx_am_mediatype_t *)e1->data, (const zbx_am_mediatype_t *)e2->data);
}

/******************************************************************************
 *                                                                            *
 * Function: am_get_mediatype                                                 *
 *                                                                            *
 * Purpose: gets media type object                                            *
 *                                                                            *
 * Parameters: manager     - [IN] the alert manager                           *
 *             mediatypeid - [IN] the media type identifier                   *
 *                                                                            *
 * Return value: The media type object or NULL if not found                   *
 *                                                                            *
 ******************************************************************************/
static zbx_am_mediatype_t	*am_get_mediatype(zbx_am_t *manager, zbx_uint64_t mediatypeid)
{
	return (zbx_am_mediatype_t *)zbx_hashset_search(&manager->mediatypes, &mediatypeid);
}

/******************************************************************************
 *                                                                            *
 * Function: am_update_mediatype                                              *
 *                                                                            *
 * Purpose: updates media type object, creating one if necessary              *
 *                                                                            *
 * Parameters: manager     - [IN] the alert manager                           *
 *             mediatypeid - [IN] the media type identifier                   *
 *                                                                            *
 ******************************************************************************/
static void	am_update_mediatype(zbx_am_t *manager, zbx_uint64_t mediatypeid, int type,
		const char *description, const char *smtp_server, const char *smtp_helo, const char *smtp_email,
		const char *exec_path, const char *gsm_modem, const char *username, const char *passwd,
		unsigned short smtp_port, unsigned char smtp_security, unsigned char smtp_verify_peer,
		unsigned char smtp_verify_host, unsigned char smtp_authentication, const char *exec_params,
		int maxsessions, int maxattempts, int attempt_interval)
{
	zbx_am_mediatype_t	*mediatype;

	if (NULL == (mediatype = am_get_mediatype(manager, mediatypeid)))
	{
		zbx_am_mediatype_t	mediatype_local = {mediatypeid, ZBX_AM_LOCATION_NOWHERE};

		mediatype = (zbx_am_mediatype_t *)zbx_hashset_insert(&manager->mediatypes, &mediatype_local,
				sizeof(mediatype_local));

		zbx_binary_heap_create(&mediatype->queue, am_alertpool_queue_compare,
				ZBX_BINARY_HEAP_OPTION_DIRECT);
	}

	mediatype->type = type;

	ZBX_UPDATE_STR(mediatype->description, description);
	ZBX_UPDATE_STR(mediatype->smtp_server, smtp_server);
	ZBX_UPDATE_STR(mediatype->smtp_helo, smtp_helo);
	ZBX_UPDATE_STR(mediatype->smtp_email, smtp_email);
	ZBX_UPDATE_STR(mediatype->exec_path, exec_path);
	ZBX_UPDATE_STR(mediatype->exec_params, exec_params);
	ZBX_UPDATE_STR(mediatype->gsm_modem, gsm_modem);
	ZBX_UPDATE_STR(mediatype->username, username);
	ZBX_UPDATE_STR(mediatype->passwd, passwd);

	mediatype->smtp_port = smtp_port;
	mediatype->smtp_security = smtp_security;
	mediatype->smtp_verify_peer = smtp_verify_peer;
	mediatype->smtp_verify_host = smtp_verify_host;
	mediatype->smtp_authentication = smtp_authentication;

	mediatype->maxsessions = maxsessions;
	mediatype->maxattempts = maxattempts;
	mediatype->attempt_interval = attempt_interval;
}

/******************************************************************************
 *                                                                            *
 * Function: am_push_meditype                                                 *
 *                                                                            *
 * Purpose: pushes media type into manager media type queue                   *
 *                                                                            *
 * Parameters: manager   - [IN] the alert manager                             *
 *             mediatype - [IN] the media type                                *
 *                                                                            *
 * Comments: The media tyep is inserted into queue only if it was not already *
 *           queued and if the number of media type alerts being processed    *
 *           not reached the limit.                                           *
 *           If media type is already queued only its location in the queue   *
 *           is updated.                                                      *
 *                                                                            *
 ******************************************************************************/
static void	am_push_meditype(zbx_am_t *manager, zbx_am_mediatype_t *mediatype)
{
	zbx_binary_heap_elem_t	elem = {mediatype->mediatypeid, mediatype};

	if (SUCCEED == zbx_binary_heap_empty(&mediatype->queue))
		return;

	if (ZBX_AM_LOCATION_NOWHERE == mediatype->location)
	{
		if (0 == mediatype->maxsessions || mediatype->alerts_num < mediatype->maxsessions)
		{
			zbx_binary_heap_insert(&manager->queue, &elem);
			mediatype->location = ZBX_AM_LOCATION_QUEUE;
		}
	}
	else
		zbx_binary_heap_update_direct(&manager->queue, &elem);
}

/******************************************************************************
 *                                                                            *
 * Function: am_pop_mediatype                                                 *
 *                                                                            *
 * Purpose: gets the next media type from queue                               *
 *                                                                            *
 * Parameters: manager - [IN] the alert manager                               *
 *                                                                            *
 * Return value: The media type object.                                       *
 *                                                                            *
 ******************************************************************************/
static zbx_am_mediatype_t	*am_pop_mediatype(zbx_am_t *manager)
{
	zbx_binary_heap_elem_t	*elem;
	zbx_am_mediatype_t	*mediatype;

	if (FAIL != zbx_binary_heap_empty(&manager->queue))
		return NULL;

	elem = zbx_binary_heap_find_min(&manager->queue);
	mediatype = (zbx_am_mediatype_t *)elem->data;
	mediatype->location = ZBX_AM_LOCATION_NOWHERE;

	zbx_binary_heap_remove_min(&manager->queue);

	return mediatype;
}

/******************************************************************************
 *                                                                            *
 * Function: am_remove_mediatype                                              *
 *                                                                            *
 * Purpose: removes alert pool                                                *
 *                                                                            *
 * Parameters: manager - [IN] the alert manager                               *
 *             alert   - [IN] the alert pool                                  *
 *                                                                            *
 ******************************************************************************/
static void	am_remove_mediatype(zbx_am_t *manager, zbx_am_mediatype_t *mediatype)
{
	zbx_free(mediatype->description);
	zbx_free(mediatype->smtp_server);
	zbx_free(mediatype->smtp_helo);
	zbx_free(mediatype->smtp_email);
	zbx_free(mediatype->exec_path);
	zbx_free(mediatype->exec_params);
	zbx_free(mediatype->gsm_modem);
	zbx_free(mediatype->username);
	zbx_free(mediatype->passwd);
	zbx_binary_heap_destroy(&mediatype->queue);
	zbx_hashset_remove_direct(&manager->mediatypes, mediatype);
}

/******************************************************************************
 *                                                                            *
 * Function: am_calc_alertpoolid                                              *
 *                                                                            *
 * Purpose: calculate alert pool id from event source, object and objectid    *
 *                                                                            *
 * Parameters: source   - [IN] the event source                               *
 *             object   - [IN] the event object type                          *
 *             objectid - [IN] the event objectid                             *
 *                                                                            *
 * Return value: The alert pool id.                                           *
 *                                                                            *
 ******************************************************************************/
static zbx_uint64_t	am_calc_alertpoolid(int source, int object, zbx_uint64_t objectid)
{
	zbx_hash_t	hash;

	hash = ZBX_DEFAULT_UINT64_HASH_FUNC(&objectid);
	hash = ZBX_DEFAULT_UINT64_HASH_ALGO(&source, sizeof(source), hash);
	hash = ZBX_DEFAULT_UINT64_HASH_ALGO(&object, sizeof(object), hash);

	return hash;
}

/******************************************************************************
 *                                                                            *
 * Function: am_get_alertpool                                                 *
 *                                                                            *
 * Purpose: gets alert pool object, creating one if the object with specified *
 *          identifiers was not found                                         *
 *                                                                            *
 * Parameters: manager     - [IN] the alert manager                           *
 *             mediatypeid - [IN] the media type identifier                   *
 *             alertpoolid - [IN] the alert pool identifier                   *
 *                                                                            *
 * Return value: The alert pool object.                                       *
 *                                                                            *
 ******************************************************************************/
static zbx_am_alertpool_t	*am_get_alertpool(zbx_am_t *manager, zbx_uint64_t mediatypeid, zbx_uint64_t alertpoolid)
{
	zbx_am_alertpool_t	*alertpool, alertpool_local;

	alertpool_local.mediatypeid = mediatypeid;
	alertpool_local.id = alertpoolid;

	if (NULL == (alertpool = (zbx_am_alertpool_t *)zbx_hashset_search(&manager->alertpools, &alertpool_local)))
	{
		alertpool = (zbx_am_alertpool_t *)zbx_hashset_insert(&manager->alertpools, &alertpool_local,
				sizeof(alertpool_local));

		zbx_binary_heap_create(&alertpool->queue, am_alert_queue_compare, ZBX_BINARY_HEAP_OPTION_EMPTY);

		alertpool->location = ZBX_AM_LOCATION_NOWHERE;
	}

	return alertpool;
}

/******************************************************************************
 *                                                                            *
 * Function: am_push_alertpool                                                *
 *                                                                            *
 * Purpose: pushes alert pool into media type alert pool queue                *
 *                                                                            *
 * Parameters: mediatype - [IN] the media type                                *
 *             alertpool - [IN] the alert pool                                *
 *                                                                            *
 * Comments: The alert pool is inserted into queue only if it was not already *
 *           queued. Otherwise its position in the queue is updated.          *
 *                                                                            *
 ******************************************************************************/
static void	am_push_alertpool(zbx_am_mediatype_t *mediatype, zbx_am_alertpool_t *alertpool)
{
	zbx_binary_heap_elem_t	elem = {alertpool->id, alertpool};

	if (ZBX_AM_LOCATION_NOWHERE == alertpool->location)
	{
		zbx_binary_heap_insert(&mediatype->queue, &elem);
		alertpool->location = ZBX_AM_LOCATION_QUEUE;
	}
	else
		zbx_binary_heap_update_direct(&mediatype->queue, &elem);
}

/******************************************************************************
 *                                                                            *
 * Function: am_pop_alertpool                                                 *
 *                                                                            *
 * Purpose: gets the next alert pool from queue                               *
 *                                                                            *
 * Parameters: mediatype - [IN] the media type                                *
 *                                                                            *
 * Return value: The alert pool object.                                       *
 *                                                                            *
 ******************************************************************************/
static zbx_am_alertpool_t	*am_pop_alertpool(zbx_am_mediatype_t *mediatype)
{
	zbx_binary_heap_elem_t	*elem;
	zbx_am_alertpool_t	*alertpool;

	if (FAIL != zbx_binary_heap_empty(&mediatype->queue))
		return NULL;

	elem = zbx_binary_heap_find_min(&mediatype->queue);
	alertpool = (zbx_am_alertpool_t *)elem->data;
	alertpool->location = ZBX_AM_LOCATION_NOWHERE;

	zbx_binary_heap_remove_min(&mediatype->queue);

	return alertpool;
}

/******************************************************************************
 *                                                                            *
 * Function: am_remove_alertpool                                              *
 *                                                                            *
 * Purpose: removes alert pool                                                *
 *                                                                            *
 * Parameters: manager - [IN] the alert manager                               *
 *             alert   - [IN] the alert pool                                  *
 *                                                                            *
 ******************************************************************************/
static void	am_remove_alertpool(zbx_am_t *manager, zbx_am_alertpool_t *alertpool)
{
	zbx_binary_heap_destroy(&alertpool->queue);
	zbx_hashset_remove_direct(&manager->alertpools, alertpool);
}

/******************************************************************************
 *                                                                            *
 * Function: am_create_alert                                                  *
 *                                                                            *
 * Purpose: creates new alert object                                          *
 *                                                                            *
 * Parameters: ...           - [IN] alert data                                *
 *                                                                            *
 * Return value: The alert object.                                            *
 *                                                                            *
 ******************************************************************************/
static zbx_am_alert_t	*am_create_alert(zbx_uint64_t alertid, zbx_uint64_t mediatypeid, int source, int object,
		zbx_uint64_t objectid, const char *sendto, const char *subject, const char *message, int status,
		int retries, int nextsend)
{
	zbx_am_alert_t		*alert;

	alert = (zbx_am_alert_t *)zbx_malloc(NULL, sizeof(zbx_am_alert_t));
	alert->alertid = alertid;
	alert->mediatypeid = mediatypeid;
	alert->alertpoolid = am_calc_alertpoolid(source, object, objectid);;

	alert->sendto = zbx_strdup(NULL, sendto);
	alert->subject = zbx_strdup(NULL, subject);
	alert->message = zbx_strdup(NULL, message);
	alert->status = status;
	alert->retries = retries;
	alert->nextsend = nextsend;

	return alert;
}

/******************************************************************************
 *                                                                            *
 * Function: am_alert_free                                                    *
 *                                                                            *
 * Purpose: frees the alert object                                            *
 *                                                                            *
 * Parameters: alert - [IN] the alert object                                  *
 *                                                                            *
 ******************************************************************************/
static void	am_alert_free(zbx_am_alert_t *alert)
{
	zbx_free(alert->sendto);
	zbx_free(alert->subject);
	zbx_free(alert->message);
	zbx_free(alert);
}

/******************************************************************************
 *                                                                            *
 * Function: am_push_alert                                                    *
 *                                                                            *
 * Purpose: pushes alert into alert pool alert queue                          *
 *                                                                            *
 * Parameters: alertpool - [IN] the alert pool                                *
 *             alert     - [IN] the alert                                     *
 *                                                                            *
 ******************************************************************************/
static void	am_push_alert(zbx_am_alertpool_t *alertpool, zbx_am_alert_t *alert)
{
	zbx_binary_heap_elem_t	elem;

	elem.data = alert;
	zbx_binary_heap_insert(&alertpool->queue, &elem);
}

/******************************************************************************
 *                                                                            *
 * Function: am_pop_alert                                                     *
 *                                                                            *
 * Purpose: gets the next alert from queue                                    *
 *                                                                            *
 * Parameters: manager - [IN] the manager                                     *
 *                                                                            *
 * Return value: The alert object.                                            *
 *                                                                            *
 ******************************************************************************/
static zbx_am_alert_t	*am_pop_alert(zbx_am_t *manager)
{
	zbx_am_mediatype_t	*mediatype;
	zbx_am_alertpool_t	*alertpool;
	zbx_am_alert_t		*alert;
	zbx_binary_heap_elem_t	*elem;

	if (NULL == (mediatype = am_pop_mediatype(manager)))
		return NULL;

	alertpool = am_pop_alertpool(mediatype);

	elem = zbx_binary_heap_find_min(&alertpool->queue);
	alert = (zbx_am_alert_t *)elem->data;
	zbx_binary_heap_remove_min(&alertpool->queue);

	/* requeue media type if the number of parallel alerts has not yet reached */
	mediatype->alerts_num++;
	if (0 == mediatype->maxsessions || mediatype->alerts_num < mediatype->maxsessions)
		am_push_meditype(manager, mediatype);

	return alert;
}

/******************************************************************************
 *                                                                            *
 * Function: am_remove_alert                                                  *
 *                                                                            *
 * Purpose: removes alert and requeues associated alert pool and media type   *
 *                                                                            *
 * Parameters: manager - [IN] the alert manager                               *
 *             alert   - [IN] the alert                                       *
 *                                                                            *
 ******************************************************************************/
static void	am_remove_alert(zbx_am_t *manager, zbx_am_alert_t *alert)
{
	zbx_am_alertpool_t	*alertpool;
	zbx_am_mediatype_t	*mediatype;

	if (NULL != (mediatype = am_get_mediatype(manager, alert->mediatypeid)))
	{
		mediatype->alerts_num--;

		if (NULL != (alertpool = am_get_alertpool(manager, alert->mediatypeid, alert->alertpoolid)))
		{
			if (SUCCEED == zbx_binary_heap_empty(&alertpool->queue))
				am_remove_alertpool(manager, alertpool);
			else
				am_push_alertpool(mediatype, alertpool);
		}

		if (SUCCEED == zbx_binary_heap_empty(&mediatype->queue) && 0 == mediatype->alerts_num)
			am_remove_mediatype(manager, mediatype);
		else
			am_push_meditype(manager, mediatype);
	}

	am_alert_free(alert);
}

/******************************************************************************
 *                                                                            *
 * Function: am_retry_alert                                                   *
 *                                                                            *
 * Purpose: retries alert if there are attempts left or removes it            *
 *                                                                            *
 * Parameters: manager - [IN] the alert manager                               *
 *             alert   - [IN] the alert                                       *
 *                                                                            *
 * Return value: SUCCEED - the alert was queued to be sent again              *
 *               FAIL - the alert retries value exceeded the mediatype        *
 *                      maxattempts limit and alert was removed as failed.    *
 *                                                                            *
 ******************************************************************************/
static int	am_retry_alert(zbx_am_t *manager, zbx_am_alert_t *alert)
{
	const char		*__function_name = "am_register_alerter";

	zbx_am_alertpool_t	*alertpool;
	zbx_am_mediatype_t	*mediatype;
	int			ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() alertid:" ZBX_FS_UI64, __function_name, alert->alertid);

	if (NULL == (mediatype = am_get_mediatype(manager, alert->mediatypeid)))
	{
		THIS_SHOULD_NEVER_HAPPEN;
		am_remove_alert(manager, alert);
		goto out;
	}

	if (++alert->retries >= mediatype->maxattempts)
	{
		am_remove_alert(manager, alert);
		goto out;
	}

	alert->nextsend = time(NULL) + mediatype->attempt_interval;

	mediatype->alerts_num--;
	alertpool = am_get_alertpool(manager, alert->mediatypeid, alert->alertpoolid);

	am_push_alert(alertpool, alert);
	am_push_alertpool(mediatype, alertpool);
	am_push_meditype(manager, mediatype);

	ret = SUCCEED;

out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: am_alerter_free                                                  *
 *                                                                            *
 * Purpose: frees alerter                                                     *
 *                                                                            *
 ******************************************************************************/
static void	am_alerter_free(zbx_am_alerter_t *alerter)
{
	zbx_ipc_client_close(alerter->client);

	zbx_free(alerter);
}

/******************************************************************************
 *                                                                            *
 * Function: am_register_alerter                                              *
 *                                                                            *
 * Purpose: registers alerter                                                 *
 *                                                                            *
 * Parameters: manager - [IN] the manager                                     *
 *             client  - [IN] the connected alerter                           *
 *             message - [IN] the received message                            *
 *                                                                            *
 ******************************************************************************/
static void	am_register_alerter(zbx_am_t *manager, zbx_ipc_client_t *client, zbx_ipc_message_t *message)
{
	const char		*__function_name = "am_register_alerter";
	zbx_am_alerter_t	*alerter = NULL;
	pid_t			ppid;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	memcpy(&ppid, message->data, sizeof(ppid));

	if (ppid != getppid())
	{
		zbx_ipc_client_close(client);
		zabbix_log(LOG_LEVEL_DEBUG, "refusing connection from foreign process");
	}
	else
	{
		if (manager->next_alerter_index == manager->alerters.values_num)
		{
			THIS_SHOULD_NEVER_HAPPEN;
			exit(EXIT_FAILURE);
		}

		alerter = (zbx_am_alerter_t *)manager->alerters.values[manager->next_alerter_index++];
		alerter->client = client;

		zbx_hashset_insert(&manager->alerters_client, &alerter, sizeof(alerter));
		zbx_queue_ptr_push(&manager->free_alerters, alerter);
	}

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}


/******************************************************************************
 *                                                                            *
 * Function: am_get_alerter_by_client                                         *
 *                                                                            *
 * Purpose: returns alerter by connected client                               *
 *                                                                            *
 * Parameters: manager - [IN] the manager                                     *
 *             client  - [IN] the connected alerter                           *
 *                                                                            *
 * Return value: The IPMI poller                                              *
 *                                                                            *
 ******************************************************************************/
static zbx_am_alerter_t	*am_get_alerter_by_client(zbx_am_t *manager, zbx_ipc_client_t *client)
{
	zbx_am_alerter_t	**alerter, alerter_local, *plocal = &alerter_local;

	plocal->client = client;

	alerter = (zbx_am_alerter_t **)zbx_hashset_search(&manager->alerters_client, &plocal);

	if (NULL == alerter)
	{
		THIS_SHOULD_NEVER_HAPPEN;
		exit(EXIT_FAILURE);
	}

	return *alerter;
}

/******************************************************************************
 *                                                                            *
 * Function: am_init                                                          *
 *                                                                            *
 * Purpose: initializes alert manager                                         *
 *                                                                            *
 * Parameters: manager - [IN] the manager to initialize                       *
 *                                                                            *
 ******************************************************************************/
static void	am_init(zbx_am_t *manager)
{
	const char		*__function_name = "am_init";
	int			i;
	zbx_am_alerter_t	*alerter;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() alerters:%d", __function_name, CONFIG_ALERTER_FORKS);

	zbx_vector_ptr_create(&manager->alerters);
	zbx_queue_ptr_create(&manager->free_alerters);
	zbx_hashset_create(&manager->alerters_client, 0, alerter_hash_func, alerter_compare_func);

	manager->next_alerter_index = 0;

	for (i = 0; i < CONFIG_ALERTER_FORKS; i++)
	{
		alerter = (zbx_am_alerter_t *)zbx_malloc(NULL, sizeof(zbx_am_alerter_t));

		alerter->client = NULL;

		zbx_vector_ptr_append(&manager->alerters, alerter);
	}

	zbx_hashset_create(&manager->mediatypes, 5, ZBX_DEFAULT_UINT64_HASH_FUNC, ZBX_DEFAULT_UINT64_COMPARE_FUNC);
	zbx_hashset_create(&manager->alertpools, 100, am_alertpool_hash_func, am_alertpool_compare_func);
	zbx_hashset_create(&manager->alertupdates, 100, ZBX_DEFAULT_UINT64_HASH_FUNC, ZBX_DEFAULT_UINT64_COMPARE_FUNC);
	zbx_binary_heap_create(&manager->queue, am_mediatype_queue_compare, ZBX_BINARY_HEAP_OPTION_DIRECT);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: am_destroy                                                       *
 *                                                                            *
 * Purpose: destroys alert manager                                            *
 *                                                                            *
 * Parameters: manager - [IN] the manager to destroy                          *
 *                                                                            *
 ******************************************************************************/
static void	am_destroy(zbx_am_t *manager)
{
	zbx_am_alert_t	*alert;

	zbx_hashset_destroy(&manager->alerters_client);
	zbx_queue_ptr_destroy(&manager->free_alerters);
	zbx_vector_ptr_clear_ext(&manager->alerters, (zbx_mem_free_func_t)am_alerter_free);
	zbx_vector_ptr_destroy(&manager->alerters);

	while (NULL != (alert = am_pop_alert(manager)))
		am_remove_alert(manager, alert);

	zbx_binary_heap_destroy(&manager->queue);
	zbx_hashset_destroy(&manager->alertupdates);
	zbx_hashset_destroy(&manager->alertpools);
	zbx_hashset_destroy(&manager->mediatypes);
}

/******************************************************************************
 *                                                                            *
 * Function: am_db_get_alerts                                                 *
 *                                                                            *
 * Purpose: reads the new alerts from database                                *
 *                                                                            *
 * Parameters: alerts - [OUT] the new alerts                                  *
 *             now    - [IN] the current timestamp                            *
 *                                                                            *
 * Comments: One the first call this function will return new and not sent    *
 *           alerts. After that only new alerts are returned.                 *
 *                                                                            *
 ******************************************************************************/
static void	am_db_get_alerts(zbx_vector_ptr_t *alerts, int now)
{
	const char		*__function_name = "am_db_get_alerts";

	static int		status_limit = 2;
	zbx_uint64_t		status_filter[] = {ALERT_STATUS_NEW, ALERT_STATUS_NOT_SENT};
	DB_RESULT		result;
	DB_ROW			row;
	char			*sql = NULL;
	size_t			sql_alloc = 0, sql_offset = 0;
	zbx_uint64_t		alertid, mediatypeid, objectid;
	int			status, attempts, source, object;
	zbx_am_alert_t		*alert;
	zbx_vector_uint64_t	alertids;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_vector_uint64_create(&alertids);

	zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
			"select a.alertid,a.mediatypeid,a.sendto,a.subject,a.message,a.status,a.retries,"
				"e.source,e.object,e.objectid"
			" from alerts a"
			" left join events e"
				" on a.eventid=e.eventid"
			" where alerttype=%d"
			" and",
			ALERT_TYPE_MESSAGE);

	DBadd_condition_alloc(&sql, &sql_alloc, &sql_offset, "a.status", status_filter, status_limit);
	zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset, " order by a.alertid");

	result = DBselect("%s", sql);
	while (NULL != (row = DBfetch(result)))
	{
		ZBX_STR2UINT64(alertid, row[0]);
		ZBX_STR2UINT64(mediatypeid, row[1]);
		status = atoi(row[5]);
		attempts = atoi(row[6]);
		source = atoi(row[7]);
		object = atoi(row[8]);
		ZBX_STR2UINT64(objectid, row[9]);

		alert = am_create_alert(alertid, mediatypeid, source, object, objectid, row[2], row[3], row[4],
				status, attempts, now);

		zbx_vector_ptr_append(alerts, alert);

		if (ALERT_STATUS_NEW == alert->status)
			zbx_vector_uint64_append(&alertids, alert->alertid);
	}

	DBfree_result(result);

	if (0 != alertids.values_num)
	{
		sql_offset = 0;
		zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset, "update alerts set status=%d where",
				ALERT_STATUS_NOT_SENT);
		DBadd_condition_alloc(&sql, &sql_alloc, &sql_offset, "alertid", alertids.values, alertids.values_num);

		DBexecute("%s", sql);
	}

	zbx_free(sql);
	zbx_vector_uint64_destroy(&alertids);

	status_limit = 1;

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s() alerts:%d", __function_name, alerts->values_num);
}

/******************************************************************************
 *                                                                            *
 * Function: am_db_update_mediatypes                                          *
 *                                                                            *
 * Purpose: updates media types of the new alerts                             *
 *                                                                            *
 * Parameters: manager - [IN] the alert manager                               *
 *             alerts  - [IN] the alerts                                      *
 *                                                                            *
 * Comments: Existing media types will be updated and new ones created if     *
 *           necessary.                                                       *
 *                                                                            *
 ******************************************************************************/
static void	am_db_update_mediatypes(zbx_am_t *manager, zbx_vector_ptr_t *alerts)
{
	const char		*__function_name = "am_db_update_mediatypes";

	DB_RESULT		result;
	DB_ROW			row;
	char			*sql = NULL;
	size_t			sql_alloc = 0, sql_offset = 0;
	zbx_vector_uint64_t	mediatypeids;
	int			i, type, maxsessions, maxattempts, attempt_interval, mediatypes_num;
	zbx_am_alert_t		*alert;
	zbx_uint64_t		mediatypeid;
	unsigned short		smtp_port;
	unsigned char		smtp_security, smtp_verify_peer, smtp_verify_host, smtp_authentication;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_vector_uint64_create(&mediatypeids);

	for (i = 0; i < alerts->values_num; i++)
	{
		alert = (zbx_am_alert_t *)alerts->values[i];
		zbx_vector_uint64_append(&mediatypeids, alert->mediatypeid);
	}

	zbx_vector_uint64_sort(&mediatypeids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);
	zbx_vector_uint64_uniq(&mediatypeids, ZBX_DEFAULT_UINT64_COMPARE_FUNC);

	zbx_strcpy_alloc(&sql, &sql_alloc, &sql_offset,
			"select mediatypeid,type,description,smtp_server,smtp_helo,smtp_email,exec_path,gsm_modem,"
				"username,passwd,smtp_port,smtp_security,smtp_verify_peer,smtp_verify_host,"
				"smtp_authentication,exec_params,maxsessions,maxattempts,attempt_interval"
			" from media_type"
			" where");

	DBadd_condition_alloc(&sql, &sql_alloc, &sql_offset, "mediatypeid", mediatypeids.values,
			mediatypeids.values_num);

	result = DBselect("%s", sql);
	zbx_free(sql);

	while (NULL != (row = DBfetch(result)))
	{
		if (FAIL == is_ushort(row[10], &smtp_port))
		{
			THIS_SHOULD_NEVER_HAPPEN;
			continue;
		}

		ZBX_STR2UINT64(mediatypeid, row[0]);
		type = atoi(row[1]);
		ZBX_STR2UCHAR(smtp_security, row[11]);
		ZBX_STR2UCHAR(smtp_verify_peer, row[12]);
		ZBX_STR2UCHAR(smtp_verify_host, row[13]);
		ZBX_STR2UCHAR(smtp_authentication, row[14]);
		maxsessions = atoi(row[16]);
		maxattempts = atoi(row[17]);
		attempt_interval = atoi(row[18]);

		am_update_mediatype(manager, mediatypeid, type, row[2], row[3], row[4], row[5], row[6], row[7], row[8],
				row[9], smtp_port, smtp_security, smtp_verify_peer, smtp_verify_host,
				smtp_authentication, row[15], maxsessions, maxattempts, attempt_interval);
	}

	DBfree_result(result);

	mediatypes_num = mediatypeids.values_num;
	zbx_vector_uint64_destroy(&mediatypeids);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s() mediatypes:%d", __function_name, mediatypes_num);
}

/******************************************************************************
 *                                                                            *
 * Function: am_db_queue_alerts                                               *
 *                                                                            *
 * Purpose: queues new alerts from database                                   *
 *                                                                            *
 * Parameters: manager - [IN] the alert manager                               *
 *             now     - [IN] the current timestamp                           *
 *                                                                            *
 ******************************************************************************/
static void	am_db_queue_alerts(zbx_am_t *manager, int now)
{
	const char		*__function_name = "am_db_queue_alerts";

	zbx_am_alert_t		*alert;
	zbx_vector_ptr_t	alerts;
	int			i;
	zbx_am_alertpool_t	*alertpool;
	zbx_am_mediatype_t	*mediatype;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	zbx_vector_ptr_create(&alerts);

	am_db_get_alerts(&alerts, now);

	if (0 < alerts.values_num)
	{
		am_db_update_mediatypes(manager, &alerts);

		for (i = 0; i < alerts.values_num; i++)
		{
			alert = (zbx_am_alert_t *)alerts.values[i];

			if (NULL == (mediatype = am_get_mediatype(manager, alert->mediatypeid)))
			{
				am_alert_free(alert);
				continue;
			}

			alertpool = am_get_alertpool(manager, alert->mediatypeid, alert->alertpoolid);

			am_push_alert(alertpool, alert);
			am_push_alertpool(mediatype, alertpool);
			am_push_meditype(manager, mediatype);
		}
	}

	zbx_vector_ptr_destroy(&alerts);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: am_db_update_alert                                               *
 *                                                                            *
 * Purpose: update alert status in local cache to be flushed after reading    *
 *          new alerts from database                                          *
 *                                                                            *
 * Parameters: manager - [IN] the manager                                     *
 *             alertid - [IN] the alert identifier                            *
 *             status  - [IN] the alert status                                *
 *             retries - [IN] the number of attempted sending retries         *
 *             error   - [IN] the error message                               *
 *                                                                            *
 ******************************************************************************/
static void	am_db_update_alert(zbx_am_t *manager, zbx_uint64_t alertid, int status, int retries, char *error)
{
	const char		*__function_name = "am_db_update_alert";

	zbx_am_alertstatus_t	*update;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() alertid:" ZBX_FS_UI64 " status:%d retries:%d error:%s", __function_name,
			alertid, status, retries, error);

	if (NULL == (update = (zbx_am_alertstatus_t *)zbx_hashset_search(&manager->alertupdates, &alertid)))
	{
		zbx_am_alertstatus_t	update_local = {alertid};

		update = (zbx_am_alertstatus_t *)zbx_hashset_insert(&manager->alertupdates, &update_local,
				sizeof(update_local));
	}

	update->retries = retries;
	update->status = status;
	ZBX_UPDATE_STR(update->error, error);

	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: am_db_flush_alert_updates                                        *
 *                                                                            *
 * Purpose: flush cached alert status updates to database                     *
 *                                                                            *
 * Parameters: manager - [IN] the manager                                     *
 *                                                                            *
 ******************************************************************************/
static void	am_db_flush_alert_updates(zbx_am_t *manager)
{
	const char		*__function_name = "am_db_flush_alert_updates";

	zbx_vector_ptr_t	updates;
	zbx_hashset_iter_t	iter;
	zbx_am_alertstatus_t	*update;
	char			*sql = NULL, *error_esc;
	size_t			sql_alloc = 0, sql_offset = 0;
	int			i;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() updates:%d", __function_name, manager->alertupdates.num_data);

	if (0 == manager->alertupdates.num_data)
		goto out;

	zbx_vector_ptr_create(&updates);

	zbx_hashset_iter_reset(&manager->alertupdates, &iter);
	while (NULL != (update = (zbx_am_alertstatus_t *)zbx_hashset_iter_next(&iter)))
		zbx_vector_ptr_append(&updates, update);

	zbx_vector_ptr_sort(&updates, ZBX_DEFAULT_UINT64_PTR_COMPARE_FUNC);

	DBbegin();
	DBbegin_multiple_update(&sql, &sql_alloc, &sql_offset)

	for (i = 0; i < updates.values_num; i++)
	{
		update = (zbx_am_alertstatus_t *)updates.values[i];

		error_esc = DBdyn_escape_string_len(update->error, ALERT_ERROR_LEN);

		zbx_snprintf_alloc(&sql, &sql_alloc, &sql_offset,
				"update alerts"
				" set status=%d,"
					"retries=%d,"
					"error='%s'"
				" where alertid=" ZBX_FS_UI64 ";\n",
				update->status, update->retries, error_esc, update->alertid);

		DBexecute_overflowed_sql(&sql, &sql_alloc, &sql_offset);

		zbx_free(error_esc);
	}

	DBend_multiple_update(&sql, &sql_alloc, &sql_offset)

	if (16 < sql_offset)	/* in ORACLE always present begin..end; */
		DBexecute("%s", sql);

	DBcommit();
	zbx_free(sql);

	zbx_hashset_iter_reset(&manager->alertupdates, &iter);
	while (NULL != (update = (zbx_am_alertstatus_t *)zbx_hashset_iter_next(&iter)))
		zbx_free(update->error);

	zbx_hashset_clear(&manager->alertupdates);
	zbx_vector_ptr_destroy(&updates);

out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);
}

/******************************************************************************
 *                                                                            *
 * Function: am_prepare_mediatype_exec_command                                *
 *                                                                            *
 * Purpose: gets script media type parameters with expanded macros            *
 *                                                                            *
 * Parameters: mediatype - [IN] the media type                                *
 *             alert     - [IN] the alert                                     *
 *             cmd       - [OUT] the command to execute                       *
 *             error     - [OUT] the error message                            *
 *                                                                            *
 * Return value: SUCCEED - the command was prepared successfully              *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	am_prepare_mediatype_exec_command(zbx_am_mediatype_t *mediatype, zbx_am_alert_t *alert, char **cmd,
		char **error)
{
	DB_ALERT	db_alert;
	size_t		cmd_alloc = ZBX_KIBIBYTE, cmd_offset = 0;
	int		ret = FAIL;

	*cmd = zbx_malloc(NULL, cmd_alloc);

	zbx_snprintf_alloc(cmd, &cmd_alloc, &cmd_offset, "%s/%s", CONFIG_ALERT_SCRIPTS_PATH, mediatype->exec_path);

	if (0 == access(*cmd, X_OK))
	{
		char	*pstart, *pend, *param = NULL;
		size_t	param_alloc = 0, param_offset;

		db_alert.sendto = alert->sendto;
		db_alert.subject = alert->subject;
		db_alert.message = alert->message;

		for (pstart = mediatype->exec_params; NULL != (pend = strchr(pstart, '\n')); pstart = pend + 1)
		{
			char	*param_esc;

			param_offset = 0;

			zbx_strncpy_alloc(&param, &param_alloc, &param_offset, pstart, pend - pstart);

			substitute_simple_macros(NULL, NULL, NULL, NULL, NULL, NULL, NULL, &db_alert, &param,
					MACRO_TYPE_ALERT, NULL, 0);

			param_esc = zbx_dyn_escape_shell_single_quote(param);
			zbx_snprintf_alloc(cmd, &cmd_alloc, &cmd_offset, " '%s'", param_esc);

			zbx_free(param_esc);
		}

		zbx_free(param);

		ret = SUCCEED;
	}
	else
	{
		*error = zbx_dsprintf(*error, "Cannot exectue command \"%s\": %s", *cmd, zbx_strerror(errno));
		zbx_free(*cmd);
	}

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: am_process_alert                                                 *
 *                                                                            *
 * Purpose: sends alert to the alerter                                        *
 *                                                                            *
 * Parameters: manager - [IN] the alert manager                               *
 *             alerter - [IN] the target alerter                              *
 *             alert   - [IN] the alert to send                               *
 *                                                                            *
 * Return value: SUCCEED - the alert was successfully sent to alerter         *
 *               FAIL    - otherwise                                          *
 *                                                                            *
 ******************************************************************************/
static int	am_process_alert(zbx_am_t *manager, zbx_am_alerter_t *alerter, zbx_am_alert_t *alert)
{
	const char		*__function_name = "am_process_alert";

	zbx_am_mediatype_t	*mediatype;
	unsigned char		*data = NULL;
	size_t			data_len;
	zbx_uint64_t		command;
	char			*cmd = NULL, *error = NULL;
	int			ret = FAIL;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s() alertid:" ZBX_FS_UI64 " mediatypeid:" ZBX_FS_UI64, __function_name,
			alert->alertid, alert->mediatypeid);

	if (NULL == (mediatype = am_get_mediatype(manager, alert->mediatypeid)))
	{
		THIS_SHOULD_NEVER_HAPPEN;
		am_alert_free(alert);
		goto out;
	}

	switch (mediatype->type)
	{
		case MEDIA_TYPE_EMAIL:
			command = ZBX_IPC_ALERTER_EMAIL;
			data_len = zbx_alerter_serialize_email(&data, alert->alertid, alert->sendto, alert->subject,
					alert->message, mediatype->smtp_server, mediatype->smtp_port,
					mediatype->smtp_helo, mediatype->smtp_email, mediatype->smtp_security,
					mediatype->smtp_verify_peer, mediatype->smtp_verify_host,
					mediatype->smtp_authentication, mediatype->username, mediatype->passwd);
			break;
		case MEDIA_TYPE_JABBER:
			command = ZBX_IPC_ALERTER_JABBER;
			data_len = zbx_alerter_serialize_jabber(&data, alert->alertid, alert->sendto, alert->subject,
					alert->message, mediatype->username, mediatype->passwd);
			break;
		case MEDIA_TYPE_SMS:
			command = ZBX_IPC_ALERTER_SMS;
			data_len = zbx_alerter_serialize_sms(&data, alert->alertid, alert->sendto, alert->message,
					mediatype->gsm_modem);
			break;
		case MEDIA_TYPE_EZ_TEXTING:
			command = ZBX_IPC_ALERTER_EZTEXTING;
			data_len = zbx_alerter_serialize_eztexting(&data, alert->alertid, alert->sendto, alert->message,
					mediatype->username, mediatype->passwd, mediatype->exec_path);
			break;
		case MEDIA_TYPE_EXEC:
			command = ZBX_IPC_ALERTER_EXEC;
			if (FAIL == am_prepare_mediatype_exec_command(mediatype, alert, &cmd, &error))
			{
				am_db_update_alert(manager, alert->alertid, ALERT_STATUS_FAILED, 0, error);
				am_remove_alert(manager, alert);
				zbx_free(error);
				goto out;
			}
			data_len = zbx_alerter_serialize_exec(&data, alert->alertid, cmd);
			zbx_free(cmd);
			break;
		default:
			am_db_update_alert(manager, alert->alertid, ALERT_STATUS_FAILED, 0, "unsupported media type");
			am_remove_alert(manager, alert);
			zabbix_log(LOG_LEVEL_ERR, "cannot process alertid:" ZBX_FS_UI64 ": unsupported media type: %d",
					alert->alertid, mediatype->type);
			goto out;
	}

	alerter->alert = alert;
	zbx_ipc_client_send(alerter->client, command, data, data_len);
	zbx_free(data);

	ret = SUCCEED;
out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: am_process_result                                                *
 *                                                                            *
 * Purpose: process alerter result                                            *
 *                                                                            *
 * Parameters: manager - [IN] the manager                                     *
 *             client  - [IN] the connected alerter                           *
 *             message - [IN] the received message                            *
 *                                                                            *
 * Return value: SUCCEED - the alert was sent successfully                    *
 *               FAIL - otherwise                                             *
 *                                                                            *
 ******************************************************************************/
static int	am_process_result(zbx_am_t *manager, zbx_ipc_client_t *client, zbx_ipc_message_t *message)
{
	const char		*__function_name = "am_process_result";

	int			ret, errcode, retries, status;
	char			*errmsg;
	zbx_am_alerter_t	*alerter;
	zbx_uint64_t		alertid;

	zabbix_log(LOG_LEVEL_DEBUG, "In %s()", __function_name);

	if (NULL == (alerter = am_get_alerter_by_client(manager, client)))
	{
		THIS_SHOULD_NEVER_HAPPEN;
		ret = FAIL;
		goto out;
	}
	retries = alerter->alert->retries;
	alertid = alerter->alert->alertid;

	zbx_alerter_deserialize_result(message->data, &errcode, &errmsg);

	if (SUCCEED == errcode)
	{
		errmsg = zbx_strdup(errmsg, "");
		status = ALERT_STATUS_SENT;

		am_remove_alert(manager, alerter->alert);
		ret = SUCCEED;
	}
	else
	{
		if (SUCCEED == am_retry_alert(manager, alerter->alert))
			status = ALERT_STATUS_NOT_SENT;
		else
			status = ALERT_STATUS_FAILED;

		ret = FAIL;
	}

	am_db_update_alert(manager, alertid, status, retries, errmsg);

	alerter->alert = NULL;
	zbx_free(errmsg);

	zbx_queue_ptr_push(&manager->free_alerters, alerter);

out:
	zabbix_log(LOG_LEVEL_DEBUG, "End of %s()", __function_name);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Function: am_check_queue                                                   *
 *                                                                            *
 * Purpose: checks alert queue if there is an alert that should be sent now   *
 *                                                                            *
 * Parameters: manager - [IN] the alert manager                               *
 *             now     - [IN] the current timestamp                           *
 *                                                                            *
 * Return value: SUCCEED - an alert can be sent                               *
 *               FAIL - there are no alerts to be sent at this time           *
 *                                                                            *
 ******************************************************************************/
static int	am_check_queue(zbx_am_t *manager, int now)
{
	zbx_binary_heap_elem_t	*elem;
	zbx_am_mediatype_t	*mediatype;
	zbx_am_alertpool_t	*alertpool;
	zbx_am_alert_t		*alert;

	if (SUCCEED == zbx_binary_heap_empty(&manager->queue))
		return FAIL;

	elem = zbx_binary_heap_find_min(&manager->queue);
	mediatype = (zbx_am_mediatype_t *)elem->data;

	if (SUCCEED == zbx_binary_heap_empty(&mediatype->queue))
		return FAIL;

	elem = zbx_binary_heap_find_min(&mediatype->queue);
	alertpool = (zbx_am_alertpool_t *)elem->data;

	if (SUCCEED == zbx_binary_heap_empty(&alertpool->queue))
		return FAIL;

	elem = zbx_binary_heap_find_min(&alertpool->queue);
	alert = (zbx_am_alert_t *)elem->data;

	if (alert->nextsend > now)
		return FAIL;

	return SUCCEED;
}

ZBX_THREAD_ENTRY(alert_manager_thread, args)
{
#define	STAT_INTERVAL	5	/* if a process is busy and does not sleep then update status not faster than */
				/* once in STAT_INTERVAL seconds */

	zbx_ipc_service_t	alerter_service;
	zbx_am_t		manager;
	char			*error = NULL;
	zbx_ipc_client_t	*client;
	zbx_ipc_message_t	*message;
	zbx_am_alerter_t	*alerter;
	int			ret, sent_num, failed_num, now, time_db;
	double			time_stat, time_idle, time_now;

	process_type = ((zbx_thread_args_t *)args)->process_type;
	server_num = ((zbx_thread_args_t *)args)->server_num;
	process_num = ((zbx_thread_args_t *)args)->process_num;

	zbx_setproctitle("%s #%d starting", get_process_type_string(process_type), process_num);

	zabbix_log(LOG_LEVEL_INFORMATION, "%s #%d started [%s #%d]", get_program_type_string(program_type),
			server_num, get_process_type_string(process_type), process_num);

	if (FAIL == zbx_ipc_service_start(&alerter_service, ZBX_IPC_SERVICE_ALERTER, &error))
	{
		zabbix_log(LOG_LEVEL_CRIT, "cannot start alerter service: %s", error);
		zbx_free(error);
		exit(EXIT_FAILURE);
	}

	DBconnect(ZBX_DB_CONNECT_NORMAL);

	am_init(&manager);

	/* initialize statistics */
	time_stat = zbx_time();
	time_now = time_stat;
	time_idle = 0;
	sent_num = 0;
	failed_num = 0;
	time_db = 0;

	zbx_setproctitle("%s #%d started", get_process_type_string(process_type), process_num);

	update_selfmon_counter(ZBX_PROCESS_STATE_BUSY);

	for (;;)
	{
		time_now = zbx_time();
		now = time_now;

		if (STAT_INTERVAL < time_now - time_stat)
		{
			zbx_setproctitle("%s #%d [sent %d, failed %d alerts, idle " ZBX_FS_DBL " sec during "
					ZBX_FS_DBL " sec]", get_process_type_string(process_type), process_num,
					sent_num, failed_num, time_idle, time_now - time_stat);

			time_stat = time_now;
			time_idle = 0;
			sent_num = 0;
			failed_num = 0;
		}

		zbx_handle_log();

		if (now - time_db >= CONFIG_SENDER_FREQUENCY)
		{
			am_db_queue_alerts(&manager, now);
			am_db_flush_alert_updates(&manager);

			now = time(NULL);
			time_db = now;
		}

		while (SUCCEED == am_check_queue(&manager, now))
		{
			if (NULL == (alerter = zbx_queue_ptr_pop(&manager.free_alerters)))
				break;

			if (FAIL == am_process_alert(&manager, alerter, am_pop_alert(&manager)))
				zbx_queue_ptr_push(&manager.free_alerters, alerter);
		}

		update_selfmon_counter(ZBX_PROCESS_STATE_IDLE);
		ret = zbx_ipc_service_recv(&alerter_service, 1, &client, &message);
		update_selfmon_counter(ZBX_PROCESS_STATE_BUSY);

		if (ZBX_IPC_RECV_IMMEDIATE != ret)
			time_idle += zbx_time() - time_now;

		if (NULL != message)
		{
			switch (message->code)
			{
				case ZBX_IPC_ALERTER_REGISTER:
					am_register_alerter(&manager, client, message);
					break;
				case ZBX_IPC_ALERTER_RESULT:
					if (SUCCEED == am_process_result(&manager, client, message))
						sent_num++;
					else
						failed_num++;
					break;
			}

			zbx_ipc_message_free(message);
		}

		if (NULL != client)
			zbx_ipc_client_release(client);
	}

	zbx_ipc_service_close(&alerter_service);
	am_destroy(&manager);

	DBclose();

	return 0;
}
