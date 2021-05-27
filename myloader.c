/*
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    Authors:        Andrew Hutchings, SkySQL (andrew at skysql dot com)
*/

#define _LARGEFILE64_SOURCE
#define _FILE_OFFSET_BITS 64

#include <mysql.h>

#if defined MARIADB_CLIENT_VERSION_STR && !defined MYSQL_SERVER_VERSION
#define MYSQL_SERVER_VERSION MARIADB_CLIENT_VERSION_STR
#endif

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <zlib.h>
#include "config.h"
#include "common.h"
#include "myloader.h"
#include "connection.h"
#include "getPassword.h"
#include "logging.h"
#include "set_verbose.h"

guint commit_count = 1000;
gchar *directory = NULL;
gboolean overwrite_tables = FALSE;
gboolean innodb_optimize_keys = FALSE;
gboolean enable_binlog = FALSE;
gboolean sync_before_add_index = FALSE;
gboolean disable_redo_log = FALSE;
guint rows = 0;
gchar *source_db = NULL;
gchar *purge_mode_str=NULL;
gchar *set_names_str=NULL;
GHashTable *tables;
enum purge_mode purge_mode;
static GMutex *init_mutex = NULL;
static GMutex *progress_mutex = NULL;
guint errors = 0;
unsigned long long int total_data_sql_files = 0;
unsigned long long int progress = 0;
gboolean read_data(FILE *file, gboolean is_compressed, GString *data,
                   gboolean *eof);
void restore_data_from_file(MYSQL *conn, char *database, char *table,
                  const char *filename, gboolean is_schema, gboolean need_use, 
		  gboolean is_create_table, GAsyncQueue *fast_index_creation_queue, GAsyncQueue *constraints_queue);
void restore_data_in_gstring_from_file(MYSQL *conn, char *database, char *table, 
		  GString *data, const char *filename, gboolean is_schema, 
		  guint *query_counter);
void restore_data_in_gstring(MYSQL *conn, char *database, char *table, GString *data, const char *filename, gboolean is_schema, guint *query_counter);
void *process_queue(struct thread_data *td);
void add_schema(const gchar *filename, GAsyncQueue *fast_index_creation_queue, GAsyncQueue *constraints_queue, MYSQL *conn);
void restore_databases(struct configuration *conf, MYSQL *conn);
void restore_schema_view(MYSQL *conn);
void restore_schema_triggers(MYSQL *conn);
void restore_schema_post(MYSQL *conn);
void no_log(const gchar *log_domain, GLogLevelFlags log_level,
            const gchar *message, gpointer user_data);
void create_database(MYSQL *conn, gchar *database);
gint compare_restore_job(gconstpointer a, gconstpointer b);

static GOptionEntry entries[] = {
    {"directory", 'd', 0, G_OPTION_ARG_STRING, &directory,
     "Directory of the dump to import", NULL},
    {"queries-per-transaction", 'q', 0, G_OPTION_ARG_INT, &commit_count,
     "Number of queries per transaction, default 1000", NULL},
    {"overwrite-tables", 'o', 0, G_OPTION_ARG_NONE, &overwrite_tables,
     "Drop tables if they already exist", NULL},
    {"database", 'B', 0, G_OPTION_ARG_STRING, &db,
     "An alternative database to restore into", NULL},
    {"source-db", 's', 0, G_OPTION_ARG_STRING, &source_db,
     "Database to restore", NULL},
    {"enable-binlog", 'e', 0, G_OPTION_ARG_NONE, &enable_binlog,
     "Enable binary logging of the restore data", NULL},
    {"innodb-optimize-keys", 0, 0, G_OPTION_ARG_NONE, &innodb_optimize_keys,
     "Creates the table without the indexes and it adds them at the end", NULL},
    { "set-names",0, 0, G_OPTION_ARG_STRING, &set_names_str, 
      "Sets the names, use it at your own risk, default binary", NULL },
    {"logfile", 'L', 0, G_OPTION_ARG_FILENAME, &logfile,
     "Log file name to use, by default stdout is used", NULL},
    { "purge-mode", 0, 0, G_OPTION_ARG_STRING, &purge_mode_str, 
      "This specify the truncate mode which can be: NONE, DROP, TRUNCATE and DELETE", NULL },
    { "sync-before-add-index", 0, 0, G_OPTION_ARG_NONE, &sync_before_add_index,
      "If --innodb-optimize-keys is used, this option will force all the data threads to complete before starting the create index phase", NULL },
    { "disable-redo-log", 0, 0, G_OPTION_ARG_NONE, &disable_redo_log,
      "Disables the REDO_LOG and enables it after, doesn't check initial status", NULL },
    {"rows", 'r', 0, G_OPTION_ARG_INT, &rows,
     "Split the INSERT statement into this many rows.", NULL},
    {NULL, 0, 0, G_OPTION_ARG_NONE, NULL, NULL, NULL}};


void split_and_restore_data_in_gstring_from_file(MYSQL *conn, char *database, char *table,
                  const char *filename, GString *data, gboolean is_schema, guint *query_counter)
{
  char *next_line=g_strstr_len(data->str,-1,"\n");
  char *insert_statement=g_strndup(data->str, next_line - data->str);
  gchar *current_line=next_line+1;
  next_line=g_strstr_len(current_line, -1, "\n");
  GString * new_insert=g_string_sized_new(strlen(insert_statement));
  do {
    guint current_rows=0;
    new_insert=g_string_append(new_insert,insert_statement);
    do {
      char *line=g_strndup(current_line, next_line - current_line);
      g_string_append(new_insert, line);
      g_free(line);
      current_rows++;
      current_line=next_line+1;
      next_line=g_strstr_len(current_line, -1, "\n");
    } while (current_rows < rows && next_line != NULL);
    new_insert->str[new_insert->len - 1]=';';
    restore_data_in_gstring_from_file(conn, database, table, new_insert, filename, is_schema, query_counter);
  } while (next_line != NULL);
  g_string_free(new_insert,TRUE);
  g_string_set_size(data, 0);
  
}

struct job * new_job (enum job_type type, void *job_data) {
  struct job *j = g_new0(struct job, 1);
  j->type = type;
  switch (type){
    case JOB_WAIT:
      j->job_data = (GAsyncQueue *)job_data;
    case JOB_SHUTDOWN:
      break;
    default:
      j->job_data = (struct restore_job *)job_data;
  }
  return j;
}

struct restore_job * new_restore_job(char * filename, char * database, char * table, GString * statement, guint part){
  struct restore_job *rj = g_new(struct restore_job, 1);
  rj->filename= filename;
  rj->statement = statement;
  rj->database = database;
  rj->table = table;
  rj->part = part;
  return rj;
}
void sync_threads(struct configuration * conf){
  guint n;
  GAsyncQueue * queue = g_async_queue_new();
  for (n = 0; n < num_threads; n++)
    g_async_queue_push(conf->queue, new_job(JOB_WAIT, queue));
  for (n = 0; n < num_threads; n++)
    g_async_queue_pop(conf->ready);
  for (n = 0; n < num_threads; n++)
    g_async_queue_push(queue, GINT_TO_POINTER(1));
}

int main(int argc, char *argv[]) {
  struct configuration conf = {NULL, NULL, NULL, NULL, NULL, 0};

  GError *error = NULL;
  GOptionContext *context;

  g_thread_init(NULL);

  init_mutex = g_mutex_new();
  progress_mutex = g_mutex_new();

  if (db == NULL && source_db != NULL) {
    db = g_strdup(source_db);
  }

  context = g_option_context_new("multi-threaded MySQL loader");
  GOptionGroup *main_group =
      g_option_group_new("main", "Main Options", "Main Options", NULL, NULL);
  g_option_group_add_entries(main_group, entries);
  g_option_group_add_entries(main_group, common_entries);
  g_option_context_set_main_group(context, main_group);
  gchar ** tmpargv=g_strdupv(argv);
  int tmpargc=argc;
  if (!g_option_context_parse(context, &tmpargc, &tmpargv, &error)) {
    g_print("option parsing failed: %s, try --help\n", error->message);
    exit(EXIT_FAILURE);
  }
  g_option_context_free(context);

  if (password != NULL){
    int i=1;
    for(i=1; i < argc; i++){
      gchar * p= g_strstr_len(argv[i],-1,password);
      if (p != NULL){
        strncpy(p, "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX", strlen(password));
      }
    }
  }
  // prompt for password if it's NULL
  if (sizeof(password) == 0 || (password == NULL && askPassword)) {
    password = passwordPrompt();
  }

  tables = g_hash_table_new ( g_str_hash, g_str_equal );

  if (program_version) {
    g_print("myloader %s, built against MySQL %s\n", VERSION,
            MYSQL_VERSION_STR);
    exit(EXIT_SUCCESS);
  }

  set_verbose(verbose);

  if (set_names_str){
    gchar *tmp_str=g_strdup_printf("/*!40101 SET NAMES %s*/",set_names_str);
    set_names_str=tmp_str;
  } else 
    set_names_str=g_strdup("/*!40101 SET NAMES binary*/");

  if (purge_mode_str){
    if (!strcmp(purge_mode_str,"TRUNCATE")){
      purge_mode=TRUNCATE;
    } else if (!strcmp(purge_mode_str,"DROP")){
      purge_mode=DROP;
    } else if (!strcmp(purge_mode_str,"DELETE")){
      purge_mode=DELETE;
    } else if (!strcmp(purge_mode_str,"NONE")){
      purge_mode=NONE;
    } else {
      g_error("Purge mode unknown");
    }
  } else if (overwrite_tables) 
    purge_mode=DROP; // Default mode is DROP when overwrite_tables is especified
  else purge_mode=NONE;
  
  if (!directory) {
    g_critical("a directory needs to be specified, see --help\n");
    exit(EXIT_FAILURE);
  } else {
    if (!g_file_test(directory,G_FILE_TEST_IS_DIR)){
      g_critical("the specified directory doesn't exists\n");
      exit(EXIT_FAILURE);
    }
    char *p = g_strdup_printf("%s/metadata", directory);
    if (!g_file_test(p, G_FILE_TEST_EXISTS)) {
      g_critical("the specified directory is not a mydumper backup\n");
      exit(EXIT_FAILURE);
    }
  }
  MYSQL *conn;
  conn = mysql_init(NULL);

  configure_connection(conn, "myloader");
  if (!mysql_real_connect(conn, hostname, username, password, NULL, port,
                          socket_path, 0)) {
    g_critical("Error connection to database: %s", mysql_error(conn));
    exit(EXIT_FAILURE);
  }

  if (mysql_query(conn, "SET SESSION wait_timeout = 2147483")) {
    g_warning("Failed to increase wait_timeout: %s", mysql_error(conn));
  }

  if (!enable_binlog)
    mysql_query(conn, "SET SQL_LOG_BIN=0");

  if (disable_redo_log){
    g_message("Disabling redologs");
    mysql_query(conn, "ALTER INSTANCE DISABLE INNODB REDO_LOG");
  }
  mysql_query(conn, "/*!40014 SET FOREIGN_KEY_CHECKS=0*/");
  conf.queue = g_async_queue_new();
  conf.ready = g_async_queue_new();
  conf.fast_index_creation_queue = g_async_queue_new();
  conf.constraints_queue = g_async_queue_new();

  guint n;
  GThread **threads = g_new(GThread *, num_threads);
  struct thread_data *td = g_new(struct thread_data, num_threads);
  for (n = 0; n < num_threads; n++) {
    td[n].conf = &conf;
    td[n].thread_id = n + 1;
    threads[n] =
        g_thread_create((GThreadFunc)process_queue, &td[n], TRUE, NULL);
    g_async_queue_pop(conf.ready);
  }

  g_message("%d threads created", num_threads);

  restore_databases(&conf, conn);
  // All jobs to restore has been added we need to let them all sync before start adding more jobs
  if (sync_before_add_index) {
    sync_threads(&conf);
  }

  // move from a queue to the done query
  struct job *job =(struct job *)g_async_queue_try_pop(conf.fast_index_creation_queue);
  while (job != NULL){
    g_async_queue_push(conf.queue, job);
    job =(struct job *)g_async_queue_try_pop(conf.fast_index_creation_queue);
  }

  sync_threads(&conf);

  // move from a queue to the done query
  job =(struct job *)g_async_queue_try_pop(conf.constraints_queue);
  while (job != NULL){
    g_async_queue_push(conf.queue, job);
    job =(struct job *)g_async_queue_try_pop(conf.constraints_queue);
  }


  for (n = 0; n < num_threads; n++) {
    g_async_queue_push(conf.queue, new_job(JOB_SHUTDOWN,NULL));
  }

  for (n = 0; n < num_threads; n++) {
    g_thread_join(threads[n]);
  }

  g_async_queue_unref(conf.ready);

  restore_schema_post(conn);

  restore_schema_view(conn);

  restore_schema_triggers(conn);

  if (disable_redo_log)
    mysql_query(conn, "ALTER INSTANCE ENABLE INNODB REDO_LOG");

  g_async_queue_unref(conf.queue);
  mysql_close(conn);
  mysql_thread_end();
  mysql_library_end();
  g_free(directory);
  g_free(td);
  g_free(threads);

  if (logoutfile) {
    fclose(logoutfile);
  }

  return errors ? EXIT_FAILURE : EXIT_SUCCESS;
}

void restore_databases(struct configuration *conf, MYSQL *conn) {
  GError *error = NULL;
  GDir *dir = g_dir_open(directory, 0, &error);

  if (error) {
    g_critical("cannot open directory %s, %s\n", directory, error->message);
    errors++;
    return;
  }

  const gchar *filename = NULL;

  while ((filename = g_dir_read_name(dir))) {
    if (!source_db ||
        g_str_has_prefix(filename, g_strdup_printf("%s.", source_db))) {
      if (g_strrstr(filename, "-schema.sql")) {
        add_schema(filename, conf->fast_index_creation_queue, conf->constraints_queue, conn);
      }
    }
  }
  g_dir_rewind(dir);
  while ((filename = g_dir_read_name(dir))) {
    if (!source_db ||
        g_str_has_prefix(filename, g_strdup_printf("%s.", source_db))) {
      if (!g_strrstr(filename, "-schema.sql") &&
          !g_strrstr(filename, "-schema-view.sql") &&
          !g_strrstr(filename, "-schema-triggers.sql") &&
          !g_strrstr(filename, "-schema-post.sql") &&
          !g_strrstr(filename, "-schema-create.sql") &&
          g_strrstr(filename, ".sql")) {
        total_data_sql_files++;
      }
    }
  }
  g_dir_rewind(dir);
  while ((filename = g_dir_read_name(dir))) {
    if (g_str_has_suffix(filename, ".metadata")) {
        gchar **split_file = g_strsplit(filename, ".", 0);
        gchar *database = g_strdup(split_file[0]);
        gchar *table = g_strdup(split_file[1]);
        struct db_table*dbt=g_new(struct db_table,1);
        dbt->database=database;
        dbt->table=table;
        gchar * buff=NULL;
        gsize len;
        char * f=g_strdup_printf("%s/%s",directory,filename);
        g_file_get_contents(f,&buff,&len,NULL);
        dbt->rows=g_ascii_strtoll(buff,NULL,10);
        g_hash_table_insert(tables, g_strdup_printf("%s_%s",dbt->database,dbt->table),dbt);
    }
  }
  g_dir_rewind(dir);
  GList * jobs=NULL;
  while ((filename = g_dir_read_name(dir))) {
    if (!source_db ||
        g_str_has_prefix(filename, g_strdup_printf("%s.", source_db))) {
      if (!g_strrstr(filename, "-schema.sql") &&
          !g_strrstr(filename, "-schema-view.sql") &&
          !g_strrstr(filename, "-schema-triggers.sql") &&
          !g_strrstr(filename, "-schema-post.sql") &&
          !g_strrstr(filename, "-schema-create.sql") &&
          g_strrstr(filename, ".sql")) {
        gchar **split_file = g_strsplit(filename, ".", 0);
        struct restore_job *rj = new_restore_job(g_strdup(filename), g_strdup(split_file[0]), g_strdup(split_file[1]), NULL, g_ascii_strtoull(split_file[2], NULL, 10));
        jobs=g_list_insert_sorted (jobs,rj,&compare_restore_job);
      }
    }
  }
  g_dir_close(dir);
  GList *j=jobs;
  for (; j != NULL; j = j->next)
  {
    g_async_queue_push(conf->queue, new_job(JOB_RESTORE_FILENAME ,j->data) );
  }
}

void restore_schema_view(MYSQL *conn) {
  GError *error = NULL;
  GDir *dir = g_dir_open(directory, 0, &error);

  if (error) {
    g_critical("cannot open directory %s, %s\n", directory, error->message);
    errors++;
    return;
  }

  const gchar *filename = NULL;

  while ((filename = g_dir_read_name(dir))) {
    if (!source_db ||
        g_str_has_prefix(filename, g_strdup_printf("%s.", source_db))) {
      if (g_strrstr(filename, "-schema-view.sql")) {
        add_schema(filename, NULL, NULL, conn);
      }
    }
  }

  g_dir_close(dir);
}

void restore_schema_triggers(MYSQL *conn) {
  GError *error = NULL;
  GDir *dir = g_dir_open(directory, 0, &error);
  gchar **split_file = NULL;
  gchar *database = NULL;
  gchar **split_table = NULL;
  gchar *table = NULL;

  if (error) {
    g_critical("cannot open directory %s, %s\n", directory, error->message);
    errors++;
    return;
  }

  const gchar *filename = NULL;

  while ((filename = g_dir_read_name(dir))) {
    if (!source_db ||
        g_str_has_prefix(filename, g_strdup_printf("%s.", source_db))) {
      if (g_strrstr(filename, "-schema-triggers.sql")) {
        split_file = g_strsplit(filename, ".", 0);
        database = split_file[0];
        split_table = g_strsplit(split_file[1], "-schema", 0);
        table = split_table[0];
        g_message("Restoring triggers for `%s`.`%s`", db ? db : database,
                  table);
        restore_data_from_file(conn, database, table, filename, TRUE, TRUE, FALSE,NULL,NULL);
      }
    }
  }

  g_strfreev(split_table);
  g_strfreev(split_file);
  g_dir_close(dir);
}

void restore_schema_post(MYSQL *conn) {
  GError *error = NULL;
  GDir *dir = g_dir_open(directory, 0, &error);
  gchar **split_file = NULL;
  gchar *database = NULL;
  // gchar* table=NULL;

  if (error) {
    g_critical("cannot open directory %s, %s\n", directory, error->message);
    errors++;
    return;
  }

  const gchar *filename = NULL;

  while ((filename = g_dir_read_name(dir))) {
    if (!source_db ||
        g_str_has_prefix(filename, g_strdup_printf("%s-schema-post", source_db))) {
      if (g_strrstr(filename, "-schema-post.sql")) {
        split_file = g_strsplit(filename, "-schema-post.sql", 0);
        database = split_file[0];
        // table= split_file[0]; //NULL
        g_message("Restoring routines and events for `%s`", db ? db : database);
        restore_data_from_file(conn, database, NULL, filename, TRUE, TRUE, FALSE,NULL,NULL);
      }
    }
  }

  g_strfreev(split_file);
  g_dir_close(dir);
}

void create_database(MYSQL *conn, gchar *database) {

  gchar *query = NULL;

  if ((db == NULL && source_db == NULL) ||
      (db != NULL && source_db != NULL && !g_ascii_strcasecmp(db, source_db))) {
    const gchar *filename =
        g_strdup_printf("%s-schema-create.sql", db ? db : database);
    const gchar *filenamegz =
        g_strdup_printf("%s-schema-create.sql.gz", db ? db : database);
    const gchar *filepath = g_strdup_printf("%s/%s-schema-create.sql",
                                            directory, db ? db : database);
    const gchar *filepathgz = g_strdup_printf("%s/%s-schema-create.sql.gz",
                                              directory, db ? db : database);

    if (g_file_test(filepath, G_FILE_TEST_EXISTS)) {
      restore_data_from_file(conn, database, NULL, filename, TRUE, FALSE, FALSE,NULL,NULL);
    } else if (g_file_test(filepathgz, G_FILE_TEST_EXISTS)) {
      restore_data_from_file(conn, database, NULL, filenamegz, TRUE, FALSE, FALSE,NULL,NULL);
    } else {
      query = g_strdup_printf("CREATE DATABASE `%s`", db ? db : database);
      mysql_query(conn, query);
    }
  } else {
    query = g_strdup_printf("CREATE DATABASE `%s`", db ? db : database);
    mysql_query(conn, query);
  }

  g_free(query);
  return;
}

void add_schema(const gchar *filename, GAsyncQueue *fast_index_creation_queue, GAsyncQueue *constraints_queue, MYSQL *conn) {
  // 0 is database, 1 is table with -schema on the end
  gchar **split_file = g_strsplit(filename, ".", 0);
  gchar *database = split_file[0];
  // Remove the -schema from the table name
  gchar **split_table = g_strsplit(split_file[1], "-schema", 0);
  gchar *table = split_table[0];

  gchar *query =
      g_strdup_printf("SHOW CREATE DATABASE `%s`", db ? db : database);
  if (mysql_query(conn, query)) {
    g_message("Creating database `%s`", db ? db : database);
    create_database(conn, database);
  } else {
    MYSQL_RES *result = mysql_store_result(conn);
    // In drizzle the query succeeds with no rows
    my_ulonglong row_count = mysql_num_rows(result);
    mysql_free_result(result);
    if (row_count == 0) {
      create_database(conn, database);
    }
  }
  int truncate_or_delete_failed=0;
  if (overwrite_tables) {
    if (purge_mode == DROP) {	  
      g_message("Dropping table or view (if exists) `%s`.`%s`",
                db ? db : database, table);
      query = g_strdup_printf("DROP TABLE IF EXISTS `%s`.`%s`",
                              db ? db : database, table);
      mysql_query(conn, query);
      query = g_strdup_printf("DROP VIEW IF EXISTS `%s`.`%s`", db ? db : database,
                              table);
      mysql_query(conn, query);
    } else if (purge_mode == TRUNCATE) {
      g_message("Truncating table `%s`.`%s`", db ? db : database, table);
      query= g_strdup_printf("TRUNCATE TABLE `%s`.`%s`", db ? db : database, table);
      truncate_or_delete_failed= mysql_query(conn, query);
      if (truncate_or_delete_failed)
        g_warning("Truncate failed, we are going to try to create table or view");
    } else if (purge_mode == DELETE) {
      g_message("Deleting content of table `%s`.`%s`", db ? db : database, table);
      query= g_strdup_printf("DELETE FROM `%s`.`%s`", db ? db : database, table);
      truncate_or_delete_failed= mysql_query(conn, query);
      if (truncate_or_delete_failed)
        g_warning("Delete failed, we are going to try to create table or view"); 
    }
  }

  g_free(query);

  if ((purge_mode == TRUNCATE || purge_mode == DELETE) && !truncate_or_delete_failed){
      g_message("Skipping table creation `%s`.`%s`", db ? db : database, table);
  }else{
    g_message("Creating table `%s`.`%s`", db ? db : database, table);
    restore_data_from_file(conn, database, table, filename, TRUE, TRUE, TRUE, fast_index_creation_queue, constraints_queue);
  }
  g_strfreev(split_table);
  g_strfreev(split_file);
  return;
}

gint compare_restore_job(gconstpointer a, gconstpointer b){
  gchar *a_key=g_strdup_printf("%s_%s",((struct restore_job *)a)->database,((struct restore_job *)a)->table);
  gchar *b_key=g_strdup_printf("%s_%s",((struct restore_job *)b)->database,((struct restore_job *)b)->table);
  struct db_table * a_val=g_hash_table_lookup(tables,a_key);
  struct db_table * b_val=g_hash_table_lookup(tables,b_key);
  g_free(a_key);
  g_free(b_key);
  return a_val->rows < b_val->rows;
}

void *process_queue(struct thread_data *td) {
  struct configuration *conf = td->conf;
  g_mutex_lock(init_mutex);
  MYSQL *thrconn = mysql_init(NULL);
  g_mutex_unlock(init_mutex);

  configure_connection(thrconn, "myloader");

  if (!mysql_real_connect(thrconn, hostname, username, password, NULL, port,
                          socket_path, 0)) {
    g_critical("Failed to connect to MySQL server: %s", mysql_error(thrconn));
    exit(EXIT_FAILURE);
  }

  if (mysql_query(thrconn, "SET SESSION wait_timeout = 2147483")) {
    g_warning("Failed to increase wait_timeout: %s", mysql_error(thrconn));
  }

  if (!enable_binlog)
    mysql_query(thrconn, "SET SQL_LOG_BIN=0");

  mysql_query(thrconn, set_names_str);
  mysql_query(thrconn, "/*!40101 SET SQL_MODE='NO_AUTO_VALUE_ON_ZERO' */");
  mysql_query(thrconn, "/*!40014 SET UNIQUE_CHECKS=0 */");
  mysql_query(thrconn, "/*!40014 SET FOREIGN_KEY_CHECKS=0*/");
  if (commit_count > 1)
    mysql_query(thrconn, "SET autocommit=0");

  g_async_queue_push(conf->ready, GINT_TO_POINTER(1));

  struct job *job = NULL;
  struct restore_job *rj = NULL;
  for (;;) {
    job = (struct job *)g_async_queue_pop(conf->queue);

    switch (job->type) {
    case JOB_RESTORE_STRING:
      rj = job->job_data;
      g_message("Thread %d restoring indexes or contraints `%s`.`%s`", td->thread_id,
                rj->database, rj->table);
      guint query_counter=0;
      //restore_data_in_gstring(thrconn, rj->database, rj->table, rj->prestatement, NULL, FALSE, &query_counter);
      restore_data_in_gstring(thrconn, rj->database, rj->table, rj->statement, NULL, FALSE, &query_counter);
      if (rj->database)
        g_free(rj->database);
      if (rj->table)
        g_free(rj->table);
      if (rj->statement)
        g_string_free(rj->statement,TRUE);
      g_free(rj);
      g_free(job);
      break;
    case JOB_RESTORE_FILENAME:
      rj = job->job_data;
      g_mutex_lock(progress_mutex);
      progress++;
      g_message("Thread %d restoring `%s`.`%s` part %d. Progress %llu of %llu .", td->thread_id,
                rj->database, rj->table, rj->part, progress,total_data_sql_files);
      g_mutex_unlock(progress_mutex);
      restore_data_from_file(thrconn, rj->database, rj->table, rj->filename, FALSE, TRUE, FALSE,NULL, NULL);
      if (rj->database)
        g_free(rj->database);
      if (rj->table)
        g_free(rj->table);
      if (rj->filename)
        g_free(rj->filename);
      g_free(rj);
      g_free(job);
      break;
    case JOB_WAIT:
      g_async_queue_push(conf->ready, GINT_TO_POINTER(1));
      GAsyncQueue *queue=job->job_data;
      g_async_queue_pop(queue);
      break;
    case JOB_SHUTDOWN:
      g_message("Thread %d shutting down", td->thread_id);
      if (thrconn)
        mysql_close(thrconn);
      g_free(job);
      mysql_thread_end();
      return NULL;
      break;
    default:
      g_critical("Something very bad happened!");
      exit(EXIT_FAILURE);
    }
  }
  if (thrconn)
    mysql_close(thrconn);
  mysql_thread_end();
  return NULL;
}


void restore_data_in_gstring_from_file(MYSQL *conn, char *database, char *table, GString *data, const char *filename, gboolean is_schema, guint *query_counter)
{
                if (mysql_real_query(conn, data->str, data->len)) {

                        g_critical("Error restoring %s.%s from file %s: %s \n%s", db ? db : database, table, filename, mysql_error(conn),data->str);
                        errors++;
                        return;
                }
                *query_counter=*query_counter+1;
                if (!is_schema && (commit_count > 1) &&(*query_counter == commit_count)) {
                        *query_counter= 0;
                        if (mysql_query(conn, "COMMIT")) {
                                g_critical("Error committing data for %s.%s: %s", db ? db : database, table, mysql_error(conn));
                                errors++;
                                return;
                        }
                        mysql_query(conn, "START TRANSACTION");
                }
                g_string_set_size(data, 0);
}

void restore_data_in_gstring(MYSQL *conn, char *database, char *table, GString *data, const char *filename, gboolean is_schema, guint *query_counter)
{
  gchar** line=g_strsplit(data->str, ";\n", -1);
  int i=0;	
  for (i=0; i < (int)g_strv_length(line);i++){
     if (strlen(line[i])>2){
       GString *str=g_string_new(line[i]);
       g_string_append_c(str,';');
       restore_data_in_gstring_from_file(conn, database, table, str, filename, is_schema, query_counter);
     }
  }
}


void append_alter_table(GString * alter_table_statement, char *database, char *table){
  g_string_append(alter_table_statement,"ALTER TABLE `");
  g_string_append(alter_table_statement,db ? db : database);
  g_string_append(alter_table_statement,"`.`");
  g_string_append(alter_table_statement,table);
  g_string_append(alter_table_statement,"` ");
}

void finish_alter_table(GString * alter_table_statement){
  gchar * str=g_strrstr_len(alter_table_statement->str,alter_table_statement->len,",");
  if ((str - alter_table_statement->str) > (long int)(alter_table_statement->len - 5)){
    *str=';';
    g_string_append_c(alter_table_statement,'\n');
  }else
    g_string_append(alter_table_statement,";\n");
}


void restore_data_from_file(MYSQL *conn, char *database, char *table,
                  const char *filename, gboolean is_schema, gboolean need_use, 
		  gboolean is_create_table, GAsyncQueue *fast_index_creation_queue, GAsyncQueue *constraints_queue) {
  void *infile;
  gboolean is_compressed = FALSE;
  gboolean eof = FALSE;
  guint query_counter = 0;
  GString *data = g_string_sized_new(512);

  gchar *path = g_build_filename(directory, filename, NULL);

  if (!g_str_has_suffix(path, ".gz")) {
    infile = g_fopen(path, "r");
    is_compressed = FALSE;
  } else {
    infile = (void *)gzopen(path, "r");
    is_compressed = TRUE;
  }

  if (!infile) {
    g_critical("cannot open file %s (%d)", filename, errno);
    errors++;
    return;
  }

  if (need_use) {
    gchar *query = g_strdup_printf("USE `%s`", db ? db : database);

    if (mysql_query(conn, query)) {
      g_critical("Error switching to database %s whilst restoring table %s from file %s",
                 db ? db : database, table, filename);
      g_free(query);
      errors++;
      return;
    }

    g_free(query);
  }

  if (!is_schema && (commit_count > 1) )
    mysql_query(conn, "START TRANSACTION");
  GString *alter_table_statement=g_string_sized_new(512);
  GString *alter_table_constraint_statement=g_string_sized_new(512);
  while (eof == FALSE) {
    if (read_data(infile, is_compressed, data, &eof)) {
      if (g_strrstr(&data->str[data->len >= 5 ? data->len - 5 : 0], ";\n")) {
        if (is_create_table){
          if (innodb_optimize_keys){

            // Check if it is a /*!40  SET 
	    if (g_strrstr(data->str,"/*!40")){
          g_string_append(alter_table_statement,data->str);
	      restore_data_in_gstring_from_file(conn, database, table, data, filename, is_schema, &query_counter);
	    }else{
              // Processing CREATE TABLE statement
              gboolean is_innodb_table=FALSE;
              gchar** split_file= g_strsplit(data->str, "\n", -1);
              gchar *autoinc_column=NULL;
              GString *table_without_indexes=g_string_sized_new(512);
              append_alter_table(alter_table_statement,db ? db : database,table);
              append_alter_table(alter_table_constraint_statement,db ? db : database,table);
              int fulltext_counter=0;
              int i=0;
              gboolean include_constraint=FALSE;
              for (i=0; i < (int)g_strv_length(split_file);i++){
                if ( g_strstr_len(split_file[i],5,"  KEY")
                  || g_strstr_len(split_file[i],8,"  UNIQUE")
                  || g_strstr_len(split_file[i],9,"  SPATIAL")
                  || g_strstr_len(split_file[i],10,"  FULLTEXT")
                  || g_strstr_len(split_file[i],7,"  INDEX")
                 ){
                // Ignore if the first column of the index is the AUTO_INCREMENT column
                  if ((autoinc_column != NULL) && (g_strrstr(split_file[i],autoinc_column))){
                    g_string_append(table_without_indexes, split_file[i]);
                    g_string_append_c(table_without_indexes,'\n');
                  }else{
                    if (g_strrstr(split_file[i],"  FULLTEXT")){
                      fulltext_counter++;
                    }
                    if (fulltext_counter>1){
                      fulltext_counter=1;
                      finish_alter_table(alter_table_statement);
                      append_alter_table(alter_table_statement,db ? db : database,table);
                    }
                    g_string_append(alter_table_statement,"\n ADD");
                    g_string_append(alter_table_statement, split_file[i]);
                  }
                }else{
                  if (g_strstr_len(split_file[i],12,"  CONSTRAINT")){
                    include_constraint=TRUE;
                    g_string_append(alter_table_constraint_statement,"\n ADD");
                    g_string_append(alter_table_constraint_statement, split_file[i]);
                  }else{
                    if (g_strrstr(split_file[i],"AUTO_INCREMENT")){
                      gchar** autoinc_split=g_strsplit(split_file[i],"`",3);
                      autoinc_column=g_strdup_printf("(`%s`", autoinc_split[1]);
                    }
                    g_string_append(table_without_indexes, split_file[i]);
                    g_string_append_c(table_without_indexes,'\n');
                  }
                }
                if (g_strrstr(split_file[i],"ENGINE=InnoDB")){
                  is_innodb_table=TRUE;
	        }
              }
              finish_alter_table(alter_table_statement);
              if (is_innodb_table){
                g_message("Fast index creation will be use for table: %s.%s",db ? db : database,table);
                restore_data_in_gstring_from_file(conn, database, table, g_string_new(g_strjoinv("\n)",g_strsplit(table_without_indexes->str,",\n)",-1))) , filename, is_schema, &query_counter);
                struct restore_job *rj = new_restore_job(g_strdup(filename), g_strdup(database), g_strdup(table), alter_table_statement, 0);
                g_async_queue_push(fast_index_creation_queue, new_job(JOB_RESTORE_STRING,rj));
                if (include_constraint){
                  rj = new_restore_job(g_strdup(filename), g_strdup(database), g_strdup(table), alter_table_constraint_statement, 0);
                  g_async_queue_push(constraints_queue, new_job(JOB_RESTORE_STRING,rj));
                }
                g_string_set_size(data, 0);
              }else{
                restore_data_in_gstring_from_file(conn, database, table, data, filename, is_schema, &query_counter);
              }
            }
	  }else{
            restore_data_in_gstring_from_file(conn, database, table, data, filename, is_schema, &query_counter);
	  }
        }else{
          if (rows > 0 && g_strrstr_len(data->str,6,"INSERT"))
              split_and_restore_data_in_gstring_from_file(conn, database, table,
                  filename, data, is_schema, &query_counter);
          else 
            restore_data_in_gstring_from_file(conn, database, table, data, filename, is_schema, &query_counter);
        }
      }
    } else {
      g_critical("error reading file %s (%d)", filename, errno);
      errors++;
      return;
    }
  }
  if (!is_schema && (commit_count > 1) && mysql_query(conn, "COMMIT")) {
    g_critical("Error committing data for %s.%s from file %s: %s",
               db ? db : database, table, filename, mysql_error(conn));
    errors++;
  }
  g_string_free(data, TRUE);
  g_free(path);
  if (!is_compressed) {
    fclose(infile);
  } else {
    gzclose((gzFile)infile);
  }
  return;
}

gboolean read_data(FILE *file, gboolean is_compressed, GString *data,
                   gboolean *eof) {
  char buffer[256];

  do {
    if (!is_compressed) {
      if (fgets(buffer, 256, file) == NULL) {
        if (feof(file)) {
          *eof = TRUE;
          buffer[0] = '\0';
        } else {
          return FALSE;
        }
      }
    } else {
      if (!gzgets((gzFile)file, buffer, 256)) {
        if (gzeof((gzFile)file)) {
          *eof = TRUE;
          buffer[0] = '\0';
        } else {
          return FALSE;
        }
      }
    }
    g_string_append(data, buffer);
  } while ((buffer[strlen(buffer)] != '\0') && *eof == FALSE);

  return TRUE;
}
