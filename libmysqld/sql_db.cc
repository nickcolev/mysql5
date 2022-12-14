/* Copyright (C) 2000-2003 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */


/* create and drop of databases */

#include "mysql_priv.h"
#include <mysys_err.h>
#include "sp.h"
#include <my_dir.h>
#include <m_ctype.h>
#ifdef __WIN__
#include <direct.h>
#endif

#define MAX_DROP_TABLE_Q_LEN      1024

const char *del_exts[]= {".frm", ".BAK", ".TMD",".opt", NullS};
static TYPELIB deletable_extentions=
{array_elements(del_exts)-1,"del_exts", del_exts, NULL};

static long mysql_rm_known_files(THD *thd, MY_DIR *dirp,
				 const char *db, const char *path, uint level, 
                                 TABLE_LIST **dropped_tables);
         
static long mysql_rm_arc_files(THD *thd, MY_DIR *dirp, const char *org_path);
static my_bool rm_dir_w_symlink(const char *org_path, my_bool send_error);
/* Database options hash */
static HASH dboptions;
static my_bool dboptions_init= 0;
static rw_lock_t LOCK_dboptions;

/* Structure for database options */
typedef struct my_dbopt_st
{
  char *name;			/* Database name                  */
  uint name_length;		/* Database length name           */
  CHARSET_INFO *charset;	/* Database default character set */
} my_dbopt_t;


/*
  Function we use in the creation of our hash to get key.
*/

static byte* dboptions_get_key(my_dbopt_t *opt, uint *length,
                               my_bool not_used __attribute__((unused)))
{
  *length= opt->name_length;
  return (byte*) opt->name;
}


/*
  Helper function to write a query to binlog used by mysql_rm_db()
*/

static inline void write_to_binlog(THD *thd, char *query, uint q_len,
                                   char *db, uint db_len)
{
  Query_log_event qinfo(thd, query, q_len, 0, 0);
  qinfo.error_code= 0;
  qinfo.db= db;
  qinfo.db_len= db_len;
  mysql_bin_log.write(&qinfo);
}  


/*
  Function to free dboptions hash element
*/

static void free_dbopt(void *dbopt)
{
  my_free((gptr) dbopt, MYF(0));
}


/* 
  Initialize database option hash

  SYNOPSIS
    my_dbopt_init()

  NOTES
    Must be called before any other database function is called.

  RETURN
    0	ok
    1	Fatal error
*/

bool my_dbopt_init(void)
{
  bool error= 0;
  (void) my_rwlock_init(&LOCK_dboptions, NULL);
  if (!dboptions_init)
  {
    dboptions_init= 1;
    error= hash_init(&dboptions, lower_case_table_names ? 
                     &my_charset_bin : system_charset_info,
                     32, 0, 0, (hash_get_key) dboptions_get_key,
                     free_dbopt,0);
  }
  return error;
}


/* 
  Free database option hash.
*/

void my_dbopt_free(void)
{
  if (dboptions_init)
  {
    dboptions_init= 0;
    hash_free(&dboptions);
    (void) rwlock_destroy(&LOCK_dboptions);
  }
}


void my_dbopt_cleanup(void)
{
  rw_wrlock(&LOCK_dboptions);
  hash_free(&dboptions);
  hash_init(&dboptions, lower_case_table_names ? 
            &my_charset_bin : system_charset_info,
            32, 0, 0, (hash_get_key) dboptions_get_key,
            free_dbopt,0);
  rw_unlock(&LOCK_dboptions);
}


/*
  Find database options in the hash.
  
  DESCRIPTION
    Search a database options in the hash, usings its path.
    Fills "create" on success.
  
  RETURN VALUES
    0 on success.
    1 on error.
*/

static my_bool get_dbopt(const char *dbname, HA_CREATE_INFO *create)
{
  my_dbopt_t *opt;
  uint length;
  my_bool error= 1;
  
  length= (uint) strlen(dbname);
  
  rw_rdlock(&LOCK_dboptions);
  if ((opt= (my_dbopt_t*) hash_search(&dboptions, (byte*) dbname, length)))
  {
    create->default_table_charset= opt->charset;
    error= 0;
  }
  rw_unlock(&LOCK_dboptions);
  return error;
}


/*
  Writes database options into the hash.
  
  DESCRIPTION
    Inserts database options into the hash, or updates
    options if they are already in the hash.
  
  RETURN VALUES
    0 on success.
    1 on error.
*/

static my_bool put_dbopt(const char *dbname, HA_CREATE_INFO *create)
{
  my_dbopt_t *opt;
  uint length;
  my_bool error= 0;
  DBUG_ENTER("put_dbopt");

  length= (uint) strlen(dbname);
  
  rw_wrlock(&LOCK_dboptions);
  if (!(opt= (my_dbopt_t*) hash_search(&dboptions, (byte*) dbname, length)))
  { 
    /* Options are not in the hash, insert them */
    char *tmp_name;
    if (!my_multi_malloc(MYF(MY_WME | MY_ZEROFILL),
                         &opt, (uint) sizeof(*opt), &tmp_name, length+1,
                         NullS))
    {
      error= 1;
      goto end;
    }
    
    opt->name= tmp_name;
    strmov(opt->name, dbname);
    opt->name_length= length;
    
    if ((error= my_hash_insert(&dboptions, (byte*) opt)))
    {
      my_free((gptr) opt, MYF(0));
      goto end;
    }
  }

  /* Update / write options in hash */
  opt->charset= create->default_table_charset;

end:
  rw_unlock(&LOCK_dboptions);  
  DBUG_RETURN(error);
}


/*
  Deletes database options from the hash.
*/

void del_dbopt(const char *path)
{
  my_dbopt_t *opt;
  rw_wrlock(&LOCK_dboptions);
  if ((opt= (my_dbopt_t *)hash_search(&dboptions, (const byte*) path,
                                      strlen(path))))
    hash_delete(&dboptions, (byte*) opt);
  rw_unlock(&LOCK_dboptions);
}


/*
  Create database options file:

  DESCRIPTION
    Currently database default charset is only stored there.

  RETURN VALUES
  0	ok
  1	Could not create file or write to it.  Error sent through my_error()
*/

static bool write_db_opt(THD *thd, const char *path, HA_CREATE_INFO *create)
{
  register File file;
  char buf[256]; // Should be enough for one option
  bool error=1;

  if (!create->default_table_charset)
    create->default_table_charset= thd->variables.collation_server;

  if (put_dbopt(path, create))
    return 1;

  if ((file=my_create(path, CREATE_MODE,O_RDWR | O_TRUNC,MYF(MY_WME))) >= 0)
  {
    ulong length;
    length= (ulong) (strxnmov(buf, sizeof(buf), "default-character-set=",
                              create->default_table_charset->csname,
                              "\ndefault-collation=",
                              create->default_table_charset->name,
                              "\n", NullS) - buf);

    /* Error is written by my_write */
    if (!my_write(file,(byte*) buf, length, MYF(MY_NABP+MY_WME)))
      error=0;
    my_close(file,MYF(0));
  }
  return error;
}


/*
  Load database options file

  load_db_opt()
  path		Path for option file
  create	Where to store the read options

  DESCRIPTION

  RETURN VALUES
  0	File found
  1	No database file or could not open it

*/

bool load_db_opt(THD *thd, const char *path, HA_CREATE_INFO *create)
{
  File file;
  char buf[256];
  DBUG_ENTER("load_db_opt");
  bool error=1;
  uint nbytes;

  bzero((char*) create,sizeof(*create));
  create->default_table_charset= thd->variables.collation_server;

  /* Check if options for this database are already in the hash */
  if (!get_dbopt(path, create))
    DBUG_RETURN(0);

  /* Otherwise, load options from the .opt file */
  if ((file=my_open(path, O_RDONLY | O_SHARE, MYF(0))) < 0)
    goto err1;

  IO_CACHE cache;
  if (init_io_cache(&cache, file, IO_SIZE, READ_CACHE, 0, 0, MYF(0)))
    goto err2;

  while ((int) (nbytes= my_b_gets(&cache, (char*) buf, sizeof(buf))) > 0)
  {
    char *pos= buf+nbytes-1;
    /* Remove end space and control characters */
    while (pos > buf && !my_isgraph(&my_charset_latin1, pos[-1]))
      pos--;
    *pos=0;
    if ((pos= strchr(buf, '=')))
    {
      if (!strncmp(buf,"default-character-set", (pos-buf)))
      {
        /*
           Try character set name, and if it fails
           try collation name, probably it's an old
           4.1.0 db.opt file, which didn't have
           separate default-character-set and
           default-collation commands.
        */
        if (!(create->default_table_charset=
        get_charset_by_csname(pos+1, MY_CS_PRIMARY, MYF(0))) &&
            !(create->default_table_charset=
              get_charset_by_name(pos+1, MYF(0))))
        {
          sql_print_error("Error while loading database options: '%s':",path);
          sql_print_error(ER(ER_UNKNOWN_CHARACTER_SET),pos+1);
          create->default_table_charset= default_charset_info;
        }
      }
      else if (!strncmp(buf,"default-collation", (pos-buf)))
      {
        if (!(create->default_table_charset= get_charset_by_name(pos+1,
                                                           MYF(0))))
        {
          sql_print_error("Error while loading database options: '%s':",path);
          sql_print_error(ER(ER_UNKNOWN_COLLATION),pos+1);
          create->default_table_charset= default_charset_info;
        }
      }
    }
  }
  /*
    Put the loaded value into the hash.
    Note that another thread could've added the same
    entry to the hash after we called get_dbopt(),
    but it's not an error, as put_dbopt() takes this
    possibility into account.
  */
  error= put_dbopt(path, create);

  end_io_cache(&cache);
err2:
  my_close(file,MYF(0));
err1:
  DBUG_RETURN(error);
}


/*
  Retrieve database options by name. Load database options file or fetch from
  cache.

  SYNOPSIS
    load_db_opt_by_name()
    db_name         Database name
    db_create_info  Where to store the database options

  DESCRIPTION
    load_db_opt_by_name() is a shortcut for load_db_opt().

  NOTE
    Although load_db_opt_by_name() (and load_db_opt()) returns status of
    the operation, it is useless usually and should be ignored. The problem
    is that there are 1) system databases ("mysql") and 2) virtual
    databases ("information_schema"), which do not contain options file.
    So, load_db_opt[_by_name]() returns FALSE for these databases, but this
    is not an error.

    load_db_opt[_by_name]() clears db_create_info structure in any case, so
    even on failure it contains valid data. So, common use case is just
    call load_db_opt[_by_name]() without checking return value and use
    db_create_info right after that.

  RETURN VALUES (read NOTE!)
    FALSE   Success
    TRUE    Failed to retrieve options
*/

bool load_db_opt_by_name(THD *thd, const char *db_name,
                         HA_CREATE_INFO *db_create_info)
{
  char db_opt_path[FN_REFLEN];

  strxnmov(db_opt_path, sizeof (db_opt_path) - 1, mysql_data_home, "/",
           db_name, "/", MY_DB_OPT_FILE, NullS);

  unpack_filename(db_opt_path, db_opt_path);

  return load_db_opt(thd, db_opt_path, db_create_info);
}


/*
  Create a database

  SYNOPSIS
  mysql_create_db()
  thd		Thread handler
  db		Name of database to create
		Function assumes that this is already validated.
  create_info	Database create options (like character set)
  silent	Used by replication when internally creating a database.
		In this case the entry should not be logged.

  SIDE-EFFECTS
   1. Report back to client that command succeeded (send_ok)
   2. Report errors to client
   3. Log event to binary log
   (The 'silent' flags turns off 1 and 3.)

  RETURN VALUES
  FALSE ok
  TRUE  Error

*/

bool mysql_create_db(THD *thd, char *db, HA_CREATE_INFO *create_info,
                     bool silent)
{
  char	 path[FN_REFLEN+16];
  long result= 1;
  int error= 0;
  MY_STAT stat_info;
  uint create_options= create_info ? create_info->options : 0;
  uint path_len;
  DBUG_ENTER("mysql_create_db");

  /* do not create 'information_schema' db */
  if (!my_strcasecmp(system_charset_info, db, information_schema_name.str))
  {
    my_error(ER_DB_CREATE_EXISTS, MYF(0), db);
    DBUG_RETURN(-1);
  }

  /*
    Do not create database if another thread is holding read lock.
    Wait for global read lock before acquiring LOCK_mysql_create_db.
    After wait_if_global_read_lock() we have protection against another
    global read lock. If we would acquire LOCK_mysql_create_db first,
    another thread could step in and get the global read lock before we
    reach wait_if_global_read_lock(). If this thread tries the same as we
    (admin a db), it would then go and wait on LOCK_mysql_create_db...
    Furthermore wait_if_global_read_lock() checks if the current thread
    has the global read lock and refuses the operation with
    ER_CANT_UPDATE_WITH_READLOCK if applicable.
  */
  if (wait_if_global_read_lock(thd, 0, 1))
  {
    error= -1;
    goto exit2;
  }

  VOID(pthread_mutex_lock(&LOCK_mysql_create_db));

  /* Check directory */
  strxmov(path, mysql_data_home, "/", db, NullS);
  path_len= unpack_dirname(path,path);    // Convert if not unix
  path[path_len-1]= 0;                    // Remove last '/' from path

  if (my_stat(path,&stat_info,MYF(0)))
  {
    if (!(create_options & HA_LEX_CREATE_IF_NOT_EXISTS))
    {
      my_error(ER_DB_CREATE_EXISTS, MYF(0), db);
      error= -1;
      goto exit;
    }
    push_warning_printf(thd, MYSQL_ERROR::WARN_LEVEL_NOTE,
			ER_DB_CREATE_EXISTS, ER(ER_DB_CREATE_EXISTS), db);
    if (!silent)
      send_ok(thd);
    error= 0;
    goto exit;
  }
  else
  {
    if (my_errno != ENOENT)
    {
      my_error(EE_STAT, MYF(0), path, my_errno);
      goto exit;
    }
    if (my_mkdir(path,0777,MYF(0)) < 0)
    {
      my_error(ER_CANT_CREATE_DB, MYF(0), db, my_errno);
      error= -1;
      goto exit;
    }
  }

  path[path_len-1]= FN_LIBCHAR;
  strmake(path+path_len, MY_DB_OPT_FILE, sizeof(path)-path_len-1);
  if (write_db_opt(thd, path, create_info))
  {
    /*
      Could not create options file.
      Restore things to beginning.
    */
    path[path_len]= 0;
    if (rmdir(path) >= 0)
    {
      error= -1;
      goto exit;
    }
    /*
      We come here when we managed to create the database, but not the option
      file.  In this case it's best to just continue as if nothing has
      happened.  (This is a very unlikely senario)
    */
  }
  
  if (!silent)
  {
    char *query;
    uint query_length;

    if (!thd->query)				// Only in replication
    {
      query= 	     path;
      query_length= (uint) (strxmov(path,"create database `", db, "`", NullS) -
			    path);
    }
    else
    {
      query= 	    thd->query;
      query_length= thd->query_length;
    }
    if (mysql_bin_log.is_open())
    {
      Query_log_event qinfo(thd, query, query_length, 0, 
			    /* suppress_use */ TRUE);

      /*
	Write should use the database being created as the "current
        database" and not the threads current database, which is the
        default. If we do not change the "current database" to the
        database being created, the CREATE statement will not be
        replicated when using --binlog-do-db to select databases to be
        replicated. 

	An example (--binlog-do-db=sisyfos):
       
          CREATE DATABASE bob;        # Not replicated
          USE bob;                    # 'bob' is the current database
          CREATE DATABASE sisyfos;    # Not replicated since 'bob' is
                                      # current database.
          USE sisyfos;                # Will give error on slave since
                                      # database does not exist.
      */
      qinfo.db     = db;
      qinfo.db_len = strlen(db);

      mysql_bin_log.write(&qinfo);
    }
    send_ok(thd, result);
  }

exit:
  VOID(pthread_mutex_unlock(&LOCK_mysql_create_db));
  start_waiting_global_read_lock(thd);
exit2:
  DBUG_RETURN(error);
}


/* db-name is already validated when we come here */

bool mysql_alter_db(THD *thd, const char *db, HA_CREATE_INFO *create_info)
{
  char path[FN_REFLEN+16];
  long result=1;
  int error= 0;
  DBUG_ENTER("mysql_alter_db");

  /*
    Do not alter database if another thread is holding read lock.
    Wait for global read lock before acquiring LOCK_mysql_create_db.
    After wait_if_global_read_lock() we have protection against another
    global read lock. If we would acquire LOCK_mysql_create_db first,
    another thread could step in and get the global read lock before we
    reach wait_if_global_read_lock(). If this thread tries the same as we
    (admin a db), it would then go and wait on LOCK_mysql_create_db...
    Furthermore wait_if_global_read_lock() checks if the current thread
    has the global read lock and refuses the operation with
    ER_CANT_UPDATE_WITH_READLOCK if applicable.
  */
  if ((error=wait_if_global_read_lock(thd,0,1)))
    goto exit2;

  VOID(pthread_mutex_lock(&LOCK_mysql_create_db));

  /* Check directory */
  strxmov(path, mysql_data_home, "/", db, "/", MY_DB_OPT_FILE, NullS);
  fn_format(path, path, "", "", MYF(MY_UNPACK_FILENAME));
  if ((error=write_db_opt(thd, path, create_info)))
    goto exit;

  /*
     Change options if current database is being altered
     TODO: Delete this code
  */
  if (thd->db && !strcmp(thd->db,db))
  {
    thd->db_charset= create_info->default_table_charset ?
		     create_info->default_table_charset :
		     thd->variables.collation_server;
    thd->variables.collation_database= thd->db_charset;
  }

  if (mysql_bin_log.is_open())
  {
    Query_log_event qinfo(thd, thd->query, thd->query_length, 0,
			  /* suppress_use */ TRUE);

    /*
      Write should use the database being created as the "current
      database" and not the threads current database, which is the
      default.
    */
    qinfo.db     = db;
    qinfo.db_len = strlen(db);

    thd->clear_error();
    mysql_bin_log.write(&qinfo);
  }
  send_ok(thd, result);

exit:
  VOID(pthread_mutex_unlock(&LOCK_mysql_create_db));
  start_waiting_global_read_lock(thd);
exit2:
  DBUG_RETURN(error);
}


/*
  Drop all tables in a database and the database itself

  SYNOPSIS
    mysql_rm_db()
    thd			Thread handle
    db			Database name in the case given by user
		        It's already validated and set to lower case
                        (if needed) when we come here
    if_exists		Don't give error if database doesn't exists
    silent		Don't generate errors

  RETURN
    FALSE ok (Database dropped)
    ERROR Error
*/

bool mysql_rm_db(THD *thd,char *db,bool if_exists, bool silent)
{
  long deleted=0;
  int error= 0;
  char	path[FN_REFLEN+16];
  MY_DIR *dirp;
  uint length;
  TABLE_LIST* dropped_tables= 0;
  DBUG_ENTER("mysql_rm_db");

  /*
    Do not drop database if another thread is holding read lock.
    Wait for global read lock before acquiring LOCK_mysql_create_db.
    After wait_if_global_read_lock() we have protection against another
    global read lock. If we would acquire LOCK_mysql_create_db first,
    another thread could step in and get the global read lock before we
    reach wait_if_global_read_lock(). If this thread tries the same as we
    (admin a db), it would then go and wait on LOCK_mysql_create_db...
    Furthermore wait_if_global_read_lock() checks if the current thread
    has the global read lock and refuses the operation with
    ER_CANT_UPDATE_WITH_READLOCK if applicable.
  */
  if (wait_if_global_read_lock(thd, 0, 1))
  {
    error= -1;
    goto exit2;
  }

  VOID(pthread_mutex_lock(&LOCK_mysql_create_db));

  (void) sprintf(path,"%s/%s",mysql_data_home,db);
  length= unpack_dirname(path,path);		// Convert if not unix
  strmov(path+length, MY_DB_OPT_FILE);		// Append db option file name
  del_dbopt(path);				// Remove dboption hash entry
  path[length]= '\0';				// Remove file name

  /* See if the directory exists */
  if (!(dirp= my_dir(path,MYF(MY_DONT_SORT))))
  {
    if (!if_exists)
    {
      error= -1;
      my_error(ER_DB_DROP_EXISTS, MYF(0), db);
      goto exit;
    }
    else
      push_warning_printf(thd, MYSQL_ERROR::WARN_LEVEL_NOTE,
			  ER_DB_DROP_EXISTS, ER(ER_DB_DROP_EXISTS), db);
  }
  else
  {
    pthread_mutex_lock(&LOCK_open);
    remove_db_from_cache(db);
    pthread_mutex_unlock(&LOCK_open);

    
    error= -1;
    if ((deleted= mysql_rm_known_files(thd, dirp, db, path, 0,
                                       &dropped_tables)) >= 0)
    {
      ha_drop_database(path);
      query_cache_invalidate1(db);
      error = 0;
    }
  }
  if (!silent && deleted>=0)
  {
    const char *query;
    ulong query_length;
    if (!thd->query)
    {
      /* The client used the old obsolete mysql_drop_db() call */
      query= path;
      query_length= (uint) (strxmov(path, "drop database `", db, "`",
                                     NullS) - path);
    }
    else
    {
      query =thd->query;
      query_length= thd->query_length;
    }
    if (mysql_bin_log.is_open())
    {
      Query_log_event qinfo(thd, query, query_length, 0, 
			    /* suppress_use */ TRUE);
      /*
        Write should use the database being created as the "current
        database" and not the threads current database, which is the
        default.
      */
      qinfo.db     = db;
      qinfo.db_len = strlen(db);

      thd->clear_error();
      mysql_bin_log.write(&qinfo);
    }
    thd->server_status|= SERVER_STATUS_DB_DROPPED;
    send_ok(thd, (ulong) deleted);
    thd->server_status&= ~SERVER_STATUS_DB_DROPPED;
  }
  else if (mysql_bin_log.is_open())
  {
    char *query, *query_pos, *query_end, *query_data_start;
    TABLE_LIST *tbl;
    uint db_len;

    if (!(query= thd->alloc(MAX_DROP_TABLE_Q_LEN)))
      goto exit; /* not much else we can do */
    query_pos= query_data_start= strmov(query,"drop table ");
    query_end= query + MAX_DROP_TABLE_Q_LEN;
    db_len= strlen(db);

    for (tbl= dropped_tables; tbl; tbl= tbl->next_local)
    {
      uint tbl_name_len;

      /* 3 for the quotes and the comma*/
      tbl_name_len= strlen(tbl->table_name) + 3;
      if (query_pos + tbl_name_len + 1 >= query_end)
      {
        write_to_binlog(thd, query, query_pos -1 - query, db, db_len);
        query_pos= query_data_start;
      }

      *query_pos++ = '`';
      query_pos= strmov(query_pos,tbl->table_name);
      *query_pos++ = '`';
      *query_pos++ = ',';
    }

    if (query_pos != query_data_start)
    {
      write_to_binlog(thd, query, query_pos -1 - query, db, db_len);
    }
  }

exit:
  (void)sp_drop_db_routines(thd, db); /* QQ Ignore errors for now  */
  /*
    If this database was the client's selected database, we silently
    change the client's selected database to nothing (to have an empty
    SELECT DATABASE() in the future). For this we free() thd->db and set
    it to 0.
  */
  if (thd->db && !strcmp(thd->db, db))
    thd->set_db(NULL, 0);
  VOID(pthread_mutex_unlock(&LOCK_mysql_create_db));
  start_waiting_global_read_lock(thd);
exit2:
  DBUG_RETURN(error);
}

/*
  Removes files with known extensions plus all found subdirectories that
  are 2 hex digits (raid directories).
  thd MUST be set when calling this function!
*/

static long mysql_rm_known_files(THD *thd, MY_DIR *dirp, const char *db,
				 const char *org_path, uint level,
                                 TABLE_LIST **dropped_tables)
{
  long deleted=0;
  ulong found_other_files=0;
  char filePath[FN_REFLEN];
  TABLE_LIST *tot_list=0, **tot_list_next;
  List<String> raid_dirs;
  DBUG_ENTER("mysql_rm_known_files");
  DBUG_PRINT("enter",("path: %s", org_path));

  tot_list_next= &tot_list;

  for (uint idx=0 ;
       idx < (uint) dirp->number_off_files && !thd->killed ;
       idx++)
  {
    FILEINFO *file=dirp->dir_entry+idx;
    char *extension;
    DBUG_PRINT("info",("Examining: %s", file->name));

    /* skiping . and .. */
    if (file->name[0] == '.' && (!file->name[1] ||
       (file->name[1] == '.' &&  !file->name[2])))
      continue;

    /* Check if file is a raid directory */
    if ((my_isdigit(system_charset_info, file->name[0]) ||
	 (file->name[0] >= 'a' && file->name[0] <= 'f')) &&
	(my_isdigit(system_charset_info, file->name[1]) ||
	 (file->name[1] >= 'a' && file->name[1] <= 'f')) &&
	!file->name[2] && !level)
    {
      char newpath[FN_REFLEN], *copy_of_path;
      MY_DIR *new_dirp;
      String *dir;
      uint length;

      strxmov(newpath,org_path,"/",file->name,NullS);
      length= unpack_filename(newpath,newpath);
      if ((new_dirp = my_dir(newpath,MYF(MY_DONT_SORT))))
      {
	DBUG_PRINT("my",("New subdir found: %s", newpath));
	if ((mysql_rm_known_files(thd, new_dirp, NullS, newpath,1,0)) < 0)
	  goto err;
	if (!(copy_of_path= thd->memdup(newpath, length+1)) ||
	    !(dir= new (thd->mem_root) String(copy_of_path, length,
					       &my_charset_bin)) ||
	    raid_dirs.push_back(dir))
	  goto err;
	continue;
      }
      found_other_files++;
      continue;
    }
    else if (file->name[0] == 'a' && file->name[1] == 'r' &&
             file->name[2] == 'c' && file->name[3] == '\0')
    {
      /* .frm archive */
      char newpath[FN_REFLEN];
      MY_DIR *new_dirp;
      strxmov(newpath, org_path, "/", "arc", NullS);
      (void) unpack_filename(newpath, newpath);
      if ((new_dirp = my_dir(newpath, MYF(MY_DONT_SORT))))
      {
	DBUG_PRINT("my",("Archive subdir found: %s", newpath));
	if ((mysql_rm_arc_files(thd, new_dirp, newpath)) < 0)
	  goto err;
	continue;
      }
      found_other_files++;
      continue;
    }
    extension= fn_ext(file->name);
    if (find_type(extension, &deletable_extentions,1+2) <= 0)
    {
      if (find_type(extension, ha_known_exts(),1+2) <= 0)
	found_other_files++;
      continue;
    }
    /* just for safety we use files_charset_info */
    if (db && !my_strcasecmp(files_charset_info,
                             extension, reg_ext))
    {
      /* Drop the table nicely */
      *extension= 0;			// Remove extension
      TABLE_LIST *table_list=(TABLE_LIST*)
	thd->calloc(sizeof(*table_list)+ strlen(db)+strlen(file->name)+2);
      if (!table_list)
	goto err;
      table_list->db= (char*) (table_list+1);
      strmov(table_list->table_name= strmov(table_list->db,db)+1, file->name);
      table_list->alias= table_list->table_name;	// If lower_case_table_names=2
      /* Link into list */
      (*tot_list_next)= table_list;
      tot_list_next= &table_list->next_local;
      deleted++;
    }
    else
    {
      strxmov(filePath, org_path, "/", file->name, NullS);
      if (my_delete_with_symlink(filePath,MYF(MY_WME)))
      {
	goto err;
      }
    }
  }
  if (thd->killed ||
      (tot_list && mysql_rm_table_part2_with_lock(thd, tot_list, 1, 0, 1)))
    goto err;

  /* Remove RAID directories */
  {
    List_iterator<String> it(raid_dirs);
    String *dir;
    while ((dir= it++))
      if (rmdir(dir->c_ptr()) < 0)
	found_other_files++;
  }
  my_dirend(dirp);  
  
  if (dropped_tables)
    *dropped_tables= tot_list;
  
  /*
    If the directory is a symbolic link, remove the link first, then
    remove the directory the symbolic link pointed at
  */
  if (found_other_files)
  {
    my_error(ER_DB_DROP_RMDIR, MYF(0), org_path, EEXIST);
    DBUG_RETURN(-1);
  }
  else
  {
    /* Don't give errors if we can't delete 'RAID' directory */
    if (rm_dir_w_symlink(org_path, level == 0))
      DBUG_RETURN(-1);
  }

  DBUG_RETURN(deleted);

err:
  my_dirend(dirp);
  DBUG_RETURN(-1);
}


/*
  Remove directory with symlink

  SYNOPSIS
    rm_dir_w_symlink()
    org_path    path of derictory
    send_error  send errors
  RETURN
    0 OK
    1 ERROR
*/

static my_bool rm_dir_w_symlink(const char *org_path, my_bool send_error)
{
  char tmp_path[FN_REFLEN], *pos;
  char *path= tmp_path;
  DBUG_ENTER("rm_dir_w_symlink");
  unpack_filename(tmp_path, org_path);
#ifdef HAVE_READLINK
  int error;
  char tmp2_path[FN_REFLEN];

  /* Remove end FN_LIBCHAR as this causes problem on Linux in readlink */
  pos= strend(path);
  if (pos > path && pos[-1] == FN_LIBCHAR)
    *--pos=0;

  if ((error= my_readlink(tmp2_path, path, MYF(MY_WME))) < 0)
    DBUG_RETURN(1);
  if (!error)
  {
    if (my_delete(path, MYF(send_error ? MY_WME : 0)))
    {
      DBUG_RETURN(send_error);
    }
    /* Delete directory symbolic link pointed at */
    path= tmp2_path;
  }
#endif
  /* Remove last FN_LIBCHAR to not cause a problem on OS/2 */
  pos= strend(path);

  if (pos > path && pos[-1] == FN_LIBCHAR)
    *--pos=0;
  if (rmdir(path) < 0 && send_error)
  {
    my_error(ER_DB_DROP_RMDIR, MYF(0), path, errno);
    DBUG_RETURN(1);
  }
  DBUG_RETURN(0);
}


/*
  Remove .frm archives from directory

  SYNOPSIS
    thd       thread handler
    dirp      list of files in archive directory
    db        data base name
    org_path  path of archive directory

  RETURN
    > 0 number of removed files
    -1  error
*/
static long mysql_rm_arc_files(THD *thd, MY_DIR *dirp,
				 const char *org_path)
{
  long deleted= 0;
  ulong found_other_files= 0;
  char filePath[FN_REFLEN];
  DBUG_ENTER("mysql_rm_arc_files");
  DBUG_PRINT("enter", ("path: %s", org_path));

  for (uint idx=0 ;
       idx < (uint) dirp->number_off_files && !thd->killed ;
       idx++)
  {
    FILEINFO *file=dirp->dir_entry+idx;
    char *extension, *revision;
    DBUG_PRINT("info",("Examining: %s", file->name));

    /* skiping . and .. */
    if (file->name[0] == '.' && (!file->name[1] ||
       (file->name[1] == '.' &&  !file->name[2])))
      continue;

    extension= fn_ext(file->name);
    if (extension[0] != '.' ||
        extension[1] != 'f' || extension[2] != 'r' ||
        extension[3] != 'm' || extension[4] != '-')
    {
      found_other_files++;
      continue;
    }
    revision= extension+5;
    while (*revision && my_isdigit(system_charset_info, *revision))
      revision++;
    if (*revision)
    {
      found_other_files++;
      continue;
    }
    strxmov(filePath, org_path, "/", file->name, NullS);
    if (my_delete_with_symlink(filePath,MYF(MY_WME)))
    {
      goto err;
    }
  }
  if (thd->killed)
    goto err;

  my_dirend(dirp);

  /*
    If the directory is a symbolic link, remove the link first, then
    remove the directory the symbolic link pointed at
  */
  if (!found_other_files &&
      rm_dir_w_symlink(org_path, 0))
    DBUG_RETURN(-1);
  DBUG_RETURN(deleted);

err:
  my_dirend(dirp);
  DBUG_RETURN(-1);
}


/*
  Change the current database.

  SYNOPSIS
    mysql_change_db()
    thd			thread handle
    name		database name
    no_access_check	if TRUE, don't do access check. In this
                        case name may be ""

  DESCRIPTION
    Check that the database name corresponds to a valid and
    existent database, check access rights (unless called with
    no_access_check), and set the current database. This function
    is called to change the current database upon user request
    (COM_CHANGE_DB command) or temporarily, to execute a stored
    routine.

  NOTES
    This function is not the only way to switch the database that
    is currently employed. When the replication slave thread
    switches the database before executing a query, it calls
    thd->set_db directly. However, if the query, in turn, uses
    a stored routine, the stored routine will use this function,
    even if it's run on the slave.

    This function allocates the name of the database on the system
    heap: this is necessary to be able to uniformly change the
    database from any module of the server. Up to 5.0 different
    modules were using different memory to store the name of the
    database, and this led to memory corruption: a stack pointer
    set by Stored Procedures was used by replication after the
    stack address was long gone.

    This function does not send anything, including error
    messages, to the client. If that should be sent to the client,
    call net_send_error after this function.

  RETURN VALUES
    0	OK
    1	error
*/

bool mysql_change_db(THD *thd, const char *name, bool no_access_check)
{
  int path_length, db_length;
  char *db_name;
  bool system_db= 0;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  ulong db_access;
  Security_context *sctx= thd->security_ctx;
  LINT_INIT(db_access);
#endif
  DBUG_ENTER("mysql_change_db");
  DBUG_PRINT("enter",("name: '%s'",name));

  if (name == NULL || name[0] == '\0' && no_access_check == FALSE)
  {
    my_message(ER_NO_DB_ERROR, ER(ER_NO_DB_ERROR), MYF(0));
    DBUG_RETURN(1);				/* purecov: inspected */
  }
  else if (name[0] == '\0')
  {
    /* Called from SP to restore the original database, which was NULL */
    DBUG_ASSERT(no_access_check);
    system_db= 1;
    db_name= NULL;
    db_length= 0;
    goto end;
  }
  /*
    Now we need to make a copy because check_db_name requires a
    non-constant argument. TODO: fix check_db_name.
  */
  if ((db_name= my_strdup(name, MYF(MY_WME))) == NULL)
    DBUG_RETURN(1);                             /* the error is set */
  db_length= strlen(db_name);
  if (check_db_name(db_name))
  {
    my_error(ER_WRONG_DB_NAME, MYF(0), db_name);
    my_free(db_name, MYF(0));
    DBUG_RETURN(1);
  }
  DBUG_PRINT("info",("Use database: %s", db_name));
  if (!my_strcasecmp(system_charset_info, db_name, information_schema_name.str))
  {
    system_db= 1;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
    db_access= SELECT_ACL;
#endif
    goto end;
  }

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  if (!no_access_check)
  {
    if (test_all_bits(sctx->master_access, DB_ACLS))
      db_access=DB_ACLS;
    else
      db_access= (acl_get(sctx->host, sctx->ip, sctx->priv_user, db_name, 0) |
                  sctx->master_access);
    if (!(db_access & DB_ACLS) && (!grant_option ||
                                   check_grant_db(thd,db_name)))
    {
      my_error(ER_DBACCESS_DENIED_ERROR, MYF(0),
               sctx->priv_user,
               sctx->priv_host,
               db_name);
      mysql_log.write(thd, COM_INIT_DB, ER(ER_DBACCESS_DENIED_ERROR),
                      sctx->priv_user, sctx->priv_host, db_name);
      my_free(db_name, MYF(0));
      DBUG_RETURN(1);
    }
  }
#endif

  if (check_db_dir_existence(db_name))
  {
    my_error(ER_BAD_DB_ERROR, MYF(0), db_name);
    my_free(db_name, MYF(0));
    DBUG_RETURN(1);
  }

end:
  x_free(thd->db);
  DBUG_ASSERT(db_name == NULL || db_name[0] != '\0');
  thd->reset_db(db_name, db_length);            // THD::~THD will free this
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  if (!no_access_check)
    sctx->db_access= db_access;
#endif
  if (system_db)
  {
    thd->db_charset= system_charset_info;
    thd->variables.collation_database= system_charset_info;
  }
  else
  {
    HA_CREATE_INFO create;

    load_db_opt_by_name(thd, db_name, &create);

    thd->db_charset= create.default_table_charset ?
      create.default_table_charset :
      thd->variables.collation_server;
    thd->variables.collation_database= thd->db_charset;
  }
  DBUG_RETURN(0);
}


/*
  Check if there is directory for the database name.

  SYNOPSIS
    check_db_dir_existence()
    db_name   database name

  RETURN VALUES
    FALSE   There is directory for the specified database name.
    TRUE    The directory does not exist.
*/

bool check_db_dir_existence(const char *db_name)
{
  char db_dir_path[FN_REFLEN];
  uint db_dir_path_len;

  strxnmov(db_dir_path, sizeof (db_dir_path) - 1, mysql_data_home, "/",
           db_name, NullS);

  db_dir_path_len= unpack_dirname(db_dir_path, db_dir_path);

  /* Remove trailing '/' or '\' if exists. */

  if (db_dir_path_len && db_dir_path[db_dir_path_len - 1] == FN_LIBCHAR)
    db_dir_path[db_dir_path_len - 1]= 0;

  /* Check access. */

  return my_access(db_dir_path, F_OK);
}
