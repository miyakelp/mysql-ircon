/* Copyright (c) 2004, 2016, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file ha_ircon.cc

  @brief
  The ha_ircon engine is a stubbed storage engine for ircon purposes only;
  it does nothing at this point. Its purpose is to provide a source
  code illustration of how to begin writing new storage engines; see also
  /storage/ircon/ha_ircon.h.

  @details
  ha_ircon will let you create/open/delete tables, but
  nothing further (for ircon, indexes are not supported nor can data
  be stored in the table). Use this ircon as a template for
  implementing the same functionality in your own storage engine. You
  can enable the ircon storage engine in your build by doing the
  following during your build process:<br> ./configure
  --with-ircon-storage-engine

  Once this is done, MySQL will let you create tables with:<br>
  CREATE TABLE <table name> (...) ENGINE=IRCON;

  The ircon storage engine is set up to use table locks. It
  implements an ircon "SHARE" that is inserted into a hash by table
  name. You can use this to store information of state that any
  ircon handler object will be able to see when it is using that
  table.

  Please read the object definition in ha_ircon.h before reading the rest
  of this file.

  @note
  When you create an IRCON table, the MySQL Server creates a table .frm
  (format) file in the database directory, using the table name as the file
  name as is customary with MySQL. No other files are created. To get an idea
  of what occurs, here is an ircon select that would do a scan of an entire
  table:

  @code
  ha_ircon::store_lock
  ha_ircon::external_lock
  ha_ircon::info
  ha_ircon::rnd_init
  ha_ircon::extra
  ENUM HA_EXTRA_CACHE        Cache record in HA_rrnd()
  ha_ircon::rnd_next
  ha_ircon::rnd_next
  ha_ircon::rnd_next
  ha_ircon::rnd_next
  ha_ircon::rnd_next
  ha_ircon::rnd_next
  ha_ircon::rnd_next
  ha_ircon::rnd_next
  ha_ircon::rnd_next
  ha_ircon::extra
  ENUM HA_EXTRA_NO_CACHE     End caching of records (def)
  ha_ircon::external_lock
  ha_ircon::extra
  ENUM HA_EXTRA_RESET        Reset database to after open
  @endcode

  Here you see that the ircon storage engine has 9 rows called before
  rnd_next signals that it has reached the end of its data. Also note that
  the table in question was already opened; had it not been open, a call to
  ha_ircon::open() would also have been necessary. Calls to
  ha_ircon::extra() are hints as to what will be occuring to the request.

  A Longer Ircon can be found called the "Skeleton Engine" which can be 
  found on TangentOrg. It has both an engine and a full build environment
  for building a pluggable storage engine.

  Happy coding!<br>
    -Brian
*/

#include "sql_class.h"           // MYSQL_HANDLERTON_INTERFACE_VERSION
#include "ha_ircon.h"
#include "probes_mysql.h"
#include "sql_plugin.h"

#include "table.h"
#include "field.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static handler *ircon_create_handler(handlerton *hton,
                                       TABLE_SHARE *table, 
                                       MEM_ROOT *mem_root);

handlerton *ircon_hton;

/* Interface to mysqld, to check system tables supported by SE */
static const char* ircon_system_database();
static bool ircon_is_supported_system_table(const char *db,
                                      const char *table_name,
                                      bool is_sql_layer_system_table);

Ircon_share::Ircon_share()
{
  thr_lock_init(&lock);
  this->socket_opened = false;
}


static int ircon_init_func(void *p)
{
  DBUG_ENTER("ircon_init_func");

  ircon_hton= (handlerton *)p;
  ircon_hton->state=                     SHOW_OPTION_YES;
  ircon_hton->create=                    ircon_create_handler;
  ircon_hton->flags=                     HTON_CAN_RECREATE;
  ircon_hton->system_database=   ircon_system_database;
  ircon_hton->is_supported_system_table= ircon_is_supported_system_table;

  DBUG_RETURN(0);
}


/**
  @brief
  Ircon of simple lock controls. The "share" it creates is a
  structure we will pass to each ircon handler. Do you have to have
  one of these? Well, you have pieces that are used for locking, and
  they are needed to function.
*/

Ircon_share *ha_ircon::get_share()
{
  Ircon_share *tmp_share;

  DBUG_ENTER("ha_ircon::get_share()");

  lock_shared_ha_data();
  if (!(tmp_share= static_cast<Ircon_share*>(get_ha_share_ptr())))
  {
    tmp_share= new Ircon_share;
    if (!tmp_share)
      goto err;

    set_ha_share_ptr(static_cast<Handler_share*>(tmp_share));
  }
err:
  unlock_shared_ha_data();
  DBUG_RETURN(tmp_share);
}


static handler* ircon_create_handler(handlerton *hton,
                                       TABLE_SHARE *table, 
                                       MEM_ROOT *mem_root)
{
  return new (mem_root) ha_ircon(hton, table);
}

ha_ircon::ha_ircon(handlerton *hton, TABLE_SHARE *table_arg)
  :handler(hton, table_arg)
{}


/**
  @brief
  If frm_error() is called then we will use this to determine
  the file extensions that exist for the storage engine. This is also
  used by the default rename_table and delete_table method in
  handler.cc.

  For engines that have two file name extentions (separate meta/index file
  and data file), the order of elements is relevant. First element of engine
  file name extentions array should be meta/index file extention. Second
  element - data file extention. This order is assumed by
  prepare_for_repair() when REPAIR TABLE ... USE_FRM is issued.

  @see
  rename_table method in handler.cc and
  delete_table method in handler.cc
*/

static const char *ha_ircon_exts[] = {
  NullS
};

const char **ha_ircon::bas_ext() const
{
  return ha_ircon_exts;
}

/*
  Following handler function provides access to
  system database specific to SE. This interface
  is optional, so every SE need not implement it.
*/
const char* ha_ircon_system_database= NULL;
const char* ircon_system_database()
{
  return ha_ircon_system_database;
}

/*
  List of all system tables specific to the SE.
  Array element would look like below,
     { "<database_name>", "<system table name>" },
  The last element MUST be,
     { (const char*)NULL, (const char*)NULL }

  This array is optional, so every SE need not implement it.
*/
static st_handler_tablename ha_ircon_system_tables[]= {
  {(const char*)NULL, (const char*)NULL}
};

/**
  @brief Check if the given db.tablename is a system table for this SE.

  @param db                         Database name to check.
  @param table_name                 table name to check.
  @param is_sql_layer_system_table  if the supplied db.table_name is a SQL
                                    layer system table.

  @return
    @retval TRUE   Given db.table_name is supported system table.
    @retval FALSE  Given db.table_name is not a supported system table.
*/
static bool ircon_is_supported_system_table(const char *db,
                                              const char *table_name,
                                              bool is_sql_layer_system_table)
{
  st_handler_tablename *systab;

  // Does this SE support "ALL" SQL layer system tables ?
  if (is_sql_layer_system_table)
    return false;

  // Check if this is SE layer system tables
  systab= ha_ircon_system_tables;
  while (systab && systab->db)
  {
    if (systab->db == db &&
        strcmp(systab->tablename, table_name) == 0)
      return true;
    systab++;
  }

  return false;
}


/**
  @brief
  Used for opening tables. The name will be the name of the file.

  @details
  A table is opened when it needs to be opened; e.g. when a request comes in
  for a SELECT on the table (tables are not open and closed for each request,
  they are cached).

  Called from handler.cc by handler::ha_open(). The server opens all tables by
  calling ha_open() which then calls the handler specific open().

  @see
  handler::ha_open() in handler.cc
*/

int ha_ircon::open(const char *name, int mode, uint test_if_locked)
{
  DBUG_ENTER("ha_ircon::open");

  if (!(share = get_share()))
    DBUG_RETURN(1);
  thr_lock_data_init(&share->lock,&lock,NULL);

  if (!share->socket_opened) {
    char *ip_port_buf = new char[32];
    char *ip_ptr = ip_port_buf;
    int port = 0;
    struct sockaddr_in addr;
    if((share->socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
      DBUG_RETURN(-1);
    }

    strncpy(ip_port_buf, table_share->table_name.str, 32);
    for (int i = 0; i < 32; i++) {
      if (ip_port_buf[i] == ':') {
        ip_port_buf[i] = '\0';
        if (i < 31) {
          port = atoi(ip_port_buf + i + 1);
        }
        break;
      }
    }
    if (port == 0) {
      port = IRCON_DEFAULT_PORT;
    }

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip_ptr);
    delete ip_port_buf;
    connect(share->socket, (struct sockaddr *)&addr, sizeof(struct sockaddr_in));
    share->socket_opened = true;

    strncpy(share->state_mode, IRCON_COMMAND_UNKNOWN, IRCON_COMMAND_LENGTH);
    strncpy(share->state_temperature, IRCON_COMMAND_UNKNOWN, IRCON_COMMAND_LENGTH);
    strncpy(share->state_power, IRCON_COMMAND_UNKNOWN, IRCON_COMMAND_LENGTH);
    strncpy(share->state_angle, IRCON_COMMAND_UNKNOWN, IRCON_COMMAND_LENGTH);
  }
  DBUG_RETURN(0);
}


/**
  @brief
  Closes a table.

  @details
  Called from sql_base.cc, sql_select.cc, and table.cc. In sql_select.cc it is
  only used to close up temporary tables or during the process where a
  temporary table is converted over to being a myisam table.

  For sql_base.cc look at close_data_tables().

  @see
  sql_base.cc, sql_select.cc and table.cc
*/

int ha_ircon::close(void)
{
  DBUG_ENTER("ha_ircon::close");

  shutdown(share->socket, SHUT_RDWR);
  my_close(share->socket, MYF(0));
  share->socket_opened = false;

  DBUG_RETURN(0);
}


/**
  @brief
  write_row() inserts a row. No extra() hint is given currently if a bulk load
  is happening. buf() is a byte array of data. You can use the field
  information to extract the data from the native byte array type.

  @details
  Ircon of this would be:
  @code
  for (Field **field=table->field ; *field ; field++)
  {
    ...
  }
  @endcode

  See ha_tina.cc for an ircon of extracting all of the data as strings.
  ha_berekly.cc has an ircon of how to store it intact by "packing" it
  for ha_berkeley's own native storage type.

  See the note for update_row() on auto_increments. This case also applies to
  write_row().

  Called from item_sum.cc, item_sum.cc, sql_acl.cc, sql_insert.cc,
  sql_insert.cc, sql_select.cc, sql_table.cc, sql_udf.cc, and sql_update.cc.

  @see
  item_sum.cc, item_sum.cc, sql_acl.cc, sql_insert.cc,
  sql_insert.cc, sql_select.cc, sql_table.cc, sql_udf.cc and sql_update.cc
*/
int ha_ircon::init_writer(const char *ip) {
  DBUG_ENTER("ha_ircon::init_writer");
  DBUG_RETURN(0);
}

int ha_ircon::write_update_row(void) {
  char attribute_buffer[1024];
  String attribute(attribute_buffer, sizeof(attribute_buffer), &my_charset_bin);
  my_bitmap_map *org_bitmap = tmp_use_all_columns(table, table->read_set);
  for (Field **field = table->field; *field; field++) {
    (*field)->val_str(&attribute, &attribute);

    if (strncmp((*field)->field_name, IRCON_COMMAND_MODE, strlen((*field)->field_name)) == 0) {
      if (attribute.length() > 0) {
        strncpy(share->state_mode, attribute.ptr(), attribute.length());
        share->state_mode[attribute.length()] = '\0';
      }
      send(share->socket, (*field)->field_name, strlen((*field)->field_name), 0);
      send(share->socket, ":", 1, 0);
      send(share->socket, share->state_mode, strlen(share->state_mode), 0);
    } else if (strncmp((*field)->field_name, IRCON_COMMAND_TEMPERATURE, strlen((*field)->field_name)) == 0) {
      if (attribute.length() > 0) {
        strncpy(share->state_temperature, attribute.ptr(), attribute.length());
        share->state_temperature[attribute.length()] = '\0';
      }
      send(share->socket, (*field)->field_name, strlen((*field)->field_name), 0);
      send(share->socket, ":", 1, 0);
      send(share->socket, share->state_temperature, strlen(share->state_temperature), 0);
    } else if (strncmp((*field)->field_name, IRCON_COMMAND_POWER, strlen((*field)->field_name)) == 0) {
      if (attribute.length() > 0) {
        strncpy(share->state_power, attribute.ptr(), attribute.length());
        share->state_power[attribute.length()] = '\0';
      }
      send(share->socket, (*field)->field_name, strlen((*field)->field_name), 0);
      send(share->socket, ":", 1, 0);
      send(share->socket, share->state_power, strlen(share->state_power), 0);
    } else if (strncmp((*field)->field_name, IRCON_COMMAND_ANGLE, strlen((*field)->field_name)) == 0) {
      if (attribute.length() > 0) {
        strncpy(share->state_angle, attribute.ptr(), attribute.length());
        share->state_angle[attribute.length()] = '\0';
      }
      send(share->socket, (*field)->field_name, strlen((*field)->field_name), 0);
      send(share->socket, ":", 1, 0);
      send(share->socket, share->state_angle, strlen(share->state_angle), 0);
    } else {
      continue;
    }
    send(share->socket, ",", 1, 0);
  }
  send(share->socket, "\n", 1, 0);
  tmp_restore_column_map(table->read_set, org_bitmap);

  return 0;
}

int ha_ircon::write_row(uchar *buf)
{
  DBUG_ENTER("ha_ircon::write_row");
  DBUG_RETURN(write_update_row());
}


/**
  @brief
  Yes, update_row() does what you expect, it updates a row. old_data will have
  the previous row record in it, while new_data will have the newest data in it.
  Keep in mind that the server can do updates based on ordering if an ORDER BY
  clause was used. Consecutive ordering is not guaranteed.

  @details
  Currently new_data will not have an updated auto_increament record. You can
  do this for ircon by doing:

  @code

  if (table->next_number_field && record == table->record[0])
    update_auto_increment();

  @endcode

  Called from sql_select.cc, sql_acl.cc, sql_update.cc, and sql_insert.cc.

  @see
  sql_select.cc, sql_acl.cc, sql_update.cc and sql_insert.cc
*/
int ha_ircon::update_row(const uchar *old_data, uchar *new_data)
{
  DBUG_ENTER("ha_ircon::update_row");
  DBUG_RETURN(write_update_row());
}


/**
  @brief
  This will delete a row. buf will contain a copy of the row to be deleted.
  The server will call this right after the current row has been called (from
  either a previous rnd_nexT() or index call).

  @details
  If you keep a pointer to the last row or can access a primary key it will
  make doing the deletion quite a bit easier. Keep in mind that the server does
  not guarantee consecutive deletions. ORDER BY clauses can be used.

  Called in sql_acl.cc and sql_udf.cc to manage internal table
  information.  Called in sql_delete.cc, sql_insert.cc, and
  sql_select.cc. In sql_select it is used for removing duplicates
  while in insert it is used for REPLACE calls.

  @see
  sql_acl.cc, sql_udf.cc, sql_delete.cc, sql_insert.cc and sql_select.cc
*/

int ha_ircon::delete_row(const uchar *buf)
{
  DBUG_ENTER("ha_ircon::delete_row");
  strncpy(share->state_mode, IRCON_COMMAND_UNKNOWN, IRCON_COMMAND_LENGTH);
  strncpy(share->state_temperature, IRCON_COMMAND_UNKNOWN, IRCON_COMMAND_LENGTH);
  strncpy(share->state_power, IRCON_COMMAND_UNKNOWN, IRCON_COMMAND_LENGTH);
  strncpy(share->state_angle, IRCON_COMMAND_UNKNOWN, IRCON_COMMAND_LENGTH);
  send(share->socket, "mode:-,\n", 8, 0);
  DBUG_RETURN(0);
}


/**
  @brief
  Positions an index cursor to the index specified in the handle. Fetches the
  row if available. If the key value is null, begin at the first key of the
  index.
*/

int ha_ircon::index_read_map(uchar *buf, const uchar *key,
                               key_part_map keypart_map MY_ATTRIBUTE((unused)),
                               enum ha_rkey_function find_flag
                               MY_ATTRIBUTE((unused)))
{
  int rc;
  DBUG_ENTER("ha_ircon::index_read");
  MYSQL_INDEX_READ_ROW_START(table_share->db.str, table_share->table_name.str);
  rc= HA_ERR_WRONG_COMMAND;
  MYSQL_INDEX_READ_ROW_DONE(rc);
  DBUG_RETURN(rc);
}


/**
  @brief
  Used to read forward through the index.
*/

int ha_ircon::index_next(uchar *buf)
{
  int rc;
  DBUG_ENTER("ha_ircon::index_next");
  MYSQL_INDEX_READ_ROW_START(table_share->db.str, table_share->table_name.str);
  rc= HA_ERR_WRONG_COMMAND;
  MYSQL_INDEX_READ_ROW_DONE(rc);
  DBUG_RETURN(rc);
}


/**
  @brief
  Used to read backwards through the index.
*/

int ha_ircon::index_prev(uchar *buf)
{
  int rc;
  DBUG_ENTER("ha_ircon::index_prev");
  MYSQL_INDEX_READ_ROW_START(table_share->db.str, table_share->table_name.str);
  rc= HA_ERR_WRONG_COMMAND;
  MYSQL_INDEX_READ_ROW_DONE(rc);
  DBUG_RETURN(rc);
}


/**
  @brief
  index_first() asks for the first key in the index.

  @details
  Called from opt_range.cc, opt_sum.cc, sql_handler.cc, and sql_select.cc.

  @see
  opt_range.cc, opt_sum.cc, sql_handler.cc and sql_select.cc
*/
int ha_ircon::index_first(uchar *buf)
{
  int rc;
  DBUG_ENTER("ha_ircon::index_first");
  MYSQL_INDEX_READ_ROW_START(table_share->db.str, table_share->table_name.str);
  rc= HA_ERR_WRONG_COMMAND;
  MYSQL_INDEX_READ_ROW_DONE(rc);
  DBUG_RETURN(rc);
}


/**
  @brief
  index_last() asks for the last key in the index.

  @details
  Called from opt_range.cc, opt_sum.cc, sql_handler.cc, and sql_select.cc.

  @see
  opt_range.cc, opt_sum.cc, sql_handler.cc and sql_select.cc
*/
int ha_ircon::index_last(uchar *buf)
{
  int rc;
  DBUG_ENTER("ha_ircon::index_last");
  MYSQL_INDEX_READ_ROW_START(table_share->db.str, table_share->table_name.str);
  rc= HA_ERR_WRONG_COMMAND;
  MYSQL_INDEX_READ_ROW_DONE(rc);
  DBUG_RETURN(rc);
}


/**
  @brief
  rnd_init() is called when the system wants the storage engine to do a table
  scan. See the ircon in the introduction at the top of this file to see when
  rnd_init() is called.

  @details
  Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc,
  and sql_update.cc.

  @see
  filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc and sql_update.cc
*/
int ha_ircon::rnd_init(bool scan)
{
  DBUG_ENTER("ha_ircon::rnd_init");
  next_is_eof = false;
  DBUG_RETURN(0);
}

int ha_ircon::rnd_end()
{
  DBUG_ENTER("ha_ircon::rnd_end");
  DBUG_RETURN(0);
}


/**
  @brief
  This is called for each row of the table scan. When you run out of records
  you should return HA_ERR_END_OF_FILE. Fill buff up with the row information.
  The Field structure for the table is the key to getting data into buf
  in a manner that will allow the server to understand it.

  @details
  Called from filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc,
  and sql_update.cc.

  @see
  filesort.cc, records.cc, sql_handler.cc, sql_select.cc, sql_table.cc and sql_update.cc
*/
int ha_ircon::rnd_next(uchar *buf)
{
  my_bitmap_map *org_bitmap;
  int rc;
  char *res = NULL;
  DBUG_ENTER("ha_ircon::rnd_next");

  MYSQL_READ_ROW_START(table_share->db.str, table_share->table_name.str,
                       TRUE);

  if (next_is_eof) {
    rc= HA_ERR_END_OF_FILE;
    MYSQL_READ_ROW_DONE(rc);
    DBUG_RETURN(rc);
  }

  memset(buf, 0, table->s->null_bytes);
  org_bitmap = tmp_use_all_columns(table, table->write_set);
  for (Field **field = table->field; *field; field++) {
    if (strncmp((*field)->field_name, IRCON_COMMAND_MODE, (*field)->field_length) == 0) {
      res = share->state_mode;
    } else if (strncmp((*field)->field_name, IRCON_COMMAND_TEMPERATURE, (*field)->field_length) == 0) {
      res = share->state_temperature;
    } else if (strncmp((*field)->field_name, IRCON_COMMAND_POWER, (*field)->field_length) == 0) {
      res = share->state_power;
    } else if (strncmp((*field)->field_name, IRCON_COMMAND_ANGLE, (*field)->field_length) == 0) {
      res = share->state_angle;
    } else {
      res = (char *)IRCON_COMMAND_UNKNOWN;
    }
    (*field)->store(res, strlen(res), system_charset_info);
  }
  tmp_restore_column_map(table->write_set, org_bitmap);
  stats.records++;

  next_is_eof = true;
  DBUG_RETURN(0);
}


/**
  @brief
  position() is called after each call to rnd_next() if the data needs
  to be ordered. You can do something like the following to store
  the position:
  @code
  my_store_ptr(ref, ref_length, current_position);
  @endcode

  @details
  The server uses ref to store data. ref_length in the above case is
  the size needed to store current_position. ref is just a byte array
  that the server will maintain. If you are using offsets to mark rows, then
  current_position should be the offset. If it is a primary key like in
  BDB, then it needs to be a primary key.

  Called from filesort.cc, sql_select.cc, sql_delete.cc, and sql_update.cc.

  @see
  filesort.cc, sql_select.cc, sql_delete.cc and sql_update.cc
*/
void ha_ircon::position(const uchar *record)
{
  DBUG_ENTER("ha_ircon::position");
  DBUG_VOID_RETURN;
}


/**
  @brief
  This is like rnd_next, but you are given a position to use
  to determine the row. The position will be of the type that you stored in
  ref. You can use ha_get_ptr(pos,ref_length) to retrieve whatever key
  or position you saved when position() was called.

  @details
  Called from filesort.cc, records.cc, sql_insert.cc, sql_select.cc, and sql_update.cc.

  @see
  filesort.cc, records.cc, sql_insert.cc, sql_select.cc and sql_update.cc
*/
int ha_ircon::rnd_pos(uchar *buf, uchar *pos)
{
  int rc;
  DBUG_ENTER("ha_ircon::rnd_pos");
  MYSQL_READ_ROW_START(table_share->db.str, table_share->table_name.str,
                       TRUE);
  rc= HA_ERR_WRONG_COMMAND;
  MYSQL_READ_ROW_DONE(rc);
  DBUG_RETURN(rc);
}


/**
  @brief
  ::info() is used to return information to the optimizer. See my_base.h for
  the complete description.

  @details
  Currently this table handler doesn't implement most of the fields really needed.
  SHOW also makes use of this data.

  You will probably want to have the following in your code:
  @code
  if (records < 2)
    records = 2;
  @endcode
  The reason is that the server will optimize for cases of only a single
  record. If, in a table scan, you don't know the number of records, it
  will probably be better to set records to two so you can return as many
  records as you need. Along with records, a few more variables you may wish
  to set are:
    records
    deleted
    data_file_length
    index_file_length
    delete_length
    check_time
  Take a look at the public variables in handler.h for more information.

  Called in filesort.cc, ha_heap.cc, item_sum.cc, opt_sum.cc, sql_delete.cc,
  sql_delete.cc, sql_derived.cc, sql_select.cc, sql_select.cc, sql_select.cc,
  sql_select.cc, sql_select.cc, sql_show.cc, sql_show.cc, sql_show.cc, sql_show.cc,
  sql_table.cc, sql_union.cc, and sql_update.cc.

  @see
  filesort.cc, ha_heap.cc, item_sum.cc, opt_sum.cc, sql_delete.cc, sql_delete.cc,
  sql_derived.cc, sql_select.cc, sql_select.cc, sql_select.cc, sql_select.cc,
  sql_select.cc, sql_show.cc, sql_show.cc, sql_show.cc, sql_show.cc, sql_table.cc,
  sql_union.cc and sql_update.cc
*/
int ha_ircon::info(uint flag)
{
  DBUG_ENTER("ha_ircon::info");
  stats.records = 2;
  DBUG_RETURN(0);
}


/**
  @brief
  extra() is called whenever the server wishes to send a hint to
  the storage engine. The myisam engine implements the most hints.
  ha_innodb.cc has the most exhaustive list of these hints.

    @see
  ha_innodb.cc
*/
int ha_ircon::extra(enum ha_extra_function operation)
{
  DBUG_ENTER("ha_ircon::extra");
  DBUG_RETURN(0);
}


/**
  @brief
  Used to delete all rows in a table, including cases of truncate and cases where
  the optimizer realizes that all rows will be removed as a result of an SQL statement.

  @details
  Called from item_sum.cc by Item_func_group_concat::clear(),
  Item_sum_count_distinct::clear(), and Item_func_group_concat::clear().
  Called from sql_delete.cc by mysql_delete().
  Called from sql_select.cc by JOIN::reinit().
  Called from sql_union.cc by st_select_lex_unit::exec().

  @see
  Item_func_group_concat::clear(), Item_sum_count_distinct::clear() and
  Item_func_group_concat::clear() in item_sum.cc;
  mysql_delete() in sql_delete.cc;
  JOIN::reinit() in sql_select.cc and
  st_select_lex_unit::exec() in sql_union.cc.
*/
int ha_ircon::delete_all_rows()
{
  DBUG_ENTER("ha_ircon::delete_all_rows");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}


/**
  @brief
  Used for handler specific truncate table.  The table is locked in
  exclusive mode and handler is responsible for reseting the auto-
  increment counter.

  @details
  Called from Truncate_statement::handler_truncate.
  Not used if the handlerton supports HTON_CAN_RECREATE, unless this
  engine can be used as a partition. In this case, it is invoked when
  a particular partition is to be truncated.

  @see
  Truncate_statement in sql_truncate.cc
  Remarks in handler::truncate.
*/
int ha_ircon::truncate()
{
  DBUG_ENTER("ha_ircon::truncate");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}


/**
  @brief
  This create a lock on the table. If you are implementing a storage engine
  that can handle transacations look at ha_berkely.cc to see how you will
  want to go about doing this. Otherwise you should consider calling flock()
  here. Hint: Read the section "locking functions for mysql" in lock.cc to understand
  this.

  @details
  Called from lock.cc by lock_external() and unlock_external(). Also called
  from sql_table.cc by copy_data_between_tables().

  @see
  lock.cc by lock_external() and unlock_external() in lock.cc;
  the section "locking functions for mysql" in lock.cc;
  copy_data_between_tables() in sql_table.cc.
*/
int ha_ircon::external_lock(THD *thd, int lock_type)
{
  DBUG_ENTER("ha_ircon::external_lock");
  DBUG_RETURN(0);
}


/**
  @brief
  The idea with handler::store_lock() is: The statement decides which locks
  should be needed for the table. For updates/deletes/inserts we get WRITE
  locks, for SELECT... we get read locks.

  @details
  Before adding the lock into the table lock handler (see thr_lock.c),
  mysqld calls store lock with the requested locks. Store lock can now
  modify a write lock to a read lock (or some other lock), ignore the
  lock (if we don't want to use MySQL table locks at all), or add locks
  for many tables (like we do when we are using a MERGE handler).

  Berkeley DB, for ircon, changes all WRITE locks to TL_WRITE_ALLOW_WRITE
  (which signals that we are doing WRITES, but are still allowing other
  readers and writers).

  When releasing locks, store_lock() is also called. In this case one
  usually doesn't have to do anything.

  In some exceptional cases MySQL may send a request for a TL_IGNORE;
  This means that we are requesting the same lock as last time and this
  should also be ignored. (This may happen when someone does a flush
  table when we have opened a part of the tables, in which case mysqld
  closes and reopens the tables and tries to get the same locks at last
  time). In the future we will probably try to remove this.

  Called from lock.cc by get_lock_data().

  @note
  In this method one should NEVER rely on table->in_use, it may, in fact,
  refer to a different thread! (this happens if get_lock_data() is called
  from mysql_lock_abort_for_thread() function)

  @see
  get_lock_data() in lock.cc
*/
THR_LOCK_DATA **ha_ircon::store_lock(THD *thd,
                                       THR_LOCK_DATA **to,
                                       enum thr_lock_type lock_type)
{
  if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK)
    lock.type=lock_type;
  *to++= &lock;
  return to;
}


/**
  @brief
  Used to delete a table. By the time delete_table() has been called all
  opened references to this table will have been closed (and your globally
  shared references released). The variable name will just be the name of
  the table. You will need to remove any files you have created at this point.

  @details
  If you do not implement this, the default delete_table() is called from
  handler.cc and it will delete all files with the file extensions returned
  by bas_ext().

  Called from handler.cc by delete_table and ha_create_table(). Only used
  during create if the table_flag HA_DROP_BEFORE_CREATE was specified for
  the storage engine.

  @see
  delete_table and ha_create_table() in handler.cc
*/
int ha_ircon::delete_table(const char *name)
{
  DBUG_ENTER("ha_ircon::delete_table");
  /* This is not implemented but we want someone to be able that it works. */
  DBUG_RETURN(0);
}


/**
  @brief
  Renames a table from one name to another via an alter table call.

  @details
  If you do not implement this, the default rename_table() is called from
  handler.cc and it will delete all files with the file extensions returned
  by bas_ext().

  Called from sql_table.cc by mysql_rename_table().

  @see
  mysql_rename_table() in sql_table.cc
*/
int ha_ircon::rename_table(const char * from, const char * to)
{
  DBUG_ENTER("ha_ircon::rename_table ");
  DBUG_RETURN(HA_ERR_WRONG_COMMAND);
}


/**
  @brief
  Given a starting key and an ending key, estimate the number of rows that
  will exist between the two keys.

  @details
  end_key may be empty, in which case determine if start_key matches any rows.

  Called from opt_range.cc by check_quick_keys().

  @see
  check_quick_keys() in opt_range.cc
*/
ha_rows ha_ircon::records_in_range(uint inx, key_range *min_key,
                                     key_range *max_key)
{
  DBUG_ENTER("ha_ircon::records_in_range");
  DBUG_RETURN(10);                         // low number to force index usage
}


/**
  @brief
  create() is called to create a database. The variable name will have the name
  of the table.

  @details
  When create() is called you do not need to worry about
  opening the table. Also, the .frm file will have already been
  created so adjusting create_info is not necessary. You can overwrite
  the .frm file at this point if you wish to change the table
  definition, but there are no methods currently provided for doing
  so.

  Called from handle.cc by ha_create_table().

  @see
  ha_create_table() in handle.cc
*/

int ha_ircon::create(const char *name, TABLE *table_arg,
                       HA_CREATE_INFO *create_info)
{
  DBUG_ENTER("ha_ircon::create");
  /*
    This is not implemented but we want someone to be able to see that it
    works.
  */
  DBUG_RETURN(0);
}


struct st_mysql_storage_engine ircon_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

static ulong srv_enum_var= 0;
static ulong srv_ulong_var= 0;
static double srv_double_var= 0;

const char *enum_var_names[]=
{
  "e1", "e2", NullS
};

TYPELIB enum_var_typelib=
{
  array_elements(enum_var_names) - 1, "enum_var_typelib",
  enum_var_names, NULL
};

static MYSQL_SYSVAR_ENUM(
  enum_var,                       // name
  srv_enum_var,                   // varname
  PLUGIN_VAR_RQCMDARG,            // opt
  "Sample ENUM system variable.", // comment
  NULL,                           // check
  NULL,                           // update
  0,                              // def
  &enum_var_typelib);             // typelib

static MYSQL_SYSVAR_ULONG(
  ulong_var,
  srv_ulong_var,
  PLUGIN_VAR_RQCMDARG,
  "0..1000",
  NULL,
  NULL,
  8,
  0,
  1000,
  0);

static MYSQL_SYSVAR_DOUBLE(
  double_var,
  srv_double_var,
  PLUGIN_VAR_RQCMDARG,
  "0.500000..1000.500000",
  NULL,
  NULL,
  8.5,
  0.5,
  1000.5,
  0);                             // reserved always 0

static MYSQL_THDVAR_DOUBLE(
  double_thdvar,
  PLUGIN_VAR_RQCMDARG,
  "0.500000..1000.500000",
  NULL,
  NULL,
  8.5,
  0.5,
  1000.5,
  0);

static struct st_mysql_sys_var* ircon_system_variables[]= {
  MYSQL_SYSVAR(enum_var),
  MYSQL_SYSVAR(ulong_var),
  MYSQL_SYSVAR(double_var),
  MYSQL_SYSVAR(double_thdvar),
  NULL
};

// this is an ircon of SHOW_FUNC and of my_snprintf() service
static int show_func_ircon(MYSQL_THD thd, struct st_mysql_show_var *var,
                             char *buf)
{
  var->type= SHOW_CHAR;
  var->value= buf; // it's of SHOW_VAR_FUNC_BUFF_SIZE bytes
  my_snprintf(buf, SHOW_VAR_FUNC_BUFF_SIZE,
              "enum_var is %lu, ulong_var is %lu, "
              "double_var is %f, %.6b", // %b is a MySQL extension
              srv_enum_var, srv_ulong_var, srv_double_var, "really");
  return 0;
}

struct ircon_vars_t
{
	ulong  var1;
	double var2;
	char   var3[64];
  bool   var4;
  bool   var5;
  ulong  var6;
};

ircon_vars_t ircon_vars= {100, 20.01, "three hundred", true, 0, 8250};

static st_mysql_show_var show_status_ircon[]=
{
  {"var1", (char *)&ircon_vars.var1, SHOW_LONG, SHOW_SCOPE_GLOBAL},
  {"var2", (char *)&ircon_vars.var2, SHOW_DOUBLE, SHOW_SCOPE_GLOBAL},
  {0,0,SHOW_UNDEF, SHOW_SCOPE_UNDEF} // null terminator required
};

static struct st_mysql_show_var show_array_ircon[]=
{
  {"array", (char *)show_status_ircon, SHOW_ARRAY, SHOW_SCOPE_GLOBAL},
  {"var3", (char *)&ircon_vars.var3, SHOW_CHAR, SHOW_SCOPE_GLOBAL},
  {"var4", (char *)&ircon_vars.var4, SHOW_BOOL, SHOW_SCOPE_GLOBAL},
  {0,0,SHOW_UNDEF, SHOW_SCOPE_UNDEF}
};

static struct st_mysql_show_var func_status[]=
{
  {"ircon_func_ircon", (char *)show_func_ircon, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
  {"ircon_status_var5", (char *)&ircon_vars.var5, SHOW_BOOL, SHOW_SCOPE_GLOBAL},
  {"ircon_status_var6", (char *)&ircon_vars.var6, SHOW_LONG, SHOW_SCOPE_GLOBAL},
  {"ircon_status",  (char *)show_array_ircon, SHOW_ARRAY, SHOW_SCOPE_GLOBAL},
  {0,0,SHOW_UNDEF, SHOW_SCOPE_UNDEF}
};

mysql_declare_plugin(ircon)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &ircon_storage_engine,
  "IRCON",
  "Brian Aker, MySQL AB",
  "Ircon storage engine",
  PLUGIN_LICENSE_GPL,
  ircon_init_func,                            /* Plugin Init */
  NULL,                                         /* Plugin Deinit */
  0x0001 /* 0.1 */,
  func_status,                                  /* status variables */
  ircon_system_variables,                     /* system variables */
  NULL,                                         /* config options */
  0,                                            /* flags */
}
mysql_declare_plugin_end;
