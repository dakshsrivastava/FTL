/* Pi-hole: A black hole for Internet advertisements
*  (c) 2019 Pi-hole, LLC (https://pi-hole.net)
*  Network-wide ad blocking via your own hardware.
*
*  FTL Engine
*  API Implementation /api/dns
*
*  This file is copyright under the latest version of the EUPL.
*  Please see LICENSE file for your rights under this license. */

#include "FTL.h"
#include "http-common.h"
#include "routes.h"
#include "json_macros.h"
#include "database/gravity-db.h"
#include "log.h"
// {s,g}et_blockingstatus()
#include "setupVars.h"
// floor()
#include <math.h>
// set_blockingmode_timer()
#include "timers.h"

int api_dns_status(struct mg_connection *conn)
{
	int method = http_method(conn);
	if(method == HTTP_GET)
	{
		// Return current status
		cJSON *json = JSON_NEW_OBJ();
		JSON_OBJ_REF_STR(json, "status", (get_blockingstatus() ? "enabled" : "disabled"));
		JSON_SEND_OBJECT(json);
	}
	else if(method == HTTP_POST)
	{
		// Verify requesting client is allowed to access this ressource
		if(check_client_auth(conn) < 0)
		{
			return send_json_unauthorized(conn, NULL);
		}

		char buffer[1024];
		int data_len = mg_read(conn, buffer, sizeof(buffer) - 1);
		if ((data_len < 1) || (data_len >= (int)sizeof(buffer))) {
			return send_json_error(conn, 400,
			                       "bad_request", "No request body data",
                                               NULL, NULL);
		}
		buffer[data_len] = '\0';

		cJSON *obj = cJSON_Parse(buffer);
		if (obj == NULL) {
			return send_json_error(conn, 400,
                                               "bad_request",
                                               "Invalid request body data",
                                                NULL, NULL);
		}

		cJSON *elem1 = cJSON_GetObjectItemCaseSensitive(obj, "action");
		if (!cJSON_IsString(elem1)) {
			cJSON_Delete(obj);
			return send_json_error(conn, 400,
			                       "bad_request",
                                               "No \"action\" string in body data",
                                               NULL, NULL);
		}
		const char *action = elem1->valuestring;

		unsigned int delay = -1;
		cJSON *elem2 = cJSON_GetObjectItemCaseSensitive(obj, "time");
		if (cJSON_IsNumber(elem2) && elem2->valuedouble > 0.0)
		{
			delay = elem2->valueint;
		}

		cJSON *json = JSON_NEW_OBJ();
		if(strcmp(action, "enable") == 0)
		{
			cJSON_Delete(obj);
			JSON_OBJ_REF_STR(json, "key", "enabled");
			// If no "time" key was present, we call this subroutine with
			// delay == -1 which will disable all previously set timers
			set_blockingmode_timer(delay, false);
			set_blockingstatus(true);
		}
		else if(strcmp(action, "disable") == 0)
		{
			cJSON_Delete(obj);
			JSON_OBJ_REF_STR(json, "key", "disabled");
			// If no "time" key was present, we call this subroutine with
			// delay == -1 which will disable all previously set timers
			set_blockingmode_timer(delay, true);
			set_blockingstatus(false);
		}
		else
		{
			cJSON_Delete(obj);
			return send_json_error(conn, 400,
			                       "bad_request",
                                               "Invalid \"action\" requested",
                                               NULL, NULL);
		}
		JSON_SEND_OBJECT(json);
	}
	else
	{
		// This results in error 404
		return 0;
	}
}

static int api_dns_somelist_read(struct mg_connection *conn, bool exact, bool whitelist)
{
	cJSON *json = JSON_NEW_ARRAY();
	const char *domain = NULL;

	int table;
	if(whitelist)
		if(exact)
			table = EXACT_WHITELIST_TABLE;
		else
			table = REGEX_WHITELIST_TABLE;
	else
		if(exact)
			table = EXACT_BLACKLIST_TABLE;
		else
			table = REGEX_BLACKLIST_TABLE;

	gravityDB_getTable(table);
	while((domain = gravityDB_getDomain()) != NULL)
	{
		JSON_ARRAY_COPY_STR(json, domain);
	}
	gravityDB_finalizeTable();

	JSON_SEND_OBJECT(json);
}

static int api_dns_somelist_POST(struct mg_connection *conn,
                                   bool store_exact,
                                   bool whitelist)
{
	char buffer[1024];
	int data_len = mg_read(conn, buffer, sizeof(buffer) - 1);
	if ((data_len < 1) || (data_len >= (int)sizeof(buffer))) {
		return send_json_error(conn, 400,
                                       "bad_request", "No request body data",
                                       NULL, NULL);
	}
	buffer[data_len] = '\0';

	cJSON *obj = cJSON_Parse(buffer);
	if (obj == NULL) {
		return send_json_error(conn, 400,
                                       "bad_request",
                                       "Invalid request body data",
                                       NULL, NULL);
	}

	cJSON *elem = cJSON_GetObjectItemCaseSensitive(obj, "domain");

	if (!cJSON_IsString(elem)) {
		cJSON_Delete(obj);
		return send_json_error(conn, 400,
                                       "bad_request",
                                       "No \"domain\" string in body data",
                                       NULL, NULL);
	}
	const char *domain = elem->valuestring;

	const char *table;
	if(whitelist)
		if(store_exact)
			table = "whitelist";
		else
			table = "regex_whitelist";
	else
		if(store_exact)
			table = "blacklist";
		else
			table = "regex_blacklist";

	cJSON *json = JSON_NEW_OBJ();
	if(gravityDB_addToTable(table, domain))
	{
		JSON_OBJ_REF_STR(json, "key", "added");
		JSON_OBJ_COPY_STR(json, "domain", domain);
		cJSON_Delete(obj);
		JSON_SEND_OBJECT(json);
	}
	else
	{
		JSON_OBJ_COPY_STR(json, "domain", domain);
		cJSON_Delete(obj);
		return send_json_error(conn, 500,
                                       "database_error",
                                       "Could not add domain to database table",
                                       json, NULL);
	}
}

static int api_dns_somelist_DELETE(struct mg_connection *conn,
                                   bool store_exact,
                                   bool whitelist)
{
	const struct mg_request_info *request = mg_get_request_info(conn);

	char domain[1024];
	// Advance one character to strip "/"
	const char *encoded_uri = strrchr(request->local_uri, '/')+1u;
	// Decode URL (necessar for regular expressions, harmless for domains)
	mg_url_decode(encoded_uri, strlen(encoded_uri), domain, sizeof(domain)-1u, 0);

	const char *table;
	if(whitelist)
		if(store_exact)
			table = "whitelist";
		else
			table = "regex_whitelist";
	else
		if(store_exact)
			table = "blacklist";
		else
			table = "regex_blacklist";

	cJSON *json = JSON_NEW_OBJ();
	if(gravityDB_delFromTable(table, domain))
	{
		JSON_OBJ_REF_STR(json, "key", "removed");
		JSON_OBJ_REF_STR(json, "domain", domain);
		JSON_SEND_OBJECT(json);
	}
	else
	{
		JSON_OBJ_REF_STR(json, "domain", domain);
		return send_json_error(conn, 500,
                                       "database_error",
                                       "Could not remove domain from database table",
                                       json, NULL);
	}
}

int api_dns_somelist(struct mg_connection *conn, bool exact, bool whitelist)
{
	// Verify requesting client is allowed to see this ressource
	if(check_client_auth(conn) < 0)
	{
		return send_json_unauthorized(conn, NULL);
	}

	int method = http_method(conn);
	if(method == HTTP_GET)
	{
		return api_dns_somelist_read(conn, exact, whitelist);
	}
	else if(method == HTTP_POST)
	{
		// Add domain from exact white-/blacklist when a user sends
		// the request to the general address /api/dns/{white,black}list
		return api_dns_somelist_POST(conn, exact, whitelist);
	}
	else if(method == HTTP_DELETE)
	{
		// Delete domain from exact white-/blacklist when a user sends
		// the request to the general address /api/dns/{white,black}list
		return api_dns_somelist_DELETE(conn, exact, whitelist);
	}
	else
	{
		// This results in error 404
		return 0;
	}
}
