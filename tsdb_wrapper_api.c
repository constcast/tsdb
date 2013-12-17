/*
 * tsdb_wrapper_api.c
 *
 *  Created on: Dec 5, 2013
 *      Author: Oleg Klyudt
 */

#include "tsdb_wrapper_api.h"

#define lambda(l_ret_type, l_arguments, l_body)         \
        ({                                                    \
         l_ret_type l_anonymous_functions_name l_arguments   \
         l_body                                            \
         &l_anonymous_functions_name;                        \
         })

static int _reportNewMetricCB(void *int_data, void *ext_data) {

  /* typeof int_data == char* */
  /* typeof ext_data == pointers_collection_t* */

  if (int_data == NULL || ext_data == NULL) {
      return -1;
  }
  /* Make a deep copy of the key to make it persistent*/
  char *key = malloc(strlen((char *)int_data) + 1); // +1 to incorporate /0 character
  if (key == NULL) return -1;
  memcpy(key, (char *)int_data, strlen((char *)int_data) + 1);

  /* Add the key to the list of metrics with reallocation of the latter */
  char **intermidiate_array;
  pointers_collection_t *cb_pointers = (pointers_collection_t*) ext_data;
  u_int32_t i;
  size_t numElems = cb_pointers->rows[i]->new_metrics.num_of_entries;
  for (i = 0; i < cb_pointers->num_of_rows; ++i) {
      /* Add a new key (metric) to every row */
      intermidiate_array = (char**) realloc(cb_pointers->rows[i]->new_metrics.list,
          (numElems + 1) * sizeof(char*) );
      if (intermidiate_array == NULL) return -1;
      intermidiate_array[numElems] = key;
      cb_pointers->rows[i]->new_metrics.list = intermidiate_array;
      cb_pointers->rows[i]->new_metrics.num_of_entries++;
      intermidiate_array = NULL;
  }

  return 0;
}

static int consolidate_incrementally(tsdb_value *new_data, tsdb_row_t *row) {
  /* The algorithm currently does not support values,
   * which span several contiguous tsdb_values elements.
   * Hence it works correctly only values_per_entry = 1
   * foe TSDB DB. It is possible to implement the support
   * for larger values, however one would need to introduce
   * arithmetic for large integers not covered by any type. */

  /* MUST BE: lenof(new_data) == lenof(row->data) == row->size */
  /* Thus function implements incremental average algorithm.
   * Let S_n = (a_1 + a_2 + ... + a_n) / n be a partial sum for
   * a sequence a_1, a_2, a_3, ..., a_n, ... The sum is an average
   * in fact. Then S_(n+1) = S_n * n / (n+1) + a_(n+1)/(n+1)
   * represents an average as well. Proof is evident. */
  /* Here S_n is every element of row, whereas a_(n+1) is an element
   * of new_data array. */
  size_t i;
  u_int32_t n = row->cr_elapsed;

  for (i=0; i < row->size; ++i) {
      row->data[i] = (tsdb_value)((long double)((int64_t) row->data[i]) * (long double)n / (long double)(n + 1)
                     + (long double)((int64_t)new_data[i]) / (long double)(n+1));
  }

  row->cr_elapsed ++;
  return 0;
}

static int _reportChunkDataCB(void *int_data, void *ext_data) {
  /* Data in chunk and data in accumulation buffers get aligned
   * and a consolidation function is invoked upon them */

  /* typeof int_data == tsdb_handler* */
  /* typeof ext_data == pointers_collection_t* */

  if (int_data == NULL || ext_data == NULL) {
      return -1;
  }

  /* Definitions and type conversions */

  u_int8_t i;

  pointers_collection_t *rows_bundle = (pointers_collection_t*) ext_data;

  tsdb_value *r_data = ((tsdb_handler *) int_data)->chunk.data; // reported data array
  tsdb_value *r_data_prepared = NULL;
  size_t tsdb_val_len = ((tsdb_handler *) int_data)->values_len; //size in bytes (i.e. chars)
  size_t r_data_size = ((tsdb_handler *) int_data)->chunk.data_len / tsdb_val_len;
  size_t unified_size = r_data_size;

  /* Find out max size across all rows and the chunk data to add */
  for (i = 0; i < rows_bundle->num_of_rows; ++i ) {
      if (unified_size < rows_bundle->rows[i]->size) unified_size = rows_bundle->rows[i]->size;
  }

  /* If size of initially provided data in chunk is smaller
   * than unified size, the data has to be reallocated to
   * be aligned in size */
  if (r_data_size != unified_size) {
      /* Align the data chunk to the new size*/
      r_data_prepared = malloc(unified_size * tsdb_val_len);
      if (r_data_prepared == NULL) return -1;
      memset(r_data_prepared, ((tsdb_handler *) int_data)->unknown_value, unified_size * tsdb_val_len);

      /* fill it with the data passed to the callBack as r_data*/
      memcpy(r_data_prepared, r_data, r_data_size * tsdb_val_len);
  }

  for (i = 0; i < rows_bundle->num_of_rows; ++i ) {
      if (rows_bundle->rows[i]->size != unified_size) { // then rows_bundle->rows[i].size < unified_size
          /* Reallocate the row */
          tsdb_value *row_data_prepared = (tsdb_value *) realloc(rows_bundle->rows[i]->data, unified_size * tsdb_val_len);
          if (row_data_prepared == NULL) {
              free(r_data_prepared);
              return -1;
          }

          /* Fill the grown undefined portion of data with default value */
          memset(&row_data_prepared[rows_bundle->rows[i]->size],
                 ((tsdb_handler *) int_data)->unknown_value,
                 (unified_size - rows_bundle->rows[i]->size) * tsdb_val_len);
          rows_bundle->rows[i]->data = row_data_prepared;
          rows_bundle->rows[i]->size = unified_size;
      }

      /* Now data in chunk and data in accumulation arrays are prepared
       * and all the arrays are aligned in size. Now one can safely
       * perform consolidation */
      if (r_data_prepared == NULL) { //reported data chunk was not reallocated, as it is the largest one
          if (consolidate_incrementally(r_data, rows_bundle->rows[i])) return -1;
      } else {
          if (consolidate_incrementally(r_data_prepared, rows_bundle->rows[i])) {
              free(r_data_prepared);
              return -1;
          }
      }
      free(r_data_prepared);
      r_data_prepared = NULL;
  }

  *(rows_bundle->last_accum_update) = time(NULL);

  return 0;
}



static int check_args_init(tsdbw_handle *handle, u_int16_t *finest_timestep,
    const char **db_files,
    char io_flag) {

  int idx;
  size_t s;

  if (db_files == NULL || handle == NULL) {
      trace_error("NULL ptr detected. Is array of DB files paths empty? DBs handle?");
      return -1;
  }

  for(idx=0; idx < TSDBW_DB_NUM; ++idx) {
      s = strlen(db_files[idx]);
      if (s == 0 || s > MAX_PATH_STRING_LEN ) {
          trace_error("Zero/too long string of a DB file path");
          return -1;
      }
  }

  switch (io_flag) {
  case 'r':
    handle->mode = TSDBW_MODE_READ;
    break;
  case 'a':
    handle->mode = TSDBW_MODE_APPEND;
    break;
  case 'w':
    handle->mode = TSDBW_MODE_WRITE;
    break;
  default:
    trace_error("Unknown mode flag");
    return -1;
  }

  return 0;
}


static void free_dbhs (tsdb_handler **h_dbs) {
  int i = 0;
  for (; i < TSDBW_DB_NUM; ++i) {
      free(h_dbs[i]);
  }
  free(h_dbs);
}

static int open_DBs(tsdbw_handle *handle, u_int16_t *finest_timestep,
    const char **db_files,
    char io_flag) {

  int i, j;
  u_int16_t values_per_entry = 1;
  u_int16_t timesteps[] = {*finest_timestep,
                           *finest_timestep * TSDBW_MM,
                           *finest_timestep * TSDBW_CM};

  /* Allocate memory for DBs handles */
  tsdb_handler **h_dbs = (tsdb_handler **) calloc(TSDBW_DB_NUM, sizeof(tsdb_handler *));
  if (h_dbs == NULL) {
      trace_error("Failed to allocate memory for DB handles");
      return -1;
  }

  for (i=0; i < TSDBW_DB_NUM; ++i ) {
      h_dbs[i] = (tsdb_handler *) calloc(1, sizeof(tsdb_handler));
      if (h_dbs[i] == NULL) {
          trace_error("Failed to allocate memory for DB handles");
          free(h_dbs);
          return -1;
      }
  }

  /* Delete old DB files if WRITE mode was set */
  if (handle->mode == TSDBW_MODE_WRITE) {
      for (i=0; i < TSDBW_DB_NUM; ++i ) {
          if (fremove(db_files[i]) != 0) {
              trace_error("Could not remove old DB files. Mode - writing.");
              return -1;
          }
      }
      trace_info("Mode = writing. All old DBs were deleted.");
  }

  /* If Berkeley DBs are to be opened in environment (to enable locks)
   * then the following snippet might be of use
   *
   *
   *    typedef struct {
        ...
        DB *db;
        DB_ENV *db_env;
        } tsdb_handler;
   *
   * if ((ret = db_env_create(&handler->db_env, 0)) != 0) {
        trace_error("Error while creating DB_ENV handler [%s]", db_strerror(ret));
        return -1;
    }

    if ((ret = handler->db_env->set_shm_key(handler->db_env , 150)) != 0) {
      trace_error("Error while setting DB_ENV handler's shared memory key [%s]", db_strerror(ret));
      return -1;
    }

    if ((ret = handler->db_env->open(handler->db_env,
                          "/home/admin/Documents/tsdb-src-refactor",
                          DB_INIT_LOCK | DB_INIT_MPOOL | DB_SYSTEM_MEM | (read_only ? 0 : DB_CREATE),
                          mode)) != 0) {
        trace_error("Error while opening DB_ENV handler [%s]", db_strerror(ret));
        return -1;
    }

    if ((ret = db_create(&handler->db, handler->db_env, 0)) != 0) {
        trace_error("Error while creating DB handler [%s]", db_strerror(ret));
        return -1;
    }
   *
   * To close environment:
   *     handler->db_env->close(handler->db_env, 0);
   * */

  /* Open TSDBs */
  for (i=0; i < TSDBW_DB_NUM; ++i ) {
      if (tsdb_open(db_files[i],
                    h_dbs[i],
                    &values_per_entry,
                    timesteps[i],
                    (handle->mode == TSDBW_MODE_READ))) {

          //close already open DBs and remove files they were assigned to
          for (j = 0; j < i; ++j){
              tsdb_close(h_dbs[j]);
              fremove(db_files[j]);
          }
          //free allocated memory
          free_dbhs(h_dbs);
          return -1;
      } else {
          trace_info("DB %s opened.",db_files[i]);
      }
  }

  /* Make open TSDBs available through tsdbw handle */
  handle->db_hs = h_dbs;
  return 0;
}

static int init_structures_and_callbacks(tsdbw_handle *h) {

  int i;

  /* Assigning initial values */
  for (i = 0; i < TSDBW_DB_NUM; ++i){
      h->db_hs[i]->unknown_value = TSDBW_UNKNOWN_VALUE;
  }

  h->mod_accum.data = NULL;
  h->mod_accum.size = 0;
  h->mod_accum.cr_elapsed = 0;
  h->mod_accum.new_metrics.list = NULL;
  h->mod_accum.new_metrics.num_of_entries = 0;
  h->mod_accum.last_flush_time = 0;

  h->coarse_accum.data = NULL;
  h->coarse_accum.size = 0;
  h->coarse_accum.cr_elapsed = 0;
  h->coarse_accum.new_metrics.list = NULL;
  h->coarse_accum.new_metrics.num_of_entries = 0;
  h->coarse_accum.last_flush_time = 0;

  h->last_accum_update = 0;

  h->cb_communication.last_accum_update = &h->last_accum_update;
  h->cb_communication.num_of_rows = TSDBW_DB_NUM - 1; // assuming every but fine DB has its own accumulation buffer for incremental consolidation
  h->cb_communication.rows = (tsdb_row_t**) malloc(h->cb_communication.num_of_rows * sizeof(tsdb_row_t*));
                        if (h->cb_communication.rows == NULL) return -1;
  h->cb_communication.rows[0] = &h->mod_accum;
  h->cb_communication.rows[1] = &h->coarse_accum;

  /* Defining callbacks for the finest TSDB.
   * For other TSDBs these have NULL values
   * and will be ignored within the original TSDB API */
  h->db_hs[0]->reportNewMetricCB.external_data = & h->cb_communication;
  h->db_hs[0]->reportChunkDataCB.external_data = & h->cb_communication;
  h->db_hs[0]->reportNewMetricCB.cb = _reportNewMetricCB;
  h->db_hs[0]->reportChunkDataCB.cb = _reportChunkDataCB;

  return 0;
}

static int consolidation_start(tsdbw_handle *h) {
  /* Here consolidation routine should either
   * be forked or started in a separate thread.
   * This however requires IPC/mutex implementation
   * and opening DBs with THREAD flag to make
   * handles thread-free, i.e., eligible to use
   * across threads  */

  /* Currently consolidation is being done during
   * DBs writing process*/
  return 0;
}

int tsdbw_init(tsdbw_handle *h, u_int16_t *finest_timestep,
               const char **db_files,
               char io_flag) {

  /* Cautious memory cleaning */
  memset(h, 0, sizeof(tsdbw_handle));

  /* Sanity checks and mode setting*/
  // h->mode is set by check_args_init
  if (check_args_init(h, finest_timestep, db_files, io_flag) != 0) return -1;

  /* Open the given TSDBs*/
  //h->db_hs is set by open_DBs()
  if (open_DBs(h, finest_timestep, db_files, io_flag) != 0) return -1;

  /* Assigning initial values */
  if (init_structures_and_callbacks(h)) return -1;

    /* Start consolidation daemon */
  if (consolidation_start(h) != 0) return -1;

  return 0;
}

void tsdbw_close(tsdbw_handle *handle) {

  int i;

  /* Close DBs */
  for (i = 0; i < TSDBW_DB_NUM; ++i ) {
      tsdb_close(handle->db_hs[i]);
  }

  /* Release memory allocated for those DBs*/
  free_dbhs(handle->db_hs);

  /* Release memory allocated for accums */
  if (handle->mod_accum.data != NULL) {
      free(handle->mod_accum.data);
      handle->mod_accum.size = 0;
      handle->mod_accum.data = NULL;
  }
  if (handle->coarse_accum.data != NULL) {
      free(handle->coarse_accum.data);
      handle->coarse_accum.size = 0;
      handle->coarse_accum.data = NULL;
  }
  if (handle->mod_accum.new_metrics.list != NULL) {
      for (i = 0; i < handle->mod_accum.new_metrics.num_of_entries; ++i) {
          free(handle->mod_accum.new_metrics.list[i]);
      }
      free(handle->mod_accum.new_metrics.list);
      handle->mod_accum.new_metrics.num_of_entries = 0;
      handle->mod_accum.new_metrics.list = NULL;
  }
  if (handle->coarse_accum.new_metrics.list != NULL) {
      for (i = 0; i < handle->coarse_accum.new_metrics.num_of_entries; ++i) {
          free(handle->coarse_accum.new_metrics.list[i]);
      }
      free(handle->coarse_accum.new_metrics.list);
      handle->coarse_accum.new_metrics.num_of_entries = 0;
      handle->coarse_accum.new_metrics.list = NULL;
  }
  if (handle->cb_communication.rows != NULL) {
      free(handle->cb_communication.rows);
      handle->cb_communication.num_of_rows = 0;
      handle->cb_communication.rows = NULL;
  }

}

static int check_args_write(tsdbw_handle *db_set_h, const char **metrics, const int64_t *values, u_int32_t num_elem) {

  int i;

  if (db_set_h == NULL) {
      trace_error("DBs handle not allocated");
      return -1;
  }

  for (i = 0; i < TSDBW_DB_NUM; ++i) {
      if (db_set_h->db_hs[i] == NULL) {
          trace_error("DBs handle not allocated");
          return -1;
      }
      if (! db_set_h->db_hs[i]->alive) {
          trace_error("DB is not alive (closed?)");
          return -1;
      }
  }

  if (metrics == NULL) {
        trace_error("Array of metric names is empty");
        return -1;
    }

  if (values == NULL) {
        trace_error("Array of values for writing is empty");
        return -1;
    }

  for (i = 0; i < num_elem; ++i) {

      if (metrics[i] == NULL) {
          trace_error("Trying to address NULL pointer");
          return -1;
      }

      if (strlen(metrics[i]) > MAX_METRIC_STRING_LEN ) {
          trace_error("Maximum allowed string length for a matric name is exceeded");
          return -1;
      }

  }

  return 0;
}

static int fine_tsdb_update(tsdbw_handle *db_set_h,
    /* This function does not support currently values_per_entry > 1*/
    const char **metrics,
    const int64_t *values,
    u_int32_t num_elem) {

  int rv;
  tsdb_value *buf = (tsdb_value *) calloc(num_elem, db_set_h->db_hs[0]->values_len);
  if (buf == NULL) {
      trace_error("Failed to allocate memory");
      return -1;
  }

#if defined __GNUC__

  /* This hack works only with GCC. The function is unpacked for other compilers. */
  rv = lambda(int,
          (tsdb_value *buf,
          tsdbw_handle *db_set_h,
          const char **metrics,
          const int64_t *values,
          u_int32_t num_elem),
          {
              int i;
              int fail_if_missing = 0;
              int is_growable = 1;
              u_int32_t cur_time = (u_int32_t) time(NULL);

              /* Converting values into the proper type for TSDB */
              for (i = 0; i < num_elem; ++i) {
                  buf[i] = (tsdb_value) values[i];
              }

              for (i = 0; i < num_elem; ++i) {

                  if (strlen(metrics[i]) == 0) continue; //skip empty metric

                  if (tsdb_goto_epoch(db_set_h->db_hs[0], cur_time, fail_if_missing, is_growable)) {
                      trace_error("Failed to advance to a new epoch");
                      return -1;
                  }
                  if (tsdb_set(db_set_h->db_hs[0], metrics[i], &buf[i])) {
                      trace_warning("Failed to set value in a TSDB. ");
                      /* An entry in TSDB with an unset value will preserve its initially
                       * set one by default (which can be adjusted on per TSDB basis)  */
                  }
              }
              return 0;
          })(buf, db_set_h, metrics, values, num_elem );
#else
  int i;
  int fail_if_missing = 0;
  int is_growable = 1;
  u_int32_t cur_time = (u_int32_t) time(NULL);

  /* Converting values into the proper type for TSDB */
  for (i = 0; i < num_elem; ++i) {
      buf[i] = (tsdb_value) values[i];
  }

  for (i = 0; i < num_elem; ++i) {

      if (strlen(metrics[i]) == 0) continue; //skip empty metric

      if (tsdb_goto_epoch(db_set_h->db_hs[0], cur_time, fail_if_missing, is_growable)) {
          trace_error("Failed to advance to a new epoch");
          free(buf);
          return -1;
      }
      if (tsdb_set(db_set_h->db_hs[0], metrics[i], &buf[i])) {
          trace_warning("Failed to set value in a TSDB. ");
          /* An entry in TSDB with an unset value will preserve its initially
           * set one by default (which can be adjusted on per TSDB basis)  */
      }
  }
  rv = 0;
#endif

  free(buf);
  return rv;
}

static int tsdbw_consolidated_flush(tsdb_handler *tsdb_h, tsdb_row_t *accum_buf, time_t last_update_time ) {
  //TODO: add flag for strict writing error handling

  u_int32_t i, j, start_idx;
  u_int8_t nvpe = tsdb_h->values_per_entry; //number of values per entry
  u_int8_t err_flag = 0;
  u_int32_t epoch_to_write = last_update_time;
  normalize_epoch(tsdb_h, &epoch_to_write);

  u_int32_t epoch_current = (u_int32_t) time(NULL);
  normalize_epoch(tsdb_h, &epoch_current);

  if (epoch_to_write + tsdb_h->slot_duration < epoch_current) { //they are equal if no epochs were missed
      /* some consolidation epochs were missed (spent as outage),
       *  i.e., not written. We may want to do smth about it here */

      /* We have a choice - either fill the missed epochs
       * in consolidated DBs with default values
       * meaning absence of data, or we can just omit their writing at all.
       * As the TSDBs have a list of epochs internally, one can detect
       * missing epochs in DB based on this list and slot_duration time
       * and decide what to return for the values from these epochs.
       * We favor the latter option.*/
      char str_beg[20], str_end[20];
      time2str(&epoch_to_write, str_beg, 20);
      time2str(&epoch_current, str_end, 20);
      trace_warning("Missing epochs detected in a consolidated DB. Time step %u. Interval: %s -- %s", tsdb_h->slot_duration, str_beg, str_end);
  }

  if (tsdb_h->lowest_free_index !=  accum_buf->size - accum_buf->new_metrics.num_of_entries) {
      /* This IF checks for absence of gaps in metrics. We dont want to end up in
       * a situation where not all data columns have associated metrics (names). This
       * will render us being unable to query these columns */
      // tsdb_h->lowest_free_index is number of metrics in the TSDB at the current state (N.B. lowest_free_index counts since 0)
      // accum_buf->size is number of metrics in TSDB after updating it
      // accum_buf->new_metrics.num_of_entries is number of NEW metrics which are to be
      // added to the current TSDB
      trace_error("Not enough metric names for provided data to write in a consolidated DB. Nothing will be written.");
      return -1;
  }

  /* Firstly we write values for already existing metrics in the consolidated DB.
   * Hence we address metrics by index rather than name. It is possible only
   * due to monotonic allocation of column indices to new metrics, so that
   * new metrics are appended always at the very end one by one*/
  for (i = 0; i < tsdb_h->lowest_free_index; ++i) {
      if (tsdb_set_by_index(tsdb_h, &accum_buf->data[i * nvpe], &i)) {
          trace_error("Failed to write a value in consolidated TSDB. New metrics were not being added and the DB consistency is intact.");
          break;
      }
  }

  /* Now we write new metrics and respective values in the consolidated DB.
   * We use regular tsdb_set() to create the mappings metric-column index internally. */
  start_idx = accum_buf->size - accum_buf->new_metrics.num_of_entries; // == tsdb_h->lowest_free_index
  for (i = 0; i < accum_buf->new_metrics.num_of_entries; ++i) {
      if (tsdb_set(tsdb_h, accum_buf->new_metrics.list[i], &accum_buf->data[(start_idx + i) * nvpe])) {
          err_flag = 1;
          trace_error("Failed to write a value in consolidated TSDB. New metrics were being written, attempting to recover for the next flush.");
          /* Attempt of recovery: all values get nullified in the accum buffer,
           * its size is preserved, unwritten metrics are preserved. So that they can
           * be written upon next flushing*/
          for (j = 0; j < accum_buf->size * nvpe; ++j) {
              accum_buf->data[j] = 0; // we deliberately nullify it and not setting it to an undefined value, because arithmetic operations in the consolidation function are undefined in general for an undefined value
          }
              /* by setting "saccum_buf->cr_elapsed = 0;" at the end of the function
               * we effectively cancel the difference for _reportChunkDataCB
               * between unallocated accum_buf->data and
               * allocated and filled with zeros. Hence
               * the consolidated values (after consolidation function
               * passage over accum_buf->data) will not be biased */
          char **saved_metrics = malloc((accum_buf->new_metrics.num_of_entries - i) * sizeof(char*));
          for (j = 0; j < accum_buf->new_metrics.num_of_entries - i; ++j) {
              saved_metrics[j] = accum_buf->new_metrics.list[i + j]; //copying pointers to unwritten metrics
          }
          for (j = 0; j < i; ++j) {
              free(accum_buf->new_metrics.list[j]); //freeing successfully written metrics
          }
          free(accum_buf->new_metrics.list);
          accum_buf->new_metrics.list = saved_metrics;
          accum_buf->new_metrics.num_of_entries = accum_buf->new_metrics.num_of_entries - i;

          trace_info("Recovery of unwritten metrics succeeded");
          break;
      }
  }

  if (!err_flag) {
      free(accum_buf->data);
      accum_buf->data = NULL;
      accum_buf->size = 0;
      for (j = 0; j < accum_buf->new_metrics.num_of_entries; ++j) { //accum_buf->new_metrics.num_of_entries is intact only if no errors happened
          free(accum_buf->new_metrics.list[j]);
      }
      free(accum_buf->new_metrics.list);
      accum_buf->new_metrics.list = NULL;
      accum_buf->new_metrics.num_of_entries = 0;
  }

  accum_buf->cr_elapsed = 0;
  accum_buf->last_flush_time = (time_t) epoch_current;

  return 0;
}

int tsdbw_write(tsdbw_handle *db_set_h,
                const char **metrics,
                const int64_t *values,
                u_int32_t num_elem) {

  int i;

  if (db_set_h->mode == TSDBW_MODE_READ) return -1;

  /* Sanity checks */
  if (check_args_write(db_set_h, metrics, values, num_elem) != 0) return -1;

  /* Flushing arrays of consolidated data, if we step over an epoch */

  time_t cur_time = time(NULL);
  time_t time_diff, time_step;

  for (i = 1; i < TSDBW_DB_NUM; ++i) { // omitting the finest TSDB

      time_diff = cur_time - db_set_h->cb_communication.rows[i]->last_flush_time;
      time_step = (time_t)db_set_h->db_hs[i]->slot_duration;

      if (time_diff < 0) {

          trace_error("current time is less then time of last DB flush. It is either a logical mistake or type overflow.");
          tsdbw_close(db_set_h);
          return -1;

      } else if (time_diff >= time_step ) {

          if (tsdbw_consolidated_flush(db_set_h->db_hs[i], db_set_h->cb_communication.rows[i], db_set_h->last_accum_update  )) {
              return -1;
          }

      }

  }

  /* Updating the fine TSDB with values for metrics*/
  if (fine_tsdb_update(db_set_h, metrics, values, num_elem) != 0) return -1;


  return 0;
}

static int get_list_of_epochs(tsdb_handler *db_h, u_int32_t epoch_from, u_int32_t epoch_to,
                        u_int32_t **epochs_list, u_int32_t *epoch_num) {
  /* The function searches epochs in interval provided by arguments
   * in the given TSDB. A new sorted list of found and missing epochs gets
   * written into epochs_list and number of entries gets set in epoch_num.
   * Missing epochs are set to 0 in the list.  */
//TODO
  return 0;
}

static int check_args_query(tsdb_handler *tsdb_h, u_int32_t epoch_from,
      u_int32_t epoch_to,  const char **metrics, u_int32_t metrics_num ) {

  //TODO
  return 0;
}

int tsdbw_query(tsdbw_handle *db_set_h,
                time_t epoch_from,
                time_t epoch_to,
                const char **metrics,
                u_int32_t metrics_num,
                data_tuple_t **tuples,
                u_int32_t *epochs_num,
                char granularity_flag) {

  tsdb_handler *tsdb_h;

  switch(granularity_flag) {
  case TSDBW_FINE_TIMESTAMP:
    tsdb_h = db_set_h->db_hs[TSDBW_FINE_TIMESTAMP];
    break;
  case TSDBW_MODERATE_TIMESTAMP:
    tsdb_h = db_set_h->db_hs[TSDBW_MODERATE_TIMESTAMP];
    break;
  case TSDBW_COARSE_TIMESTAMP:
    tsdb_h = db_set_h->db_hs[TSDBW_COARSE_TIMESTAMP];
    break;
  default:
    return -1;
  }

  if (check_args_query(tsdb_h, epoch_from, epoch_to, metrics, metrics_num )) return -1;

  u_int32_t *epochs_list = NULL, epoch_num = 0;
  get_list_of_epochs(tsdb_h, epoch_from, epoch_to, &epochs_list, &epoch_num);

  /* Invoke tsdb_get() for every non zero epoch, for every metric */
  //TODO

  return 0;
}
