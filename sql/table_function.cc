/*
  Copyright (c) 2020, MariaDB Corporation

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

#include "mariadb.h"
#include "sql_priv.h"
#include "sql_class.h" /* TMP_TABLE_PARAM */
#include "table.h"
#include "item_jsonfunc.h"
#include "table_function.h"


class table_function_handlerton
{
public:
  handlerton m_hton;
  table_function_handlerton()
  {
    bzero(&m_hton, sizeof(m_hton));
    m_hton.tablefile_extensions= hton_no_exts;
    m_hton.slot= HA_SLOT_UNDEF;
  }
};


static table_function_handlerton table_function_hton;


class ha_json_table: public handler
{
protected:
  Table_function_json_table *m_jt;
  String m_tmps;
  String *m_js;
public:
  ha_json_table(TABLE_SHARE *share_arg, Table_function_json_table *jt):
    handler(&table_function_hton.m_hton, share_arg), m_jt(jt)
  {
    /*
      set the mark_trx_read_write_done to avoid the
      handler::mark_trx_read_write_internal() call.
      It relies on &ha_thd()->ha_data[ht->slot].ha_info[0] to be set.
      But we don't set the ha_data for the ha_json_table, and
      that call makes no sence for ha_json_table.
   */
    mark_trx_read_write_done= 1;
  }
  ~ha_json_table() {}
  handler *clone(const char *name, MEM_ROOT *mem_root) { return NULL; }
  const char *index_type(uint inx) { return "NONE"; }
  /* Rows also use a fixed-size format */
  enum row_type get_row_type() const { return ROW_TYPE_FIXED; }
  ulonglong table_flags() const
  {
    return (HA_FAST_KEY_READ | HA_NO_BLOBS | HA_NULL_IN_KEY |
            HA_CAN_SQL_HANDLER |
            HA_REC_NOT_IN_SEQ | HA_NO_TRANSACTIONS |
            HA_HAS_RECORDS | HA_CAN_HASH_KEYS);
  }
  ulong index_flags(uint inx, uint part, bool all_parts) const
  {
    return HA_ONLY_WHOLE_INDEX | HA_KEY_SCAN_NOT_ROR;
  }
  uint max_supported_keys() const { return 1; }
  uint max_supported_key_part_length() const { return MAX_KEY_LENGTH; }
  double scan_time() { return 1000000.0; }
  double read_time(uint index, uint ranges, ha_rows rows) { return 0.0; }

  int open(const char *name, int mode, uint test_if_locked);
  int close(void) { return 0; }
  int rnd_init(bool scan);
  int rnd_next(uchar *buf);
  int rnd_pos(uchar * buf, uchar *pos) { return 1; }
  void position(const uchar *record) {}
  int can_continue_handler_scan() { return 1; }
  int info(uint);
  int extra(enum ha_extra_function operation);
  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
			     enum thr_lock_type lock_type)
    { return NULL; }
  int create(const char *name, TABLE *form, HA_CREATE_INFO *create_info)
    { return 1; }
private:
  void update_key_stats();
};


/*
  Helper class that creates the temporary table that
  represents the table function in the query.
*/
  
class Create_json_table: public Data_type_statistics
{
  // The following members are initialized only in start()
  Field **m_default_field;
  uchar	*m_bitmaps;
  // The following members are initialized in ctor
  uint m_temp_pool_slot;
  uint m_null_count;
public:
  Create_json_table(const TMP_TABLE_PARAM *param,
                    bool save_sum_fields)
   :m_temp_pool_slot(MY_BIT_NONE),
    m_null_count(0)
  { }

  void add_field(TABLE *table, Field *field, uint fieldnr, bool force_not_null_cols);

  TABLE *start(THD *thd,
               TMP_TABLE_PARAM *param,
               Table_function_json_table *jt,
               const LEX_CSTRING *table_alias);

  bool add_json_table_fields(THD *thd, TABLE *table,
                             Table_function_json_table *jt);
  bool finalize(THD *thd, TABLE *table, TMP_TABLE_PARAM *param,
                Table_function_json_table *jt);
};


void Json_table_nested_path::scan_start(CHARSET_INFO *i_cs,
                                        const uchar *str, const uchar *end)
{
  json_get_path_start(&m_engine, i_cs, str, end, &m_cur_path);
  m_cur_nested= 0;
  m_null= false;
  m_ordinality_counter= 0;
}


int Json_table_nested_path::scan_next()
{
  if (m_cur_nested)
  {
    for (;;)
    {
      if (m_cur_nested->scan_next() == 0)
        return 0;
      if (!(m_cur_nested= m_cur_nested->m_next_nested))
        break;
handle_new_nested:
      m_cur_nested->scan_start(m_engine.s.cs, m_engine.value_begin,
                               m_engine.s.str_end);
    }
  }

  DBUG_ASSERT(!m_cur_nested);

  while (!json_get_path_next(&m_engine, &m_cur_path))
  {
    if (json_path_compare(&m_path, &m_cur_path, m_engine.value_type))
      continue;
    /* path found. */
    ++m_ordinality_counter;

    if (!m_nested)
      return 0;

    m_cur_nested= m_nested;
    goto handle_new_nested;
  }

  m_null= true;
  return 1;
}


int ha_json_table::open(const char *name, int mode, uint test_if_locked)
{
  return 0;
}


int ha_json_table::extra(enum ha_extra_function operation)
{
  return 0;
}


int ha_json_table::rnd_init(bool scan)
{
  Json_table_nested_path &p= m_jt->m_nested_path;
  DBUG_ENTER("ha_json_table::rnd_init");

  if ((m_js= m_jt->m_json->val_str(&m_tmps)))
  {
    p.scan_start(m_js->charset(),
                 (const uchar *) m_js->ptr(), (const uchar *) m_js->end());
  }

  DBUG_RETURN(0);
}


int ha_json_table::rnd_next(uchar *buf)
{
  Field **f= table->field;
  Json_table_column *jc;

  if (!m_js)
    return HA_ERR_END_OF_FILE;

  if (m_jt->m_nested_path.scan_next())
  {
    if (m_jt->m_nested_path.m_engine.s.error)
    {
      report_json_error_ex(m_js->ptr(), &m_jt->m_nested_path.m_engine,
          "JSON_TABLE", 0, Sql_condition::WARN_LEVEL_ERROR);

      return HA_ERR_INVALID_JSON;
    }
    return HA_ERR_END_OF_FILE;
  }
  
  List_iterator_fast<Json_table_column> jc_i(m_jt->m_columns);
  my_ptrdiff_t ptrdiff= buf - table->record[0];
  while ((jc= jc_i++))
  {
    if (ptrdiff)
      (*f)->move_field_offset(ptrdiff);
    switch (jc->m_column_type)
    {
    case Json_table_column::FOR_ORDINALITY:
      (*f)->set_notnull();
      (*f)->store(jc->m_nest->m_ordinality_counter, TRUE);
      break;
    case Json_table_column::PATH:
    case Json_table_column::EXISTS_PATH:
    {
      json_engine_t je;
      json_engine_t &nest_je= jc->m_nest->m_engine;
      json_path_step_t *cur_step;
      uint array_counters[JSON_DEPTH_LIMIT];
      int not_found;

      if (jc->m_nest->m_null)
      {
        (*f)->set_null();
        break;
      }
      json_scan_start(&je, nest_je.s.cs,
                      nest_je.value_begin, nest_je.s.str_end);

      cur_step= jc->m_path.steps;
      not_found= json_find_path(&je, &jc->m_path, &cur_step, array_counters) ||
                 json_read_value(&je);

      if (jc->m_column_type == Json_table_column::EXISTS_PATH)
      {
        (*f)->set_notnull();
        (*f)->store(!not_found);
      }
      else /*PATH*/
      {
        Json_table_column::On_response *r= NULL;

        if (not_found)
          r= (jc->m_on_empty.m_response ==
                Json_table_column::RESPONSE_NOT_SPECIFIED) ?
            &jc->m_on_error : &jc->m_on_empty;
        else
        {
          (*f)->set_notnull();
          if (!json_value_scalar(&je) ||
              (*f)->store((const char *) je.value,
                          (uint32) je.value_len, je.s.cs))
            r= &jc->m_on_error;
        }
        if (r)
        {
          switch (r->m_response)
          {
          case Json_table_column::RESPONSE_NOT_SPECIFIED:
          case Json_table_column::RESPONSE_NULL:
            (*f)->set_null();
          break;
          case Json_table_column::RESPONSE_ERROR:
            (*f)->set_null();
            my_error(ER_JSON_TABLE_ERROR_ON_FIELD, MYF(0),
                (*f)->field_name.str, table->alias.ptr());
          break;
          case Json_table_column::RESPONSE_DEFAULT:
            (*f)->set_notnull();
            (*f)->store(r->m_default.str,
                        r->m_default.length, jc->m_defaults_cs);
          break;
          }
        }
      }
      break;
    }
    };
    if (ptrdiff)
      (*f)->move_field_offset(-ptrdiff);
    f++;
  }
  return 0;
}


int ha_json_table::info(uint)
{
  /* We don't want 0 or 1 in stats.records. */
  stats.records= 4;
  return 0;
}


static void
setup_tmp_table_column_bitmaps(TABLE *table, uchar *bitmaps, uint field_count)
{
  uint bitmap_size= bitmap_buffer_size(field_count);

  DBUG_ASSERT(table->s->virtual_fields == 0);

  my_bitmap_init(&table->def_read_set, (my_bitmap_map*) bitmaps, field_count,
              FALSE);
  bitmaps+= bitmap_size;
  my_bitmap_init(&table->tmp_set,
                 (my_bitmap_map*) bitmaps, field_count, FALSE);
  bitmaps+= bitmap_size;
  my_bitmap_init(&table->eq_join_set,
                 (my_bitmap_map*) bitmaps, field_count, FALSE);
  bitmaps+= bitmap_size;
  my_bitmap_init(&table->cond_set,
                 (my_bitmap_map*) bitmaps, field_count, FALSE);
  bitmaps+= bitmap_size;
  my_bitmap_init(&table->has_value_set,
                 (my_bitmap_map*) bitmaps, field_count, FALSE);
  /* write_set and all_set are copies of read_set */
  table->def_write_set= table->def_read_set;
  table->s->all_set= table->def_read_set;
  bitmap_set_all(&table->s->all_set);
  table->default_column_bitmaps();
}


void Create_json_table::add_field(TABLE *table, Field *field,
                                  uint fieldnr, bool force_not_null_cols)
{
  DBUG_ASSERT(!field->field_name.str ||
              strlen(field->field_name.str) == field->field_name.length);

  if (force_not_null_cols)
  {
    field->flags|= NOT_NULL_FLAG;
    field->null_ptr= NULL;
  }

  if (!(field->flags & NOT_NULL_FLAG))
    m_null_count++;

  table->s->reclength+= field->pack_length();

  // Assign it here, before update_data_type_statistics() changes m_blob_count
  if (field->flags & BLOB_FLAG)
    table->s->blob_field[m_blob_count]= fieldnr;

  table->field[fieldnr]= field;
  field->field_index= fieldnr;

  field->update_data_type_statistics(this);
}


/**
  Create a json table according to a field list.

  @param thd                  thread handle
  @param param                a description used as input to create the table
  @param jt                   json_table specificaion
  @param table_alias          alias
*/

TABLE *Create_json_table::start(THD *thd,
                               TMP_TABLE_PARAM *param,
                               Table_function_json_table *jt,
                               const LEX_CSTRING *table_alias)
{
  MEM_ROOT *mem_root_save, own_root;
  TABLE *table;
  TABLE_SHARE *share;
  uint  copy_func_count= param->func_count;
  char  *tmpname,path[FN_REFLEN];
  Field **reg_field;
  uint *blob_field;
  DBUG_ENTER("Create_json_table::start");
  DBUG_PRINT("enter",
             ("table_alias: '%s'  ", table_alias->str));

  if (use_temp_pool && !(test_flags & TEST_KEEP_TMP_TABLES))
    m_temp_pool_slot = bitmap_lock_set_next(&temp_pool);

  if (m_temp_pool_slot != MY_BIT_NONE) // we got a slot
    sprintf(path, "%s-%lx-%i", tmp_file_prefix,
            current_pid, m_temp_pool_slot);
  else
  {
    /* if we run out of slots or we are not using tempool */
    sprintf(path, "%s-%lx-%lx-%x", tmp_file_prefix,current_pid,
            (ulong) thd->thread_id, thd->tmp_table++);
  }

  /*
    No need to change table name to lower case.
  */
  fn_format(path, path, mysql_tmpdir, "",
            MY_REPLACE_EXT|MY_UNPACK_FILENAME);

  const uint field_count= param->field_count;
  DBUG_ASSERT(field_count);

  init_sql_alloc(&own_root, "tmp_table", TABLE_ALLOC_BLOCK_SIZE, 0,
                 MYF(MY_THREAD_SPECIFIC));

  if (!multi_alloc_root(&own_root,
                        &table, sizeof(*table),
                        &share, sizeof(*share),
                        &reg_field, sizeof(Field*) * (field_count+1),
                        &m_default_field, sizeof(Field*) * (field_count),
                        &blob_field, sizeof(uint)*(field_count+1),
                        &param->items_to_copy,
                          sizeof(param->items_to_copy[0])*(copy_func_count+1),
                        &param->keyinfo, sizeof(*param->keyinfo),
                        &param->start_recinfo,
                        sizeof(*param->recinfo)*(field_count*2+4),
                        &tmpname, (uint) strlen(path)+1,
                        &m_bitmaps, bitmap_buffer_size(field_count)*6,
                        NullS))
  {
    DBUG_RETURN(NULL);				/* purecov: inspected */
  }
  strmov(tmpname, path);
  /* make table according to fields */

  bzero((char*) table,sizeof(*table));
  bzero((char*) reg_field, sizeof(Field*) * (field_count+1));
  bzero((char*) m_default_field, sizeof(Field*) * (field_count));

  table->mem_root= own_root;
  mem_root_save= thd->mem_root;
  thd->mem_root= &table->mem_root;

  table->field=reg_field;
  table->alias.set(table_alias->str, table_alias->length, table_alias_charset);

  table->reginfo.lock_type=TL_WRITE;	/* Will be updated */
  table->map=1;
  table->temp_pool_slot= m_temp_pool_slot;
  table->copy_blobs= 1;
  table->in_use= thd;
  table->no_rows_with_nulls= param->force_not_null_cols;
  table->update_handler= NULL;
  table->check_unique_buf= NULL;

  table->s= share;
  init_tmp_table_share(thd, share, "", 0, "(temporary)", tmpname);
  share->blob_field= blob_field;
  share->table_charset= param->table_charset;
  share->primary_key= MAX_KEY;               // Indicate no primary key
  if (param->schema_table)
    share->db= INFORMATION_SCHEMA_NAME;

  param->using_outer_summary_function= 0;

  share->db_plugin= NULL;
  if (!(table->file= new (&table->mem_root) ha_json_table(share, jt)))
    DBUG_RETURN(NULL);

  table->file->init();

  thd->mem_root= mem_root_save;
  DBUG_RETURN(table);
}


bool Create_json_table::finalize(THD *thd, TABLE *table,
                                 TMP_TABLE_PARAM *param,
                                 Table_function_json_table *jt)
{
  DBUG_ENTER("Create_json_table::finalize");
  DBUG_ASSERT(table);

  uint null_pack_length;
  bool  use_packed_rows= false;
  uchar *pos;
  uchar *null_flags;
  TMP_ENGINE_COLUMNDEF *recinfo;
  TABLE_SHARE  *share= table->s;

  MEM_ROOT *mem_root_save= thd->mem_root;
  thd->mem_root= &table->mem_root;

  DBUG_ASSERT(param->field_count >= share->fields);
  DBUG_ASSERT(param->field_count >= share->blob_fields);

  if (table->file->set_ha_share_ref(&share->ha_share))
  {
    delete table->file;
    goto err;
  }

  if (share->blob_fields == 0)
    m_null_count++;

  null_pack_length= (m_null_count + m_uneven_bit_length + 7) / 8;
  share->reclength+= null_pack_length;
  if (!share->reclength)
    share->reclength= 1;                // Dummy select

  {
    uint alloc_length= ALIGN_SIZE(share->reclength + MI_UNIQUE_HASH_LENGTH+1);
    share->rec_buff_length= alloc_length;
    if (!(table->record[0]= (uchar*)
                            alloc_root(&table->mem_root, alloc_length*3)))
      goto err;
    table->record[1]= table->record[0]+alloc_length;
    share->default_values= table->record[1]+alloc_length;
  }

  setup_tmp_table_column_bitmaps(table, m_bitmaps, table->s->fields);

  recinfo=param->start_recinfo;
  null_flags=(uchar*) table->record[0];
  pos=table->record[0]+ null_pack_length;
  if (null_pack_length)
  {
    bzero((uchar*) recinfo,sizeof(*recinfo));
    recinfo->type=FIELD_NORMAL;
    recinfo->length=null_pack_length;
    recinfo++;
    bfill(null_flags,null_pack_length,255);	// Set null fields

    table->null_flags= (uchar*) table->record[0];
    share->null_fields= m_null_count;
    share->null_bytes= share->null_bytes_for_compare= null_pack_length;
  }
  m_null_count= (share->blob_fields == 0) ? 1 : 0;
  for (uint i= 0; i < share->fields; i++, recinfo++)
  {
    Field *field= table->field[i];
    uint length;
    bzero((uchar*) recinfo,sizeof(*recinfo));

    if (!(field->flags & NOT_NULL_FLAG))
    {
      recinfo->null_bit= (uint8)1 << (m_null_count & 7);
      recinfo->null_pos= m_null_count/8;
      field->move_field(pos, null_flags + m_null_count/8,
			(uint8)1 << (m_null_count & 7));
      m_null_count++;
    }
    else
      field->move_field(pos,(uchar*) 0,0);
    if (field->type() == MYSQL_TYPE_BIT)
    {
      /* We have to reserve place for extra bits among null bits */
      ((Field_bit*) field)->set_bit_ptr(null_flags + m_null_count / 8,
                                        m_null_count & 7);
      m_null_count+= (field->field_length & 7);
    }
    field->reset();

    /*
      Test if there is a default field value. The test for ->ptr is to skip
      'offset' fields generated by initialize_tables
    */
    if (m_default_field[i] && m_default_field[i]->ptr)
    {
      /* 
         default_field[i] is set only in the cases  when 'field' can
         inherit the default value that is defined for the field referred
         by the Item_field object from which 'field' has been created.
      */
      const Field *orig_field= m_default_field[i];
      /* Get the value from default_values */
      if (orig_field->is_null_in_record(orig_field->table->s->default_values))
        field->set_null();
      else
      {
        field->set_notnull();
        memcpy(field->ptr,
               orig_field->ptr_in_record(orig_field->table->s->default_values),
               field->pack_length_in_rec());
      }
    } 

    length=field->pack_length();
    pos+= length;

    /* Make entry for create table */
    recinfo->length=length;
    recinfo->type= field->tmp_engine_column_type(use_packed_rows);

    // fix table name in field entry
    field->set_table_name(&table->alias);
  }

  param->recinfo= recinfo;              	// Pointer to after last field
  store_record(table,s->default_values);        // Make empty default record

  share->max_rows= ~(ha_rows) 0;
  param->end_write_records= HA_POS_ERROR;

  share->db_record_offset= 1;

  if (unlikely(table->file->ha_open(table, table->s->path.str, O_RDWR,
                             HA_OPEN_TMP_TABLE | HA_OPEN_INTERNAL_TABLE)))
    goto err;

  table->db_stat= HA_OPEN_KEYFILE;
  table->set_created();

  thd->mem_root= mem_root_save;

  DBUG_RETURN(false);

err:
  thd->mem_root= mem_root_save;
  DBUG_RETURN(true);
}


bool Create_json_table::add_json_table_fields(THD *thd, TABLE *table,
                                              Table_function_json_table *jt)
{
  TABLE_SHARE *share= table->s;
  Json_table_column *jc;
  uint fieldnr= 0;
  MEM_ROOT *mem_root_save= thd->mem_root;
  List_iterator_fast<Json_table_column> jc_i(jt->m_columns);

  DBUG_ENTER("add_json_table_fields");

  thd->mem_root= &table->mem_root;

  while ((jc= jc_i++))
  {
    Create_field *sql_f= jc->m_field;
    List_iterator_fast<Json_table_column> it2(jt->m_columns);
    Json_table_column *jc2;
    if (!sql_f->charset)
      sql_f->charset= thd->variables.collation_server;

    if (sql_f->prepare_stage1(thd, thd->mem_root, table->file,
                              table->file->ha_table_flags()))
      goto err_exit;

    while ((jc2= it2++) != jc)
    {
      if (lex_string_cmp(system_charset_info,
            &sql_f->field_name, &jc2->m_field->field_name) == 0)
      {
        my_error(ER_DUP_FIELDNAME, MYF(0), sql_f->field_name.str);
        goto err_exit;
      }
    }
    it2.rewind();
  }

  jc_i.rewind();

  while ((jc= jc_i++))
  {
    Create_field *sql_f= jc->m_field;
    Record_addr addr(!(sql_f->flags && NOT_NULL_FLAG));
    Bit_addr bit(addr.null());

    sql_f->prepare_stage2(table->file, table->file->ha_table_flags());

    if (!sql_f->charset)
      sql_f->charset= &my_charset_utf8mb4_bin;

    Field *f= sql_f->type_handler()->make_table_field_from_def(share,
        thd->mem_root, &sql_f->field_name, addr, bit, sql_f, sql_f->flags);
    if (!f)
      goto err_exit;
    f->init(table);
    add_field(table, f, fieldnr++, FALSE);
  }

  share->fields= fieldnr;
  share->blob_fields= m_blob_count;
  table->field[fieldnr]= 0;                     // End marker
  share->blob_field[m_blob_count]= 0;           // End marker
  share->column_bitmap_size= bitmap_buffer_size(share->fields);

  thd->mem_root= mem_root_save;

  DBUG_RETURN(FALSE);
err_exit:
  thd->mem_root= mem_root_save;
  DBUG_RETURN(TRUE);
}


TABLE *create_table_for_function(THD *thd, TABLE_LIST *sql_table)
{
  TMP_TABLE_PARAM tp;
  TABLE *table;
  uint field_count= sql_table->table_function->m_columns.elements+1;
  
  DBUG_ENTER("create_table_for_function");

  tp.init();
  tp.table_charset= system_charset_info;
  tp.field_count= field_count;
  {
    Create_json_table maker(&tp, false);

    if (!(table= maker.start(thd, &tp,
                             sql_table->table_function, &sql_table->alias)) ||
        maker.add_json_table_fields(thd, table, sql_table->table_function) ||
        maker.finalize(thd, table, &tp, sql_table->table_function))
    {
      if (table)
        free_tmp_table(thd, table);
      DBUG_RETURN(NULL);
    }
  }
  sql_table->schema_table_name.length= 0;

  my_bitmap_map* bitmaps=
    (my_bitmap_map*) thd->alloc(bitmap_buffer_size(field_count));
  my_bitmap_init(&table->def_read_set, (my_bitmap_map*) bitmaps, field_count,
                 FALSE);
  table->read_set= &table->def_read_set;
  bitmap_clear_all(table->read_set);
  table->alias_name_used= true;
  table->next= thd->derived_tables;
  thd->derived_tables= table;
  table->s->tmp_table= INTERNAL_TMP_TABLE;
  table->grant.privilege= SELECT_ACL;

  sql_table->table= table;

  DBUG_RETURN(table);
}


int Json_table_column::set(THD *thd, enum_type ctype, const LEX_CSTRING &path)
{
  set(ctype);
  if (json_path_setup(&m_path, thd->variables.collation_connection,
        (const uchar *) path.str, (const uchar *)(path.str + path.length)))
  {
    report_path_error_ex(path.str, &m_path, "JSON_TABLE", 1,
                         Sql_condition::WARN_LEVEL_ERROR);
    return 1;
  }
  return 0;
}


int Json_table_nested_path::set_path(THD *thd, const LEX_CSTRING &path)
{
  if (json_path_setup(&m_path, thd->variables.collation_connection,
        (const uchar *) path.str, (const uchar *)(path.str + path.length)))
  {
    report_path_error_ex(path.str, &m_path, "JSON_TABLE", 1,
                         Sql_condition::WARN_LEVEL_ERROR);
    return 1;
  }
  return 0;
}


int Table_function_json_table::setup(THD *thd, TABLE_LIST *sql_table)
{
  thd->where= "JSON_TABLE argument";
  if (m_json->fix_fields(thd, &m_json))
    return TRUE;

  m_dep_tables= m_json->used_tables();

  if (m_dep_tables)
  {
    sql_table->dep_tables|= m_dep_tables;
    sql_table->table->no_cache= TRUE;
    if (unlikely(sql_table->dep_tables & sql_table->get_map()))
    {
      /* Table itself is used in the argument. */
      my_error(ER_WRONG_USAGE, MYF(0), "JSON_TABLE", "argument"); 
      return TRUE;
    }
  }

  return FALSE;
}

void Table_function_json_table::get_estimates(ha_rows *out_rows,
                                  double *scan_time, double *startup_cost)
{
  *out_rows= 40;
  *scan_time= 0.0;
  *startup_cost= 0.0;
}

