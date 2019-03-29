/*
 * FogLAMP storage service.
 *
 * Copyright (c) 2018 OSIsoft, LLC
 *
 * Released under the Apache 2.0 Licence
 *
 * Author: Massimiliano Pinto
 */
#include <connection.h>
#include <connection_manager.h>
#include <common.h>

/*
 * Control the way purge deletes readings. The block size sets a limit as to how many rows
 * get deleted in each call, whilst the sleep interval controls how long the thread sleeps
 * between deletes. The idea is to not keep the database locked too long and allow other threads
 * to have access to the database between blocks.
 */
#define PURGE_SLEEP_MS 500
#define PURGE_DELETE_BLOCK_SIZE	20
#define TARGET_PURGE_BLOCK_DEL_TIME	(70*1000) 	// 70 msec
#define PURGE_BLOCK_SZ_GRANULARITY	5 	// 5 rows
#define MIN_PURGE_DELETE_BLOCK_SIZE	20
#define MAX_PURGE_DELETE_BLOCK_SIZE	1500
#define RECALC_PURGE_BLOCK_SIZE_NUM_BLOCKS	30	// recalculate purge block size after every 30 blocks

#define PURGE_SLOWDOWN_AFTER_BLOCKS 5
#define PURGE_SLOWDOWN_SLEEP_MS 500

#ifndef PLUGIN_LOG_NAME
#define PLUGIN_LOG_NAME "SQLite 3"
#endif

/**
 * SQLite3 storage plugin for FogLAMP
 */

using namespace std;
using namespace rapidjson;

#define CONNECT_ERROR_THRESHOLD		5*60	// 5 minutes

#define MAX_RETRIES			40	// Maximum no. of retries when a lock is encountered
#define RETRY_BACKOFF			100	// Multipler to backoff DB retry on lock

/*
 * The following allows for conditional inclusion of code that tracks the top queries
 * run by the storage plugin and the number of times a particular statement has to
 * be retried because of the database being busy./
 */
#define DO_PROFILE		0
#define DO_PROFILE_RETRIES	0
#if DO_PROFILE
#include <profile.h>

#define	TOP_N_STATEMENTS		10	// Number of statements to report in top n
#define RETRY_REPORT_THRESHOLD		1000	// Report retry statistics every X calls

QueryProfile profiler(TOP_N_STATEMENTS);
unsigned long retryStats[MAX_RETRIES] = { 0,0,0,0,0,0,0,0,0,0 };
unsigned long numStatements = 0;
int	      maxQueue = 0;
#endif

static std::atomic<int> m_waiting(0);
static std::atomic<int> m_writeAccessOngoing(0);
static std::mutex	db_mutex;
static std::condition_variable	db_cv;
static int purgeBlockSize = PURGE_DELETE_BLOCK_SIZE;

#define START_TIME std::chrono::high_resolution_clock::time_point t1 = std::chrono::high_resolution_clock::now();
#define END_TIME std::chrono::high_resolution_clock::time_point t2 = std::chrono::high_resolution_clock::now(); \
				 auto usecs = std::chrono::duration_cast<std::chrono::microseconds>( t2 - t1 ).count();

#define _DB_NAME              "/foglamp.sqlite"

static time_t connectErrorTime = 0;

/**
 * Apply FogLAMP default datetime formatting
 * to a detected DATETIME datatype column
 *
 * @param pStmt    Current SQLite3 result set
 * @param i        Current column index
 * @param          Output parameter for new date
 * @return         True is format has been applied,
 *		   False otherwise
 */
bool Connection::applyColumnDateTimeFormat(sqlite3_stmt *pStmt,
					   int i,
					   string& newDate)
{

	bool apply_format = false;
	string formatStmt = {};

	if (sqlite3_column_database_name(pStmt, i) != NULL &&
	    sqlite3_column_table_name(pStmt, i)    != NULL)
	{

		if ((strcmp(sqlite3_column_origin_name(pStmt, i), "user_ts") == 0) &&
		    (strcmp(sqlite3_column_table_name(pStmt, i), "readings") == 0) &&
		    (strlen((char *) sqlite3_column_text(pStmt, i)) == 32))
		{

			// Extract milliseconds and microseconds for the user_ts field of the readings table
			formatStmt = string("SELECT strftime('");
			formatStmt += string(F_DATEH24_SEC);
			formatStmt += "', '" + string((char *) sqlite3_column_text(pStmt, i));
			formatStmt += "')";
			formatStmt += " || substr('" + string((char *) sqlite3_column_text(pStmt, i));
			formatStmt += "', instr('" + string((char *) sqlite3_column_text(pStmt, i));
			formatStmt += "', '.'), 7)";

			apply_format = true;
		}
		else
		{
			/**
			 * Handle here possible unformatted DATETIME column type
			 * If (column_name == column_original_name) AND
			 * (sqlite3_column_table_name() == "DATETIME")
			 * we assume the column has not been formatted
			 * by any datetime() or strftime() SQLite function.
			 * Thus we apply default FOGLAMP formatting:
			 * "%Y-%m-%d %H:%M:%f"
			 */
			if (sqlite3_column_database_name(pStmt, i) != NULL &&
			    sqlite3_column_table_name(pStmt, i) != NULL &&
			    (strcmp(sqlite3_column_origin_name(pStmt, i),
				    sqlite3_column_name(pStmt, i)) == 0))
			{
				const char *pzDataType;
				int retType = sqlite3_table_column_metadata(dbHandle,
									    sqlite3_column_database_name(pStmt, i),
									    sqlite3_column_table_name(pStmt, i),
									    sqlite3_column_name(pStmt, i),
									    &pzDataType,
									    NULL, NULL, NULL, NULL);

				// Check whether to Apply dateformat
				if (pzDataType != NULL &&
				    retType == SQLITE_OK &&
				    strcmp(pzDataType, SQLITE3_FOGLAMP_DATETIME_TYPE) == 0 &&
				    strcmp(sqlite3_column_origin_name(pStmt, i),
					   sqlite3_column_name(pStmt, i)) == 0)
				{
					// Column metadata found and column datatype is "pzDataType"
					formatStmt = string("SELECT strftime('");
					formatStmt += string(F_DATEH24_MS);
					formatStmt += "', '" + string((char *) sqlite3_column_text(pStmt, i));
					formatStmt += "')";

					apply_format = true;

				}
				else
				{
					// Format not done
					// Just log the error if present
					if (retType != SQLITE_OK)
					{
						Logger::getLogger()->error("SQLite3 failed " \
                                                                "to call sqlite3_table_column_metadata() " \
                                                                "for column '%s'",
									   sqlite3_column_name(pStmt, i));
					}
				}
			}
		}
	}

	if (apply_format)
	{

		char* zErrMsg = NULL;
		// New formatted data
		char formattedData[100] = "";

		// Exec the format SQL
		int rc = SQLexec(dbHandle,
				 formatStmt.c_str(),
				 dateCallback,
				 formattedData,
				 &zErrMsg);

		if (rc == SQLITE_OK )
		{
			// Use new formatted datetime value
			newDate.assign(formattedData);

			return true;
		}
		else
		{
			Logger::getLogger()->error("SELECT dateformat '%s': error %s",
						   formatStmt.c_str(),
						   zErrMsg);

			sqlite3_free(zErrMsg);
		}

	}

	return false;
}

/**
 * Map a SQLite3 result set to a string version of a JSON document
 *
 * @param res          Sqlite3 result set
 * @param resultSet    Output Json as string
 * @return             SQLite3 result code of sqlite3_step(res)
 *
 */
int Connection::mapResultSet(void* res, string& resultSet)
{
// Cast to SQLite3 result set
sqlite3_stmt* pStmt = (sqlite3_stmt *)res;
// JSON generic document
Document doc;
// SQLite3 return code
int rc;
// Number of returned rows, number of columns
unsigned long nRows = 0, nCols = 0;

	// Create the JSON document
	doc.SetObject();
	// Get document allocator
	Document::AllocatorType& allocator = doc.GetAllocator();
	// Create the array for returned rows
	Value rows(kArrayType);
	// Rows counter, set it to 0 now
	Value count;
	count.SetInt(0);

	// Iterate over all the rows in the resultSet
	while ((rc = SQLstep(pStmt)) == SQLITE_ROW)
	{
		// Get number of columns for current row
		nCols = sqlite3_column_count(pStmt);
		// Create the 'row' object
		Value row(kObjectType);

		// Build the row with all fields
		for (int i = 0; i < nCols; i++)
		{
			// JSON document for the current row
			Document d;
			// Set object name as the column name
			Value name(sqlite3_column_name(pStmt, i), allocator);
			// Get the "TEXT" value of the column value
			char* str = (char *)sqlite3_column_text(pStmt, i);

			// Check the column value datatype
			switch (sqlite3_column_type(pStmt, i))
			{
				case (SQLITE_NULL):
				{
					row.AddMember(name, "", allocator);
					break;
				}
				case (SQLITE3_TEXT):
				{

					/**
					 * Handle here possible unformatted DATETIME column type
					 */
					string newDate;
					if (applyColumnDateTimeFormat(pStmt, i, newDate))
					{
						// Use new formatted datetime value
						str = (char *)newDate.c_str();
					}

					Value value;
					if (!d.Parse(str).HasParseError())
					{
						if (d.IsNumber())
						{
							// Set string
							value = Value(str, allocator);
						}
						else
						{
							// JSON parsing ok, use the document
							value = Value(d, allocator);
						}
					}
					else
					{
						// Use (char *) value
						value = Value(str, allocator);
					}
					// Add name & value to the current row
					row.AddMember(name, value, allocator);
					break;
				}
				case (SQLITE_INTEGER):
				{
					int64_t intVal = atol(str);
					// Add name & value to the current row
					row.AddMember(name, intVal, allocator);
					break;
				}
				case (SQLITE_FLOAT):
				{
					double dblVal = atof(str);
					// Add name & value to the current row
					row.AddMember(name, dblVal, allocator);
					break;
				}
				default:
				{
					// Default: use  (char *) value
					Value value(str != NULL ? str : "", allocator);
					// Add name & value to the current row
					row.AddMember(name, value, allocator);
					break;
				}
			}
		}

		// All fields added: increase row counter
		nRows++;

		// Add the current row to the all rows object
		rows.PushBack(row, allocator);
	}

	// All rows added: update rows count
	count.SetInt(nRows);

	// Add 'rows' and 'count' to the final JSON document
	doc.AddMember("count", count, allocator);
	doc.AddMember("rows", rows, allocator);

	/* Write out the JSON document we created */
	StringBuffer buffer;
	Writer<StringBuffer> writer(buffer);
	doc.Accept(writer);

	// Set the result as a CPP string 
	resultSet = buffer.GetString();

	// Return SQLite3 ret code
	return rc;
}

/**
 * Perform a delete against a common table
 *
 */
int Connection::deleteRows(const string& table, const string& condition)
{
// Default template parameter uses UTF8 and MemoryPoolAllocator.
Document document;
SQLBuffer	sql;
 
	sql.append("DELETE FROM foglamp.");
	sql.append(table);
	if (! condition.empty())
	{
		sql.append(" WHERE ");
		if (document.Parse(condition.c_str()).HasParseError())
		{
			raiseError("delete", "Failed to parse JSON payload");
			return -1;
		}
		else
		{
			if (document.HasMember("where"))
			{
				if (!jsonWhereClause(document["where"], sql))
				{
					return -1;
				}
			}
			else
			{
				raiseError("delete",
					   "JSON does not contain where clause");
				return -1;
			}
		}
	}
	sql.append(';');

	const char *query = sql.coalesce();
	logSQL("CommonDelete", query);
	char *zErrMsg = NULL;
	int delete_rows;
	int rc;

	// Exec the DELETE statement: no callback, no result set
	m_writeAccessOngoing.fetch_add(1);
	rc = SQLexec(dbHandle,
		     query,
		     NULL,
		     NULL,
		     &zErrMsg);
	m_writeAccessOngoing.fetch_sub(1);
	if (m_writeAccessOngoing == 0)
		db_cv.notify_all();

	// Check result code
	if (rc == SQLITE_OK)
	{
		// Success. Release memory for 'query' var
		delete[] query;
        	return sqlite3_changes(dbHandle);
	}
	else
	{
 		raiseError("delete", zErrMsg);
		sqlite3_free(zErrMsg);
		Logger::getLogger()->error("SQL statement: %s", query);
		delete[] query;

		// Failure
		return -1;
	}
}

/**
 * Append a set of readings to the readings table
 */
int Connection::appendReadings(const char *readings)
{
// Default template parameter uses UTF8 and MemoryPoolAllocator.
Document 	doc;
SQLBuffer	sql;
int		row = 0;
bool 		add_row = false;

	ParseResult ok = doc.Parse(readings);
	if (!ok)
	{
 		raiseError("appendReadings", GetParseError_En(doc.GetParseError()));
		return -1;
	}

	sql.append("INSERT INTO foglamp.readings ( user_ts, asset_code, read_key, reading ) VALUES ");

	if (!doc.HasMember("readings"))
	{
 		raiseError("appendReadings", "Payload is missing a readings array");
	        return -1;
	}
	Value &rdings = doc["readings"];
	if (!rdings.IsArray())
	{
		raiseError("appendReadings", "Payload is missing the readings array");
		return -1;
	}
	for (Value::ConstValueIterator itr = rdings.Begin(); itr != rdings.End(); ++itr)
	{
		if (!itr->IsObject())
		{
			raiseError("appendReadings",
				   "Each reading in the readings array must be an object");
			return -1;
		}

		add_row = true;

		// Handles - user_ts
		const char *str = (*itr)["user_ts"].GetString();
		if (strcmp(str, "now()") == 0)
		{
			if (row)
			{
				sql.append(", (");
			}
			else
			{
				sql.append('(');
			}

			sql.append(SQLITE3_NOW_READING);
		}
		else
		{
			char formatted_date[LEN_BUFFER_DATE] = {0};
			if (! formatDate(formatted_date, sizeof(formatted_date), str) )
			{
				raiseError("appendReadings", "Invalid date |%s|", str);
				add_row = false;
			}
			else
			{
				if (row)
				{
					sql.append(", (");
				}
				else
				{
					sql.append('(');
				}

				sql.append('\'');
				sql.append(formatted_date);
				sql.append('\'');
			}
		}

		if (add_row)
		{
			row++;

			// Handles - asset_code
			sql.append(",\'");
			sql.append((*itr)["asset_code"].GetString());

			// Handles - read_key
			// Python code is passing the string None when here is no read_key in the payload
			if (itr->HasMember("read_key") && strcmp((*itr)["read_key"].GetString(), "None") != 0)
			{
				sql.append("', \'");
				sql.append((*itr)["read_key"].GetString());
				sql.append("', \'");
			}
			else
			{
				// No "read_key" in this reading, insert NULL
				sql.append("', NULL, '");
			}

			// Handles - reading
			StringBuffer buffer;
			Writer<StringBuffer> writer(buffer);
			(*itr)["reading"].Accept(writer);
			sql.append(buffer.GetString());
			sql.append('\'');

			sql.append(')');
		}

	}
	sql.append(';');

	const char *query = sql.coalesce();
	logSQL("ReadingsAppend", query);
	char *zErrMsg = NULL;
	int rc;

	{
	m_writeAccessOngoing.fetch_add(1);
	unique_lock<mutex> lck(db_mutex);

	// Exec the INSERT statement: no callback, no result set
	rc = SQLexec(dbHandle,
		     query,
		     NULL,
		     NULL,
		     &zErrMsg);

	m_writeAccessOngoing.fetch_sub(1);
	db_cv.notify_all();
	}

	// Release memory for 'query' var
	delete[] query;

	// Check result code
	if (rc == SQLITE_OK)
	{
		// Success
		return sqlite3_changes(dbHandle);
	}
	else
	{
	 	raiseError("appendReadings", zErrMsg);
		sqlite3_free(zErrMsg);

		// Failure
		return -1;
	}
}

/**
 * Fetch a block of readings from the reading table
 * It might not work with SQLite 3
 *
 * Fetch, used by the north side, returns timestamp in UTC.
 *
 * NOTE : it expects to handle a date having a fixed format
 * with milliseconds, microseconds and timezone expressed,
 * like for example :
 *
 *    2019-01-11 15:45:01.123456+01:00
 */
bool Connection::fetchReadings(unsigned long id,
			       unsigned int blksize,
			       std::string& resultSet)
{
char sqlbuffer[512];
char *zErrMsg = NULL;
int rc;
int retrieve;

	// SQL command to extract the data from the foglamp.readings
	const char *sql_cmd = R"(
	SELECT
		id,
		asset_code,
		read_key,
		reading,
		strftime('%%Y-%%m-%%d %%H:%%M:%%S', user_ts, 'utc')  ||
		substr(user_ts, instr(user_ts, '.'), 7) AS user_ts,
		strftime('%%Y-%%m-%%d %%H:%%M:%%f', ts, 'utc') AS ts
	FROM foglamp.readings
	WHERE id >= %lu
	ORDER BY id ASC
	LIMIT %u;
	)";

	/*
	 * This query assumes datetime values are in 'localtime'
	 */
	snprintf(sqlbuffer,
		 sizeof(sqlbuffer),
		 sql_cmd,
		 id,
		 blksize);
	logSQL("ReadingsFetch", sqlbuffer);
	sqlite3_stmt *stmt;
	// Prepare the SQL statement and get the result set
	if (sqlite3_prepare_v2(dbHandle,
			       sqlbuffer,
			       -1,
			       &stmt,
			       NULL) != SQLITE_OK)
	{
		raiseError("retrieve", sqlite3_errmsg(dbHandle));

		// Failure
		return false;
	}
	else
	{
		// Call result set mapping
		rc = mapResultSet(stmt, resultSet);

		// Delete result set
		sqlite3_finalize(stmt);

		// Check result set errors
		if (rc != SQLITE_DONE)
		{
			raiseError("retrieve", sqlite3_errmsg(dbHandle));

			// Failure
			return false;
               	}
		else
		{
			// Success
			return true;
		}
	}
}

/**
 * Perform a query against the readings table
 *
 * retrieveReadings, used by the API, returns timestamp in localtime.
 *
 */
bool Connection::retrieveReadings(const string& condition, string& resultSet)
{
// Default template parameter uses UTF8 and MemoryPoolAllocator.
Document	document;
SQLBuffer	sql;
// Extra constraints to add to where clause
SQLBuffer	jsonConstraints;
bool		isAggregate = false;

	try {
		if (dbHandle == NULL)
		{
			raiseError("retrieve", "No SQLite 3 db connection available");
			return false;
		}

		if (condition.empty())
		{
			const char *sql_cmd = R"(
					SELECT
						id,
						asset_code,
						read_key,
						reading,
						strftime(')" F_DATEH24_SEC R"(', user_ts, 'localtime')  ||
						substr(user_ts, instr(user_ts, '.'), 7) AS user_ts,
						strftime(')" F_DATEH24_MS R"(', ts, 'localtime') AS ts
					FROM foglamp.readings)";

			sql.append(sql_cmd);
		}
		else
		{
			if (document.Parse(condition.c_str()).HasParseError())
			{
				raiseError("retrieve", "Failed to parse JSON payload");
				return false;
			}
			if (document.HasMember("aggregate"))
			{
				isAggregate = true;
				sql.append("SELECT ");
				if (document.HasMember("modifier"))
				{
					sql.append(document["modifier"].GetString());
					sql.append(' ');
				}
				if (!jsonAggregates(document, document["aggregate"], sql, jsonConstraints, true))
				{
					return false;
				}
				sql.append(" FROM foglamp.");
			}
			else if (document.HasMember("return"))
			{
				int col = 0;
				Value& columns = document["return"];
				if (! columns.IsArray())
				{
					raiseError("retrieve", "The property return must be an array");
					return false;
				}
				sql.append("SELECT ");
				if (document.HasMember("modifier"))
				{
					sql.append(document["modifier"].GetString());
					sql.append(' ');
				}
				for (Value::ConstValueIterator itr = columns.Begin(); itr != columns.End(); ++itr)
				{
					if (col)
						sql.append(", ");
					if (!itr->IsObject())	// Simple column name
					{
						if (strcmp(itr->GetString() ,"user_ts") == 0)
						{
							// Display without TZ expression and microseconds also
							sql.append(" strftime('" F_DATEH24_SEC "', user_ts, 'localtime') ");
							sql.append(" || substr(user_ts, instr(user_ts, '.'), 7) ");
							sql.append(" as  user_ts ");
						}
						else if (strcmp(itr->GetString() ,"ts") == 0)
						{
							// Display without TZ expression and microseconds also
							sql.append(" strftime('" F_DATEH24_MS "', ts, 'localtime') ");
							sql.append(" as ts ");
						}
						else
						{
							sql.append(itr->GetString());
						}
					}
					else
					{
						if (itr->HasMember("column"))
						{
							if (! (*itr)["column"].IsString())
							{
								raiseError("retrieve",
									   "column must be a string");
								return false;
							}
							if (itr->HasMember("format"))
							{
								if (! (*itr)["format"].IsString())
								{
									raiseError("retrieve",
										   "format must be a string");
									return false;
								}

								// SQLite 3 date format.
								string new_format;
								applyColumnDateFormatLocaltime((*itr)["format"].GetString(),
										      (*itr)["column"].GetString(),
										      new_format, true);
								// Add the formatted column or use it as is
								sql.append(new_format);
							}
							else if (itr->HasMember("timezone"))
							{
								if (! (*itr)["timezone"].IsString())
								{
									raiseError("retrieve",
										   "timezone must be a string");
									return false;
								}
								// SQLite3 doesnt support time zone formatting
								const char *tz = (*itr)["timezone"].GetString();

								if (strncasecmp(tz, "utc", 3) == 0)
								{
									if (strcmp((*itr)["column"].GetString() ,"user_ts") == 0)
									{
										// Extract milliseconds and microseconds for the user_ts fields

										sql.append("strftime('" F_DATEH24_SEC "', user_ts, 'utc') ");
										sql.append(" || substr(user_ts, instr(user_ts, '.'), 7) ");
										if (! itr->HasMember("alias"))
										{
											sql.append(" AS ");
											sql.append((*itr)["column"].GetString());
										}
									}
									else
									{
										sql.append("strftime('" F_DATEH24_MS "', ");
										sql.append((*itr)["column"].GetString());
										sql.append(", 'utc')");
										if (! itr->HasMember("alias"))
										{
											sql.append(" AS ");
											sql.append((*itr)["column"].GetString());
										}
									}
								}
								else if (strncasecmp(tz, "localtime", 9) == 0)
								{
									if (strcmp((*itr)["column"].GetString() ,"user_ts") == 0)
									{
										// Extract milliseconds and microseconds for the user_ts fields

										sql.append("strftime('" F_DATEH24_SEC "', user_ts, 'localtime') ");
										sql.append(" || substr(user_ts, instr(user_ts, '.'), 7) ");
										if (! itr->HasMember("alias"))
										{
											sql.append(" AS ");
											sql.append((*itr)["column"].GetString());
										}
									}
									else
									{
										sql.append("strftime('" F_DATEH24_MS "', ");
										sql.append((*itr)["column"].GetString());
										sql.append(", 'localtime')");
										if (! itr->HasMember("alias"))
										{
											sql.append(" AS ");
											sql.append((*itr)["column"].GetString());
										}
									}
								}
								else
								{
									raiseError("retrieve",
										   "SQLite3 plugin does not support timezones in queries");
									return false;
								}
							}
							else
							{

								if (strcmp((*itr)["column"].GetString() ,"user_ts") == 0)
								{
									// Extract milliseconds and microseconds for the user_ts fields

									sql.append("strftime('" F_DATEH24_SEC "', user_ts, 'localtime') ");
									sql.append(" || substr(user_ts, instr(user_ts, '.'), 7) ");
									if (! itr->HasMember("alias"))
									{
										sql.append(" AS ");
										sql.append((*itr)["column"].GetString());
									}
								}
								else
								{
									sql.append("strftime('" F_DATEH24_MS "', ");
									sql.append((*itr)["column"].GetString());
									sql.append(", 'localtime')");
									if (! itr->HasMember("alias"))
									{
										sql.append(" AS ");
										sql.append((*itr)["column"].GetString());
									}
								}
							}
							sql.append(' ');
						}
						else if (itr->HasMember("json"))
						{
							const Value& json = (*itr)["json"];
							if (! returnJson(json, sql, jsonConstraints))
								return false;
						}
						else
						{
							raiseError("retrieve",
								   "return object must have either a column or json property");
							return false;
						}

						if (itr->HasMember("alias"))
						{
							sql.append(" AS \"");
							sql.append((*itr)["alias"].GetString());
							sql.append('"');
						}
					}
					col++;
				}
				sql.append(" FROM foglamp.");
			}
			else
			{
				sql.append("SELECT ");
				if (document.HasMember("modifier"))
				{
					sql.append(document["modifier"].GetString());
					sql.append(' ');
				}

				const char *sql_cmd = R"(
						id,
						asset_code,
						read_key,
						reading,
						strftime(')" F_DATEH24_SEC R"(', user_ts, 'localtime')  ||
						substr(user_ts, instr(user_ts, '.'), 7) AS user_ts,
						strftime(')" F_DATEH24_MS R"(', ts, 'localtime') AS ts
					FROM foglamp.)";

				sql.append(sql_cmd);
			}
			sql.append("readings");
			if (document.HasMember("where"))
			{
				sql.append(" WHERE ");
			 
				if (document.HasMember("where"))
				{
					if (!jsonWhereClause(document["where"], sql))
					{
						return false;
					}
				}
				else
				{
					raiseError("retrieve",
						   "JSON does not contain where clause");
					return false;
				}
				if (! jsonConstraints.isEmpty())
				{
					sql.append(" AND ");
                                        const char *jsonBuf =  jsonConstraints.coalesce();
                                        sql.append(jsonBuf);
                                        delete[] jsonBuf;
				}
			}
			else if (isAggregate)
			{
				/*
				 * Performance improvement: force sqlite to use an index
				 * if we are doing an aggregate and have no where clause.
				 */
				sql.append(" WHERE asset_code = asset_code");
			}
			if (!jsonModifiers(document, sql))
			{
				return false;
			}
		}
		sql.append(';');

		const char *query = sql.coalesce();
		char *zErrMsg = NULL;
		int rc;
		sqlite3_stmt *stmt;

		logSQL("ReadingsRetrieve", query);

		// Prepare the SQL statement and get the result set
		rc = sqlite3_prepare_v2(dbHandle, query, -1, &stmt, NULL);

		// Release memory for 'query' var
		delete[] query;

		if (rc != SQLITE_OK)
		{
			raiseError("retrieve", sqlite3_errmsg(dbHandle));
			return false;
		}

		// Call result set mapping
		rc = mapResultSet(stmt, resultSet);

		// Delete result set
		sqlite3_finalize(stmt);

		// Check result set mapping errors
		if (rc != SQLITE_DONE)
		{
			raiseError("retrieve", sqlite3_errmsg(dbHandle));
			// Failure
			return false;
		}
		// Success
		return true;
	} catch (exception e) {
		raiseError("retrieve", "Internal error: %s", e.what());
	}
}

/**
 * Purge readings from the reading table
 */
unsigned int  Connection::purgeReadings(unsigned long age,
					unsigned int flags,
					unsigned long sent,
					std::string& result)
{
long unsentPurged = 0;
long unsentRetained = 0;
long numReadings = 0;
unsigned long rowidLimit = 0, minrowidLimit = 0, maxrowidLimit = 0, rowidMin;
struct timeval startTv, endTv;
int blocks = 0;

	Logger *logger = Logger::getLogger();

	result = "{ \"removed\" : 0, ";
	result += " \"unsentPurged\" : 0, ";
	result += " \"unsentRetained\" : 0, ";
	result += " \"readings\" : 0 }";

	logger->info("Purge starting...");
	gettimeofday(&startTv, NULL);
	/*
	 * We fetch the current rowid and limit the purge process to work on just
	 * those rows present in the database when the purge process started.
	 * This provents us looping in the purge process if new readings become
	 * eligible for purging at a rate that is faster than we can purge them.
	 */
	{
		char *zErrMsg = NULL;
		int rc;
		rc = SQLexec(dbHandle,
		     "select max(rowid) from foglamp.readings;",
	  	     rowidCallback,
		     &rowidLimit,
		     &zErrMsg);

		if (rc != SQLITE_OK)
		{
 			raiseError("purge - phase 0, fetching rowid limit ", zErrMsg);
			sqlite3_free(zErrMsg);
			return 0;
		}
		maxrowidLimit = rowidLimit;
	}

	{
		char *zErrMsg = NULL;
		int rc;
		rc = SQLexec(dbHandle,
		     "select min(rowid) from foglamp.readings;",
	  	     rowidCallback,
		     &minrowidLimit,
		     &zErrMsg);

		if (rc != SQLITE_OK)
		{
 			raiseError("purge - phaase 0, fetching minrowid limit ", zErrMsg);
			sqlite3_free(zErrMsg);
			return 0;
		}
	}

	if (age == 0)
	{
		/*
		 * An age of 0 means remove the oldest hours data.
		 * So set age based on the data we have and continue.
		 */
		SQLBuffer oldest;
		oldest.append("SELECT (strftime('%s','now', 'utc') - strftime('%s', MIN(user_ts)))/360 FROM foglamp.readings where rowid <= ");
		oldest.append(rowidLimit);
		oldest.append(';');
		const char *query = oldest.coalesce();
		char *zErrMsg = NULL;
		int rc;
		int purge_readings = 0;

		// Exec query and get result in 'purge_readings' via 'selectCallback'
		rc = SQLexec(dbHandle,
			     query,
			     selectCallback,
			     &purge_readings,
			     &zErrMsg);
		// Release memory for 'query' var
		delete[] query;

		if (rc == SQLITE_OK)
		{
			age = purge_readings;
		}
		else
		{
 			raiseError("purge - phase 1", zErrMsg);
			sqlite3_free(zErrMsg);
			return 0;
		}
	}

	{
		/*
		 * Refine rowid limit to just those rows older than age hours.
		 */
		char *zErrMsg = NULL;
		int rc;
		unsigned long l = minrowidLimit;
		unsigned long r = ((flags & 0x01) && sent) ? min(sent, rowidLimit) : rowidLimit;
		r = max(r, l);
		//logger->info("%s:%d: l=%u, r=%u, sent=%u, rowidLimit=%u, minrowidLimit=%u, flags=%u", __FUNCTION__, __LINE__, l, r, sent, rowidLimit, minrowidLimit, flags);
		if (l == r)
		{
 			logger->info("No data to purge: min_id == max_id == %u", minrowidLimit);
			return 0;
		}

		unsigned long m=l;

		while (l <= r)
		{
			unsigned long midRowId = 0;
			unsigned long prev_m = m;
		    m = l + (r - l) / 2;
			if (prev_m == m) break;

			// e.g. select id from readings where rowid = 219867307 AND user_ts < datetime('now' , '-24 hours', 'utc');
			SQLBuffer sqlBuffer;
			sqlBuffer.append("select id from foglamp.readings where rowid = ");
			sqlBuffer.append(m);
			sqlBuffer.append(" AND user_ts < datetime('now' , '-");
			sqlBuffer.append(age);
			sqlBuffer.append(" hours');");
			const char *query = sqlBuffer.coalesce();

			rc = SQLexec(dbHandle,
		     query,
	  	     rowidCallback,
		     &midRowId,
		     &zErrMsg);

			if (rc != SQLITE_OK)
			{
	 			raiseError("purge - phase 1, fetching midRowId ", zErrMsg);
				sqlite3_free(zErrMsg);
				return 0;
			}

			if (midRowId == 0) // mid row doesn't satisfy given condition for user_ts, so discard right/later half and look in left/earlier half
			{
				// search in earlier/left half
				r = m - 1;
			}
			else //if (l != m)
			{
				// search in later/right half
		        l = m + 1;
			}
		}

		rowidLimit = m;

		if (minrowidLimit == rowidLimit)
		{
 			logger->info("No data to purge");
			return 0;
		}

		rowidMin = minrowidLimit;
	}
	//logger->info("Purge collecting unsent row count");
	if ((flags & 0x01) == 0)
	{
		char *zErrMsg = NULL;
		int rc;
		int lastPurgedId;
		SQLBuffer idBuffer;
		idBuffer.append("select id from foglamp.readings where rowid = ");
		idBuffer.append(rowidLimit);
		idBuffer.append(';');
		const char *idQuery = idBuffer.coalesce();
		rc = SQLexec(dbHandle,
		     idQuery,
	  	     rowidCallback,
		     &lastPurgedId,
		     &zErrMsg);

		// Release memory for 'idQuery' var
		delete[] idQuery;

		if (rc != SQLITE_OK)
		{
 			raiseError("purge - phase 0, fetching rowid limit ", zErrMsg);
			sqlite3_free(zErrMsg);
			return 0;
		}

		if (sent != 0 && lastPurgedId > sent)	// Unsent readings will be purged
		{
			// Get number of unsent rows we are about to remove
			int unsent = rowidLimit - sent;
			unsentPurged = unsent;
		}
	}
	if (m_writeAccessOngoing)
	{
		while (m_writeAccessOngoing)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}

	unsigned int deletedRows = 0;
	char *zErrMsg = NULL;
	unsigned int rowsAffected, totTime=0, prevBlocks=0, prevTotTime=0;
	logger->info("Purge about to delete readings # %ld to %ld", rowidMin, rowidLimit);
	while (rowidMin < rowidLimit)
	{
		blocks++;
		rowidMin += purgeBlockSize;
		if (rowidMin > rowidLimit)
		{
			rowidMin = rowidLimit;
		}
		SQLBuffer sql;
		sql.append("DELETE FROM foglamp.readings WHERE rowid <= ");
		sql.append(rowidMin);
		sql.append(';');
		const char *query = sql.coalesce();
		logSQL("ReadingsPurge", query);

		int rc;
		{
		unique_lock<mutex> lck(db_mutex);
		if (m_writeAccessOngoing) db_cv.wait(lck);

		START_TIME;
		// Exec DELETE query: no callback, no resultset
		rc = SQLexec(dbHandle,
			     query,
			     NULL,
			     NULL,
			     &zErrMsg);
		END_TIME;

		// Release memory for 'query' var
		delete[] query;

		totTime += usecs;

		if(usecs>150000)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(100+usecs/10000));
		}
		}

		if (rc != SQLITE_OK)
		{
			raiseError("purge - phase 3", zErrMsg);
			sqlite3_free(zErrMsg);
			// Release memory for 'query' var
			delete[] query;
			return 0;
		}

		// Get db changes
		rowsAffected = sqlite3_changes(dbHandle);
		deletedRows += rowsAffected;
		logger->debug("Purge delete block #%d with %d readings", blocks, rowsAffected);

		if(blocks % RECALC_PURGE_BLOCK_SIZE_NUM_BLOCKS == 0)
		{
			int prevAvg = prevTotTime/(prevBlocks?prevBlocks:1);
			int currAvg = (totTime-prevTotTime)/(blocks-prevBlocks);
			int avg = ((prevAvg?prevAvg:currAvg)*5 + currAvg*5) / 10; // 50% weightage for long term avg and 50% weightage for current avg
			prevBlocks = blocks;
			prevTotTime = totTime;
			int deviation = abs(avg - TARGET_PURGE_BLOCK_DEL_TIME);
			logger->debug("blocks=%d, totTime=%d usecs, prevAvg=%d usecs, currAvg=%d usecs, avg=%d usecs, TARGET_PURGE_BLOCK_DEL_TIME=%d usecs, deviation=%d usecs", 
							blocks, totTime, prevAvg, currAvg, avg, TARGET_PURGE_BLOCK_DEL_TIME, deviation);
			if (deviation > TARGET_PURGE_BLOCK_DEL_TIME/10)
			{
				float ratio = (float)TARGET_PURGE_BLOCK_DEL_TIME / (float)avg;
				if (ratio > 2.0) ratio = 2.0;
				if (ratio < 0.5) ratio = 0.5;
				purgeBlockSize = (float)purgeBlockSize * ratio;
				purgeBlockSize = purgeBlockSize / PURGE_BLOCK_SZ_GRANULARITY * PURGE_BLOCK_SZ_GRANULARITY;
				if (purgeBlockSize < MIN_PURGE_DELETE_BLOCK_SIZE)
					purgeBlockSize = MIN_PURGE_DELETE_BLOCK_SIZE;
				if (purgeBlockSize > MAX_PURGE_DELETE_BLOCK_SIZE)
					purgeBlockSize = MAX_PURGE_DELETE_BLOCK_SIZE;
				logger->debug("Changed purgeBlockSize to %d", purgeBlockSize);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
		//Logger::getLogger()->debug("Purge delete block #%d with %d readings", blocks, rowsAffected);
	} while (rowidMin  < rowidLimit);

	unsentRetained = maxrowidLimit - rowidLimit;

	numReadings = maxrowidLimit - minrowidLimit - deletedRows;

	if (sent == 0)	// Special case when not north process is used
	{
		unsentPurged = deletedRows;
	}

	ostringstream convert;

	convert << "{ \"removed\" : " << deletedRows << ", ";
	convert << " \"unsentPurged\" : " << unsentPurged << ", ";
	convert << " \"unsentRetained\" : " << unsentRetained << ", ";
    convert << " \"readings\" : " << numReadings << " }";

	result = convert.str();

	//logger->debug("Purge result=%s", result.c_str());

	gettimeofday(&endTv, NULL);
	unsigned long duration = (1000000 * (endTv.tv_sec - startTv.tv_sec)) + endTv.tv_usec - startTv.tv_usec;
	logger->info("Purge process complete in %d blocks in %lduS", blocks, duration);

	return deletedRows;
}

#ifdef AAAA
/**
 * Process the aggregate options and return the columns to be selected
 */
bool Connection::jsonAggregates(const Value& payload,
				const Value& aggregates,
				SQLBuffer& sql,
				SQLBuffer& jsonConstraint,
				bool isTableReading)
{
	if (aggregates.IsObject())
	{
		if (! aggregates.HasMember("operation"))
		{
			raiseError("Select aggregation",
				   "Missing property \"operation\"");
			return false;
		}
		if ((! aggregates.HasMember("column")) && (! aggregates.HasMember("json")))
		{
			raiseError("Select aggregation",
				   "Missing property \"column\" or \"json\"");
			return false;
		}
		sql.append(aggregates["operation"].GetString());
		sql.append('(');
		if (aggregates.HasMember("column"))
		{
			string col = aggregates["column"].GetString();
			if (col.compare("*") == 0)	// Faster to count ROWID rather than *
			{
				sql.append("ROWID");
			}
			else
			{
				// an operation different from the 'count' is requested
				if (isTableReading && (col.compare("user_ts") == 0) )
				{
					sql.append("strftime('" F_DATEH24_SEC "', user_ts, 'localtime') ");
					sql.append(" || substr(user_ts, instr(user_ts, '.'), 7) ");
				}
				else
				{
					sql.append("\"");
					sql.append(col);
					sql.append("\"");
				}
			}
		}
		else if (aggregates.HasMember("json"))
		{
			const Value& json = aggregates["json"];
			if (! json.IsObject())
			{
				raiseError("Select aggregation",
					   "The json property must be an object");
				return false;
			}

			if (!json.HasMember("column"))
			{
				raiseError("retrieve",
					   "The json property is missing a column property");
				return false;
			}
			// Use json_extract(field, '$.key1.key2') AS value
			sql.append("json_extract(");
			sql.append(json["column"].GetString());
			sql.append(", '$.");

			if (!json.HasMember("properties"))
			{
				raiseError("retrieve",
					   "The json property is missing a properties property");
				return false;
			}
			const Value& jsonFields = json["properties"];

			if (jsonFields.IsArray())
			{
				if (! jsonConstraint.isEmpty())
				{
					jsonConstraint.append(" AND ");
				}
				// JSON1 SQLite3 extension 'json_type' object check:
				// json_type(field, '$.key1.key2') IS NOT NULL
				// Build the Json keys NULL check
				jsonConstraint.append("json_type(");
				jsonConstraint.append(json["column"].GetString());
				jsonConstraint.append(", '$.");

				int field = 0;
				string prev;
				for (Value::ConstValueIterator itr = jsonFields.Begin(); itr != jsonFields.End(); ++itr)
				{
					if (field)
					{
						sql.append(".");
					}
					if (prev.length() > 0)
					{
						// Append Json field for NULL check
						jsonConstraint.append(prev);
						jsonConstraint.append(".");
					}
					prev = itr->GetString();
					field++;
					// Append Json field for query
					sql.append(itr->GetString());
				}
				// Add last Json key
				jsonConstraint.append(prev);

				// Add condition for all json keys not null
				jsonConstraint.append("') IS NOT NULL");
			}
			else
			{
				// Append Json field for query
				sql.append(jsonFields.GetString());

				if (! jsonConstraint.isEmpty())
				{
					jsonConstraint.append(" AND ");
				}
				// JSON1 SQLite3 extension 'json_type' object check:
				// json_type(field, '$.key1.key2') IS NOT NULL
				// Build the Json key NULL check
				jsonConstraint.append("json_type(");
				jsonConstraint.append(json["column"].GetString());
				jsonConstraint.append(", '$.");
				jsonConstraint.append(jsonFields.GetString());

				// Add condition for json key not null
				jsonConstraint.append("') IS NOT NULL");
			}
			sql.append("')");
		}
		sql.append(") AS \"");
		if (aggregates.HasMember("alias"))
		{
			sql.append(aggregates["alias"].GetString());
		}
		else
		{
			sql.append(aggregates["operation"].GetString());
			sql.append('_');
			sql.append(aggregates["column"].GetString());
		}
		sql.append("\"");
	}
	else if (aggregates.IsArray())
	{
		int index = 0;
		for (Value::ConstValueIterator itr = aggregates.Begin(); itr != aggregates.End(); ++itr)
		{
			if (!itr->IsObject())
			{
				raiseError("select aggregation",
						"Each element in the aggregate array must be an object");
				return false;
			}
			if ((! itr->HasMember("column")) && (! itr->HasMember("json")))
			{
				raiseError("Select aggregation", "Missing property \"column\"");
				return false;
			}
			if (! itr->HasMember("operation"))
			{
				raiseError("Select aggregation", "Missing property \"operation\"");
				return false;
			}
			if (index)
				sql.append(", ");
			index++;
			sql.append((*itr)["operation"].GetString());
			sql.append('(');
			if (itr->HasMember("column"))
			{
				string column_name= (*itr)["column"].GetString();
				if (isTableReading && (column_name.compare("user_ts") == 0) )
				{
					sql.append("strftime('" F_DATEH24_SEC "', user_ts, 'localtime') ");
					sql.append(" || substr(user_ts, instr(user_ts, '.'), 7) ");
				}
				else
				{
					sql.append("\"");
					sql.append(column_name);
					sql.append("\"");
				}

			}
			else if (itr->HasMember("json"))
			{
				const Value& json = (*itr)["json"];
				if (! json.IsObject())
				{
					raiseError("Select aggregation", "The json property must be an object");
					return false;
				}
				if (!json.HasMember("column"))
				{
					raiseError("retrieve", "The json property is missing a column property");
					return false;
				}
				if (!json.HasMember("properties"))
				{
					raiseError("retrieve", "The json property is missing a properties property");
					return false;
				}
				const Value& jsonFields = json["properties"];
				if (! jsonConstraint.isEmpty())
				{
					jsonConstraint.append(" AND ");
				}
				// Use json_extract(field, '$.key1.key2') AS value
				sql.append("json_extract(");
				sql.append(json["column"].GetString());
				sql.append(", '$.");

				// JSON1 SQLite3 extension 'json_type' object check:
				// json_type(field, '$.key1.key2') IS NOT NULL
				// Build the Json keys NULL check
				jsonConstraint.append("json_type(");
				jsonConstraint.append(json["column"].GetString());
				jsonConstraint.append(", '$.");

				if (jsonFields.IsArray())
				{
					string prev;
					for (Value::ConstValueIterator itr = jsonFields.Begin(); itr != jsonFields.End(); ++itr)
					{
						if (prev.length() > 0)
						{
							jsonConstraint.append(prev);
							jsonConstraint.append('.');
							sql.append('.');
						}
						// Append Json field for query
						sql.append(itr->GetString());
						prev = itr->GetString();
					}
					// Add last Json key
					jsonConstraint.append(prev);

					// Add condition for json key not null
					jsonConstraint.append("') IS NOT NULL");
				}
				else
				{
					// Append Json field for query
					sql.append(jsonFields.GetString());

					// JSON1 SQLite3 extension 'json_type' object check:
					// json_type(field, '$.key1.key2') IS NOT NULL
					// Build the Json key NULL check
					jsonConstraint.append(jsonFields.GetString());

					// Add condition for json key not null
					jsonConstraint.append("') IS NOT NULL");
				}
				sql.append("')");
			}
			sql.append(") AS \"");
			if (itr->HasMember("alias"))
			{
				sql.append((*itr)["alias"].GetString());
			}
			else
			{
				sql.append((*itr)["operation"].GetString());
				sql.append('_');
				sql.append((*itr)["column"].GetString());
			}
			sql.append("\"");
		}
	}
	if (payload.HasMember("group"))
	{
		sql.append(", ");
		if (payload["group"].IsObject())
		{
			const Value& grp = payload["group"];
			
			if (grp.HasMember("format"))
			{
				// SQLite 3 date format.
				string new_format;
				applyColumnDateFormat(grp["format"].GetString(),
						      grp["column"].GetString(),
						      new_format);
				// Add the formatted column or use it as is
				sql.append(new_format);
			}
			else
			{
				sql.append(grp["column"].GetString());
			}

			if (grp.HasMember("alias"))
			{
				sql.append(" AS \"");
				sql.append(grp["alias"].GetString());
				sql.append("\"");
			}
			else
			{
				sql.append(" AS \"");
				sql.append(grp["column"].GetString());
				sql.append("\"");
			}
		}
		else
		{
			sql.append(payload["group"].GetString());
		}
	}
	if (payload.HasMember("timebucket"))
	{
		const Value& tb = payload["timebucket"];
		if (! tb.IsObject())
		{
			raiseError("Select data",
				   "The \"timebucket\" property must be an object");
			return false;
		}
		if (! tb.HasMember("timestamp"))
		{
			raiseError("Select data",
				   "The \"timebucket\" object must have a timestamp property");
			return false;
		}

		if (tb.HasMember("format"))
		{
			// SQLite 3 date format is limited.
			string new_format;
			if (applyDateFormat(tb["format"].GetString(),
					    new_format))
			{
				sql.append(", ");
				// Add the formatted column
				sql.append(new_format);

				if (tb.HasMember("size"))
				{
					// Use JulianDay, with microseconds
					sql.append(tb["size"].GetString());
					sql.append(" * round(");
					sql.append("strftime('%J', ");
					sql.append(tb["timestamp"].GetString());
					sql.append(") / ");
					sql.append(tb["size"].GetString());
					sql.append(", 6)");
				}
				else
				{
					sql.append(tb["timestamp"].GetString());
				}
				sql.append(")");
			}
			else
			{
				/**
				 * No date format found: we should return an error.
				 * Note: currently if input Json payload has no 'result' member
				 * raiseError() results in no data being sent to the client
				 * For the time being we just use JulianDay, with microseconds
				 */
				sql.append(", datetime(");
				if (tb.HasMember("size"))
				{
					sql.append(tb["size"].GetString());
					sql.append(" * round(");
				}
				// Use JulianDay, with microseconds
				sql.append("strftime('%J', ");
				sql.append(tb["timestamp"].GetString());
				if (tb.HasMember("size"))
				{
					sql.append(") / ");
					sql.append(tb["size"].GetString());
					sql.append(", 6)");
				}
				else
				{
					sql.append(")");
				}
				sql.append(")");
			}
		}
		else
		{
			sql.append(", datetime(");
			if (tb.HasMember("size"))
			{
				sql.append(tb["size"].GetString());
				sql.append(" * round(");
			}

			/*
			 * Default format when no format is specified:
			 * - we use JulianDay in order to get milliseconds.
			 */
			sql.append("strftime('%J', ");
			sql.append(tb["timestamp"].GetString());
			if (tb.HasMember("size"))
			{
				sql.append(") / ");
				sql.append(tb["size"].GetString());
				sql.append(", 6)");
			}
			else
			{
				sql.append(")");
			}

			sql.append(")");
		}

		sql.append(" AS \"");
		if (tb.HasMember("alias"))
		{
			sql.append(tb["alias"].GetString());
		}
		else
		{
			sql.append("timestamp");
		}
		sql.append('"');
	}
	return true;
}
#endif

#ifdef AAAA
/**
 * Process the modifers for limit, skip, sort and group
 */
bool Connection::jsonModifiers(const Value& payload, SQLBuffer& sql)
{
	if (payload.HasMember("timebucket") && payload.HasMember("sort"))
	{
		raiseError("query modifiers",
			   "Sort and timebucket modifiers can not be used in the same payload");
		return false;
	}

	if (payload.HasMember("group"))
	{
		sql.append(" GROUP BY ");
		if (payload["group"].IsObject())
		{
			const Value& grp = payload["group"];
			if (grp.HasMember("format"))
			{
				/**
				 * SQLite 3 date format is limited.
				 * Handle all availables formats here.
				 */
				string new_format;
				applyColumnDateFormat(grp["format"].GetString(),
						      grp["column"].GetString(),
						      new_format);
				// Add the formatted column or use it as is
				sql.append(new_format);
			}
		}
		else
		{
			sql.append(payload["group"].GetString());
		}
	}

	if (payload.HasMember("sort"))
	{
		sql.append(" ORDER BY ");
		const Value& sortBy = payload["sort"];
		if (sortBy.IsObject())
		{
			if (! sortBy.HasMember("column"))
			{
				raiseError("Select sort", "Missing property \"column\"");
				return false;
			}
			sql.append(sortBy["column"].GetString());
			sql.append(' ');
			if (! sortBy.HasMember("direction"))
			{
				sql.append("ASC");
			}
			else
			{
				sql.append(sortBy["direction"].GetString());
			}
		}
		else if (sortBy.IsArray())
		{
			int index = 0;
			for (Value::ConstValueIterator itr = sortBy.Begin(); itr != sortBy.End(); ++itr)
			{
				if (!itr->IsObject())
				{
					raiseError("select sort",
						   "Each element in the sort array must be an object");
					return false;
				}
				if (! itr->HasMember("column"))
				{
					raiseError("Select sort", "Missing property \"column\"");
					return false;
				}
				if (index)
				{
					sql.append(", ");
				}
				index++;
				sql.append((*itr)["column"].GetString());
				sql.append(' ');
				if (! itr->HasMember("direction"))
				{
					 sql.append("ASC");
				}
				else
				{
					sql.append((*itr)["direction"].GetString());
				}
			}
		}
	}

	if (payload.HasMember("timebucket"))
	{
		const Value& tb = payload["timebucket"];
		if (! tb.IsObject())
		{
			raiseError("Select data",
				   "The \"timebucket\" property must be an object");
			return false;
		}
		if (! tb.HasMember("timestamp"))
		{
			raiseError("Select data",
				   "The \"timebucket\" object must have a timestamp property");
			return false;
		}
		if (payload.HasMember("group"))
		{
			sql.append(", ");
		}
		else
		{
			sql.append(" GROUP BY ");
		}

		// Use DateTime Y-m-d H:M:S from JulianDay
                sql.append("datetime(strftime('%J', ");
                sql.append(tb["timestamp"].GetString());
                sql.append("))");

		sql.append(" ORDER BY ");

		// Use DateTime Y-m-d H:M:S fromt JulianDay
                sql.append("datetime(strftime('%J', ");
                sql.append(tb["timestamp"].GetString());
                sql.append("))");

		sql.append(" DESC");
	}

	if (payload.HasMember("limit"))
	{
		if (!payload["limit"].IsInt())
		{
			raiseError("limit",
				   "Limit must be specfied as an integer");
			return false;
		}
		sql.append(" LIMIT ");
		try {
			sql.append(payload["limit"].GetInt());
		} catch (exception e) {
			raiseError("limit",
				   "Bad value for limit parameter: %s",
				   e.what());
			return false;
		}
	}

	// OFFSET must go after LIMIT
	if (payload.HasMember("skip"))
	{
		// Add no limits
		if (!payload.HasMember("limit"))
		{
			sql.append(" LIMIT -1");
		}

		if (!payload["skip"].IsInt())
		{
			raiseError("skip",
				   "Skip must be specfied as an integer");
			return false;
		}
		sql.append(" OFFSET ");
		sql.append(payload["skip"].GetInt());
	}
	return true;
}
#endif

#ifdef AAAA
/**
 * Convert a JSON where clause into a SQLite3 where clause
 *
 */
bool Connection::jsonWhereClause(const Value& whereClause,
				 SQLBuffer& sql, bool convertLocaltime)
{
	if (!whereClause.IsObject())
	{
		raiseError("where clause", "The \"where\" property must be a JSON object");
		return false;
	}
	if (!whereClause.HasMember("column"))
	{
		raiseError("where clause", "The \"where\" object is missing a \"column\" property");
		return false;
	}
	if (!whereClause.HasMember("condition"))
	{
		raiseError("where clause", "The \"where\" object is missing a \"condition\" property");
		return false;
	}
	if (!whereClause.HasMember("value"))
	{
		raiseError("where clause",
			   "The \"where\" object is missing a \"value\" property");
		return false;
	}

	sql.append(whereClause["column"].GetString());
	sql.append(' ');
	string cond = whereClause["condition"].GetString();
	if (!cond.compare("older"))
	{
		if (!whereClause["value"].IsInt())
		{
			raiseError("where clause",
				   "The \"value\" of an \"older\" condition must be an integer");
			return false;
		}
		sql.append("< datetime('now', '-");
		sql.append(whereClause["value"].GetInt());
		if (convertLocaltime)
			sql.append(" seconds', 'localtime')"); // Get value in localtime
		else
			sql.append(" seconds')"); // Get value in UTC by asking for no timezone
	}
	else if (!cond.compare("newer"))
	{
		if (!whereClause["value"].IsInt())
		{
			raiseError("where clause",
				   "The \"value\" of an \"newer\" condition must be an integer");
			return false;
		}
		sql.append("> datetime('now', '-");
		sql.append(whereClause["value"].GetInt());
		if (convertLocaltime)
			sql.append(" seconds', 'localtime')"); // Get value in localtime
		else
			sql.append(" seconds')"); // Get value in UTC by asking for no timezone
	}
	else if (!cond.compare("in") || !cond.compare("not in"))
	{
		// Check we have a non empty array
		if (whereClause["value"].IsArray() &&
		    whereClause["value"].Size())
		{
			sql.append(cond);
			sql.append(" ( ");
			int field = 0;
			for (Value::ConstValueIterator itr = whereClause["value"].Begin();
							itr != whereClause["value"].End();
							++itr)
			{
				if (field)
				{
					sql.append(", ");
				}
				field++;
				if (itr->IsNumber())
				{
					if (itr->IsInt())
					{
						sql.append(itr->GetInt());
					}
					else if (itr->IsInt64())
					{
						sql.append((long)itr->GetInt64());
					}
					else
					{
						sql.append(itr->GetDouble());
					}
				}
				else if (itr->IsString())
				{
					sql.append('\'');
					sql.append(escape(itr->GetString()));
					sql.append('\'');
				}
				else
				{
					string message("The \"value\" of a \"" + \
							cond + \
							"\" condition array element must be " \
							"a string, integer or double.");
					raiseError("where clause", message.c_str());
					return false;
				}
			}
			sql.append(" )");
		}
		else
		{
			string message("The \"value\" of a \"" + \
					cond + "\" condition must be an array " \
					"and must not be empty.");
			raiseError("where clause", message.c_str());
			return false;
		}
	}
	else
	{
		sql.append(cond);
		sql.append(' ');
		if (whereClause["value"].IsInt())
		{
			sql.append(whereClause["value"].GetInt());
		} else if (whereClause["value"].IsString())
		{
			sql.append('\'');
			sql.append(escape(whereClause["value"].GetString()));
			sql.append('\'');
		}
	}
 
	if (whereClause.HasMember("and"))
	{
		sql.append(" AND ");
		if (!jsonWhereClause(whereClause["and"], sql, convertLocaltime))
		{
			return false;
		}
	}
	if (whereClause.HasMember("or"))
	{
		sql.append(" OR ");
		if (!jsonWhereClause(whereClause["or"], sql, convertLocaltime))
		{
			return false;
		}
	}

	return true;
}
#endif

#ifdef AAAA
/**
 * This routine uses SQLit3 JSON1 extension functions
 */
bool Connection::returnJson(const Value& json,
			    SQLBuffer& sql,
			    SQLBuffer& jsonConstraint)
{
	if (! json.IsObject())
	{
		raiseError("retrieve", "The json property must be an object");
		return false;
	}
	if (!json.HasMember("column"))
	{
		raiseError("retrieve", "The json property is missing a column property");
		return false;
	}
	// Call JSON1 SQLite3 extension routine 'json_extract'
	// json_extract(field, '$.key1.key2') AS value
	sql.append("json_extract(");
	sql.append(json["column"].GetString());
	sql.append(", '$.");
	if (!json.HasMember("properties"))
	{
		raiseError("retrieve", "The json property is missing a properties property");
		return false;
	}
	const Value& jsonFields = json["properties"];
	if (jsonFields.IsArray())
	{
		if (! jsonConstraint.isEmpty())
		{
			jsonConstraint.append(" AND ");
		}
		// JSON1 SQLite3 extension 'json_type' object check:
		// json_type(field, '$.key1.key2') IS NOT NULL
		// Build the Json keys NULL check
		jsonConstraint.append("json_type(");
		jsonConstraint.append(json["column"].GetString());
		jsonConstraint.append(", '$.");
		int field = 0;
		string prev;
		for (Value::ConstValueIterator itr = jsonFields.Begin(); itr != jsonFields.End(); ++itr)
		{
			if (field)
			{
				sql.append(".");
			}
			if (prev.length())
			{
				jsonConstraint.append(prev);
				jsonConstraint.append(".");
			}
			field++;
			// Append Json field for query
			sql.append(itr->GetString());
			prev = itr->GetString();
		}
		// Add last Json key
		jsonConstraint.append(prev);

		// Add condition for all json keys not null
		jsonConstraint.append("') IS NOT NULL");
	}
	else
	{
		// Append Json field for query
		sql.append(jsonFields.GetString());
		if (! jsonConstraint.isEmpty())
		{
			jsonConstraint.append(" AND ");
		}
		// JSON1 SQLite3 extension 'json_type' object check:
		// json_type(field, '$.key1.key2') IS NOT NULL
		// Build the Json key NULL check
		jsonConstraint.append("json_type(");
		jsonConstraint.append(json["column"].GetString());
		jsonConstraint.append(", '$.");
		jsonConstraint.append(jsonFields.GetString());

		// Add condition for json key not null
		jsonConstraint.append("') IS NOT NULL");
	}
	sql.append("') ");

	return true;
}
#endif
