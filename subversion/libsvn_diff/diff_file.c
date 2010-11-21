/*
 * diff_file.c :  routines for doing diffs on files
 *
 * ====================================================================
 *    Licensed to the Apache Software Foundation (ASF) under one
 *    or more contributor license agreements.  See the NOTICE file
 *    distributed with this work for additional information
 *    regarding copyright ownership.  The ASF licenses this file
 *    to you under the Apache License, Version 2.0 (the
 *    "License"); you may not use this file except in compliance
 *    with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing,
 *    software distributed under the License is distributed on an
 *    "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 *    KIND, either express or implied.  See the License for the
 *    specific language governing permissions and limitations
 *    under the License.
 * ====================================================================
 */


#include <apr.h>
#include <apr_pools.h>
#include <apr_general.h>
#include <apr_file_io.h>
#include <apr_file_info.h>
#include <apr_time.h>
#include <apr_mmap.h>
#include <apr_getopt.h>

#include "svn_error.h"
#include "svn_diff.h"
#include "svn_types.h"
#include "svn_string.h"
#include "svn_subst.h"
#include "svn_io.h"
#include "svn_utf.h"
#include "svn_pools.h"
#include "diff.h"
#include "svn_private_config.h"
#include "svn_path.h"
#include "svn_ctype.h"

#include "private/svn_utf_private.h"
#include "private/svn_eol_private.h"

/* A token, i.e. a line read from a file. */
typedef struct svn_diff__file_token_t
{
  /* Next token in free list. */
  struct svn_diff__file_token_t *next;
  svn_diff_datasource_e datasource;
  /* Offset in the datasource. */
  apr_off_t offset;
  /* Offset of the normalized token (may skip leading whitespace) */
  apr_off_t norm_offset;
  /* Total length - before normalization. */
  apr_off_t raw_length;
  /* Total length - after normalization. */
  apr_off_t length;
} svn_diff__file_token_t;


typedef struct svn_diff__file_baton_t
{
  const svn_diff_file_options_t *options;

  struct file_info {
    const char *path;  /* path to this file, absolute or relative to CWD */

    /* All the following fields are active while this datasource is open */
    apr_file_t *file;  /* handle of this file */
    apr_off_t size;    /* total raw size in bytes of this file */

    /* The current chunk: CHUNK_SIZE bytes except for the last chunk. */
    int chunk;     /* the current chunk number, zero-based */
    char *buffer;  /* a buffer containing the current chunk */
    char *curp;    /* current position in the current chunk */
    char *endp;    /* next memory address after the current chunk */

    svn_diff__normalize_state_t normalize_state;

    /* Where the identical suffix starts in this datasource */
    int suffix_start_chunk;
    apr_off_t suffix_offset_in_chunk;
  } files[4];

  /* List of free tokens that may be reused. */
  svn_diff__file_token_t *tokens;

  apr_pool_t *pool;
} svn_diff__file_baton_t;

static int
datasource_to_index(svn_diff_datasource_e datasource)
{
  switch (datasource)
    {
    case svn_diff_datasource_original:
      return 0;

    case svn_diff_datasource_modified:
      return 1;

    case svn_diff_datasource_latest:
      return 2;

    case svn_diff_datasource_ancestor:
      return 3;
    }

  return -1;
}

/* Files are read in chunks of 128k.  There is no support for this number
 * whatsoever.  If there is a number someone comes up with that has some
 * argumentation, let's use that.
 */
#define CHUNK_SHIFT 17
#define CHUNK_SIZE (1 << CHUNK_SHIFT)

#define chunk_to_offset(chunk) ((chunk) << CHUNK_SHIFT)
#define offset_to_chunk(offset) ((offset) >> CHUNK_SHIFT)
#define offset_in_chunk(offset) ((offset) & (CHUNK_SIZE - 1))


/* Read a chunk from a FILE into BUFFER, starting from OFFSET, going for
 * *LENGTH.  The actual bytes read are stored in *LENGTH on return.
 */
static APR_INLINE svn_error_t *
read_chunk(apr_file_t *file, const char *path,
           char *buffer, apr_off_t length,
           apr_off_t offset, apr_pool_t *pool)
{
  /* XXX: The final offset may not be the one we asked for.
   * XXX: Check.
   */
  SVN_ERR(svn_io_file_seek(file, APR_SET, &offset, pool));
  return svn_io_file_read_full(file, buffer, (apr_size_t) length, NULL, pool);
}


/* Map or read a file at PATH. *BUFFER will point to the file
 * contents; if the file was mapped, *FILE and *MM will contain the
 * mmap context; otherwise they will be NULL.  SIZE will contain the
 * file size.  Allocate from POOL.
 */
#if APR_HAS_MMAP
#define MMAP_T_PARAM(NAME) apr_mmap_t **NAME,
#define MMAP_T_ARG(NAME)   &(NAME),
#else
#define MMAP_T_PARAM(NAME)
#define MMAP_T_ARG(NAME)
#endif

static svn_error_t *
map_or_read_file(apr_file_t **file,
                 MMAP_T_PARAM(mm)
                 char **buffer, apr_off_t *size,
                 const char *path, apr_pool_t *pool)
{
  apr_finfo_t finfo;
  apr_status_t rv;

  *buffer = NULL;

  SVN_ERR(svn_io_file_open(file, path, APR_READ, APR_OS_DEFAULT, pool));
  SVN_ERR(svn_io_file_info_get(&finfo, APR_FINFO_SIZE, *file, pool));

#if APR_HAS_MMAP
  if (finfo.size > APR_MMAP_THRESHOLD)
    {
      rv = apr_mmap_create(mm, *file, 0, (apr_size_t) finfo.size,
                           APR_MMAP_READ, pool);
      if (rv == APR_SUCCESS)
        {
          *buffer = (*mm)->mm;
        }

      /* On failure we just fall through and try reading the file into
       * memory instead.
       */
    }
#endif /* APR_HAS_MMAP */

   if (*buffer == NULL && finfo.size > 0)
    {
      *buffer = apr_palloc(pool, (apr_size_t) finfo.size);

      SVN_ERR(svn_io_file_read_full(*file, *buffer, (apr_size_t) finfo.size,
                                    NULL, pool));

      /* Since we have the entire contents of the file we can
       * close it now.
       */
      SVN_ERR(svn_io_file_close(*file, pool));

      *file = NULL;
    }

  *size = finfo.size;

  return SVN_NO_ERROR;
}


/* Let FILE stand for the file_info struct element of BATON->files that is
 * indexed by DATASOURCE.  BATON's type is (svn_diff__file_baton_t *).
 *
 * Open the file at FILE.path; initialize FILE.file, FILE.size, FILE.buffer,
 * FILE.curp and FILE.endp; allocate a buffer and read the first chunk.
 *
 * Implements svn_diff_fns_t::datasource_open. */
static svn_error_t *
datasource_open(void *baton, svn_diff_datasource_e datasource)
{
  svn_diff__file_baton_t *file_baton = baton;
  struct file_info *file = &file_baton->files[datasource_to_index(datasource)];
  apr_finfo_t finfo;
  apr_off_t length;
  char *curp;
  char *endp;

  SVN_ERR(svn_io_file_open(&file->file, file->path,
                           APR_READ, APR_OS_DEFAULT, file_baton->pool));

  SVN_ERR(svn_io_file_info_get(&finfo, APR_FINFO_SIZE,
                               file->file, file_baton->pool));

  file->size = finfo.size;
  length = finfo.size > CHUNK_SIZE ? CHUNK_SIZE : finfo.size;

  if (length == 0)
    return SVN_NO_ERROR;

  endp = curp = apr_palloc(file_baton->pool, (apr_size_t) length);
  endp += length;

  file->buffer = file->curp = curp;
  file->endp = endp;

  return read_chunk(file->file, file->path,
                    curp, length, 0, file_baton->pool);
}


/* For all files in the FILE array, increment the curp pointer.  If a file
 * points before the beginning of file, let it point at the first byte again.
 * If the end of the current chunk is reached, read the next chunk in the
 * buffer and point curp to the start of the chunk.  If EOF is reached, set
 * curp equal to endp to indicate EOF. */
static svn_error_t *
increment_pointers(struct file_info *file[], int file_len, apr_pool_t *pool)
{
  int i;

  for (i = 0; i < file_len; i++)
    if (file[i]->chunk == -1) /* indicates before beginning of file */
      {
        file[i]->chunk = 0; /* point to beginning of file again */
      }
    else if (file[i]->curp == file[i]->endp - 1)
      {
        apr_off_t last_chunk = offset_to_chunk(file[i]->size);
        if (file[i]->chunk == last_chunk)
          {
            file[i]->curp++; /* curp == endp signals end of file */
          }
        else
          {
            apr_off_t length;
            file[i]->chunk++;
            length = file[i]->chunk == last_chunk ? 
              offset_in_chunk(file[i]->size) : CHUNK_SIZE;
            SVN_ERR(read_chunk(file[i]->file, file[i]->path, file[i]->buffer,
                               length, chunk_to_offset(file[i]->chunk),
                               pool));
            file[i]->endp = file[i]->buffer + length;
            file[i]->curp = file[i]->buffer;
          }
      }
    else
      {
        file[i]->curp++;
      }

  return SVN_NO_ERROR;
}

/* For all files in the FILE array, decrement the curp pointer.  If the
 * start of a chunk is reached, read the previous chunk in the buffer and
 * point curp to the last byte of the chunk.  If the beginning of a FILE is
 * reached, set chunk to -1 to indicate BOF. */
static svn_error_t *
decrement_pointers(struct file_info *file[], int file_len, apr_pool_t *pool)
{
  int i;

  for (i = 0; i < file_len; i++)
    if (file[i]->curp == file[i]->buffer)
      {
        if (file[i]->chunk == 0)
          file[i]->chunk--; /* chunk == -1 signals beginning of file */
        else
          {
            file[i]->chunk--;
            SVN_ERR(read_chunk(file[i]->file, file[i]->path, file[i]->buffer,
                               CHUNK_SIZE, chunk_to_offset(file[i]->chunk),
                               pool));
            file[i]->endp = file[i]->buffer + CHUNK_SIZE;
            file[i]->curp = file[i]->endp - 1;
          }
      }
    else
      {
        file[i]->curp--;
      }

  return SVN_NO_ERROR;
}

/* Check whether one of the FILEs has its pointers 'before' the beginning of
 * the file (this can happen while scanning backwards). This is the case if
 * one of them has chunk == -1. */
static svn_boolean_t
is_one_at_bof(struct file_info *file[], int file_len)
{
  int i;

  for (i = 0; i < file_len; i++)
    if (file[i]->chunk == -1)
      return TRUE;

  return FALSE;
}

/* Check whether one of the FILEs has its pointers at EOF (this is the case if
 * one of them has curp == endp (this can only happen at the last chunk)) */
static svn_boolean_t
is_one_at_eof(struct file_info *file[], int file_len)
{
  int i;

  for (i = 0; i < file_len; i++)
    if (file[i]->curp == file[i]->endp)
      return TRUE;

  return FALSE;
}

/* Find the prefix which is identical between all elements of the FILE array.
 * Return the number of prefix lines in PREFIX_LINES.  REACHED_ONE_EOF will be
 * set to TRUE if one of the FILEs reached its end while scanning prefix,
 * i.e. at least one file consisted entirely of prefix.  Otherwise, 
 * REACHED_ONE_EOF is set to FALSE.
 *
 * After this function is finished, the buffers, chunks, curp's and endp's 
 * of the FILEs are set to point at the first byte after the prefix. */
static svn_error_t *
find_identical_prefix(svn_boolean_t *reached_one_eof, apr_off_t *prefix_lines,
                      struct file_info *file[], int file_len,
                      apr_pool_t *pool)
{
  svn_boolean_t had_cr = FALSE;
  svn_boolean_t is_match, reached_all_eof;
  int i;

  *prefix_lines = 0;
  for (i = 1, is_match = TRUE; i < file_len; i++)
    is_match = is_match && *file[0]->curp == *file[i]->curp;
  while (is_match)
    {
      /* ### TODO: see if we can take advantage of 
         diff options like ignore_eol_style or ignore_space. */
      /* check for eol, and count */
      if (*file[0]->curp == '\r')
        {
          (*prefix_lines)++;
          had_cr = TRUE;
        }
      else if (*file[0]->curp == '\n' && !had_cr)
        {
          (*prefix_lines)++;
          had_cr = FALSE;
        }
      else 
        {
          had_cr = FALSE;
        }

      increment_pointers(file, file_len, pool);

      /* curp == endp indicates EOF (this can only happen with last chunk) */
      *reached_one_eof = is_one_at_eof(file, file_len);
      if (*reached_one_eof)
        break;
      else
        for (i = 1, is_match = TRUE; i < file_len; i++)
          is_match = is_match && *file[0]->curp == *file[i]->curp;
    }

  /* If all files reached their end (i.e. are fully identical), we're done */
  for (i = 0, reached_all_eof = TRUE; i < file_len; i++)
    reached_all_eof = reached_all_eof && file[i]->curp == file[i]->endp;
  if (reached_all_eof)
    return SVN_NO_ERROR;

  if (had_cr)
    {
      /* Check if we ended in the middle of a \r\n for one file, but \r for 
         another. If so, back up one byte, so the next loop will back up
         the entire line. Also decrement *prefix_lines, since we counted one
         too many for the \r. */
      svn_boolean_t ended_at_nonmatching_newline = FALSE;
      for (i = 0; i < file_len; i++)
        ended_at_nonmatching_newline = ended_at_nonmatching_newline 
                                       || *file[i]->curp == '\n';
      if (ended_at_nonmatching_newline)
        {
          (*prefix_lines)--;
          decrement_pointers(file, file_len, pool);
        }
    }

  /* Back up one byte, so we point at the last identical byte */
  decrement_pointers(file, file_len, pool);

  /* Back up to the last eol sequence (\n, \r\n or \r) */
  while (!is_one_at_bof(file, file_len) && 
         *file[0]->curp != '\n' && *file[0]->curp != '\r')
    decrement_pointers(file, file_len, pool);

  /* Slide one byte forward, to point past the eol sequence */
  increment_pointers(file, file_len, pool);

  return SVN_NO_ERROR;
}


#define SUFFIX_LINES_TO_KEEP 50

/* Find the suffix which is identical between all elements of the FILE array.
 *
 * Before this function is called the FILEs' pointers and chunks should be 
 * positioned right after the identical prefix (which is the case after 
 * find_identical_prefix), so we can determine where suffix scanning should 
 * ultimately stop. */
static svn_error_t *
find_identical_suffix(struct file_info *file[], int file_len,
                      apr_pool_t *pool)
{
  struct file_info file_for_suffix[4];
  struct file_info *file_for_suffix_ptr[4];
  apr_off_t length[4];
  apr_off_t suffix_min_chunk0;
  apr_off_t suffix_min_offset0;
  apr_off_t min_file_size;
  int suffix_lines_to_keep = SUFFIX_LINES_TO_KEEP;
  svn_boolean_t is_match, reached_prefix;
  int i;

  for (i = 0; i < file_len; i++)
    {
      memset(&file_for_suffix[i], 0, sizeof(file_for_suffix[i]));
      file_for_suffix_ptr[i] = &file_for_suffix[i];
    }

  /* Initialize file_for_suffix[].
     Read last chunk, position curp at last byte. */
  for (i = 0; i < file_len; i++)
    {
      file_for_suffix[i].path = file[i]->path;
      file_for_suffix[i].file = file[i]->file;
      file_for_suffix[i].size = file[i]->size;
      file_for_suffix[i].chunk =
        (int) offset_to_chunk(file_for_suffix[i].size); /* last chunk */
      length[i] = offset_in_chunk(file_for_suffix[i].size);
      if (file_for_suffix[i].chunk == file[i]->chunk)
        {
          /* Prefix ended in last chunk, so we can reuse the prefix buffer */
          file_for_suffix[i].buffer = file[i]->buffer;
        }
      else
        {
          /* There is at least more than 1 chunk,
             so allocate full chunk size buffer */
          file_for_suffix[i].buffer = apr_palloc(pool, CHUNK_SIZE);
          SVN_ERR(read_chunk(file_for_suffix[i].file, file_for_suffix[i].path,
                             file_for_suffix[i].buffer, length[i],
                             chunk_to_offset(file_for_suffix[i].chunk),
                             pool));
        }
      file_for_suffix[i].endp = file_for_suffix[i].buffer + length[i];
      file_for_suffix[i].curp = file_for_suffix[i].endp - 1;
    }

  /* Get the chunk and pointer offset (for file[0]) at which we should stop
     scanning backward for the identical suffix, i.e. when we reach prefix. */
  suffix_min_chunk0 = file[0]->chunk;
  suffix_min_offset0 = file[0]->curp - file[0]->buffer;

  /* Compensate if other files are smaller than file[0] */
  for (i = 1, min_file_size = file[0]->size; i < file_len; i++)
    if (file[i]->size < min_file_size)
      min_file_size = file[i]->size;
  if (file[0]->size > min_file_size)
    {
      suffix_min_chunk0 += (file[0]->size - min_file_size) / CHUNK_SIZE;
      suffix_min_offset0 += (file[0]->size - min_file_size) % CHUNK_SIZE;
    }

  /* Scan backwards until mismatch or until we reach the prefix. */
  for (i = 1, is_match = TRUE; i < file_len; i++)
    is_match =
      is_match && *file_for_suffix[0].curp == *file_for_suffix[i].curp;
  while (is_match)
    {
      decrement_pointers(file_for_suffix_ptr, file_len, pool);
      
      reached_prefix = file_for_suffix[0].chunk == suffix_min_chunk0 
                       && (file_for_suffix[0].curp - file_for_suffix[0].buffer)
                          == suffix_min_offset0;

      if (reached_prefix || is_one_at_bof(file_for_suffix_ptr, file_len))
        break;
      else
        for (i = 1, is_match = TRUE; i < file_len; i++)
          is_match =
            is_match && *file_for_suffix[0].curp == *file_for_suffix[i].curp;
    }

  /* Slide one byte forward, to point at the first byte of identical suffix */
  increment_pointers(file_for_suffix_ptr, file_len, pool);

  /* Slide forward until we find an eol sequence to add the rest of the line
     we're in. Then add SUFFIX_LINES_TO_KEEP more lines. Stop if at least 
     one file reaches its end. */
  do
    {
      while (!is_one_at_eof(file_for_suffix_ptr, file_len)
             && *file_for_suffix[0].curp != '\n'
             && *file_for_suffix[0].curp != '\r')
        increment_pointers(file_for_suffix_ptr, file_len, pool);

      /* Slide one or two more bytes, to point past the eol. */
      if (!is_one_at_eof(file_for_suffix_ptr, file_len)
          && *file_for_suffix[0].curp == '\r')
        increment_pointers(file_for_suffix_ptr, file_len, pool);
      if (!is_one_at_eof(file_for_suffix_ptr, file_len)
          && *file_for_suffix[0].curp == '\n')
        increment_pointers(file_for_suffix_ptr, file_len, pool);
    }
  while (!is_one_at_eof(file_for_suffix_ptr, file_len) 
         && suffix_lines_to_keep--);

  /* Save the final suffix information in the original file_info */
  for (i = 0; i < file_len; i++)
    {
      file[i]->suffix_start_chunk = file_for_suffix[i].chunk;
      file[i]->suffix_offset_in_chunk = 
        file_for_suffix[i].curp - file_for_suffix[i].buffer;
    }

  return SVN_NO_ERROR;
}


/* Let FILE stand for the array of file_info struct elements of BATON->files
 * that are indexed by the elements of the DATASOURCE array.
 * BATON's type is (svn_diff__file_baton_t *).
 *
 * For each file in the FILE array, open the file at FILE.path; initialize 
 * FILE.file, FILE.size, FILE.buffer, FILE.curp and FILE.endp; allocate a 
 * buffer and read the first chunk.  Then find the prefix and suffix lines
 * which are identical between all the files.  Return the number of identical
 * prefix lines in PREFIX_LINES.
 *
 * Finding the identical prefix and suffix allows us to exclude those from the
 * rest of the diff algorithm, which increases performance by reducing the 
 * problem space.
 *
 * Implements svn_diff_fns_t::datasources_open. */
static svn_error_t *
datasources_open(void *baton, apr_off_t *prefix_lines,
                 svn_diff_datasource_e datasource[],
                 int datasource_len)
{
  svn_diff__file_baton_t *file_baton = baton;
  struct file_info *file[4];
  apr_finfo_t finfo[4];
  apr_off_t length[4];
  svn_boolean_t reached_one_eof;
  int i;

  /* Open datasources and read first chunk */
  for (i = 0; i < datasource_len; i++)
    {
      file[i] = &file_baton->files[datasource_to_index(datasource[i])];
      SVN_ERR(svn_io_file_open(&file[i]->file, file[i]->path,
                               APR_READ, APR_OS_DEFAULT, file_baton->pool));
      SVN_ERR(svn_io_file_info_get(&finfo[i], APR_FINFO_SIZE,
                                   file[i]->file, file_baton->pool));
      file[i]->size = finfo[i].size;
      length[i] = finfo[i].size > CHUNK_SIZE ? CHUNK_SIZE : finfo[i].size;
      file[i]->buffer = apr_palloc(file_baton->pool, (apr_size_t) length[i]);
      SVN_ERR(read_chunk(file[i]->file, file[i]->path, file[i]->buffer,
                         length[i], 0, file_baton->pool));
      file[i]->endp = file[i]->buffer + length[i];
      file[i]->curp = file[i]->buffer;
    }

  for (i = 0; i < datasource_len; i++)
    if (length[i] == 0)
      /* There will not be any identical prefix/suffix, so we're done. */
      return SVN_NO_ERROR;

  SVN_ERR(find_identical_prefix(&reached_one_eof, prefix_lines,
                                file, datasource_len, file_baton->pool));

  if (reached_one_eof)
    /* At least one file consisted totally of identical prefix, 
     * so there will be no identical suffix. We're done. */
    return SVN_NO_ERROR;

  SVN_ERR(find_identical_suffix(file, datasource_len, file_baton->pool));

  return SVN_NO_ERROR;
}


/* Implements svn_diff_fns_t::datasource_close */
static svn_error_t *
datasource_close(void *baton, svn_diff_datasource_e datasource)
{
  /* Do nothing.  The compare_token function needs previous datasources
   * to stay available until all datasources are processed.
   */

  return SVN_NO_ERROR;
}

/* Implements svn_diff_fns_t::datasource_get_next_token */
static svn_error_t *
datasource_get_next_token(apr_uint32_t *hash, void **token, void *baton,
                          svn_diff_datasource_e datasource)
{
  svn_diff__file_baton_t *file_baton = baton;
  svn_diff__file_token_t *file_token;
  struct file_info *file = &file_baton->files[datasource_to_index(datasource)];
  char *endp;
  char *curp;
  char *eol;
  apr_off_t last_chunk;
  apr_off_t length;
  apr_uint32_t h = 0;
  /* Did the last chunk end in a CR character? */
  svn_boolean_t had_cr = FALSE;

  *token = NULL;

  curp = file->curp;
  endp = file->endp;

  last_chunk = offset_to_chunk(file->size);

  if (curp == endp
      && last_chunk == file->chunk)
    {
      return SVN_NO_ERROR;
    }

  /* If identical suffix is defined, stop when we encounter it */
  if (file->suffix_start_chunk || file->suffix_offset_in_chunk)
    if (file->chunk == file->suffix_start_chunk
        && (curp - file->buffer) == file->suffix_offset_in_chunk)
      return SVN_NO_ERROR;

  /* Get a new token */
  file_token = file_baton->tokens;
  if (file_token)
    {
      file_baton->tokens = file_token->next;
    }
  else
    {
      file_token = apr_palloc(file_baton->pool, sizeof(*file_token));
    }

  file_token->datasource = datasource;
  file_token->offset = chunk_to_offset(file->chunk)
                       + (curp - file->buffer);
  file_token->raw_length = 0;
  file_token->length = 0;

  while (1)
    {
      eol = svn_eol__find_eol_start(curp, endp - curp);
      if (eol)
        {
          had_cr = (*eol == '\r');
          eol++;
          /* If we have the whole eol sequence in the chunk... */
          if (!(had_cr && eol == endp))
            {
              /* Also skip past the '\n' in an '\r\n' sequence. */
              if (had_cr && *eol == '\n')
                eol++;
              break;
            }
        }

      if (file->chunk == last_chunk)
        {
          eol = endp;
          break;
        }

      length = endp - curp;
      file_token->raw_length += length;
      svn_diff__normalize_buffer(&curp, &length,
                                 &file->normalize_state,
                                 curp, file_baton->options);
      file_token->length += length;
      h = svn_diff__adler32(h, curp, length);

      curp = endp = file->buffer;
      file->chunk++;
      length = file->chunk == last_chunk ?
        offset_in_chunk(file->size) : CHUNK_SIZE;
      endp += length;
      file->endp = endp;

      SVN_ERR(read_chunk(file->file, file->path,
                         curp, length,
                         chunk_to_offset(file->chunk),
                         file_baton->pool));

      /* If the last chunk ended in a CR, we're done. */
      if (had_cr)
        {
          eol = curp;
          if (*curp == '\n')
            ++eol;
          break;
        }
    }

  length = eol - curp;
  file_token->raw_length += length;
  file->curp = eol;

  /* If the file length is exactly a multiple of CHUNK_SIZE, we will end up
   * with a spurious empty token.  Avoid returning it.
   * Note that we use the unnormalized length; we don't want a line containing
   * only spaces (and no trailing newline) to appear like a non-existent
   * line. */
  if (file_token->raw_length > 0)
    {
      char *c = curp;
      svn_diff__normalize_buffer(&c, &length,
                                 &file->normalize_state,
                                 curp, file_baton->options);

      file_token->norm_offset = file_token->offset;
      if (file_token->length == 0)
        /* move past leading ignored characters */
        file_token->norm_offset += (c - curp);

      file_token->length += length;

      *hash = svn_diff__adler32(h, c, length);
      *token = file_token;
    }

  return SVN_NO_ERROR;
}

#define COMPARE_CHUNK_SIZE 4096

/* Implements svn_diff_fns_t::token_compare */
static svn_error_t *
token_compare(void *baton, void *token1, void *token2, int *compare)
{
  svn_diff__file_baton_t *file_baton = baton;
  svn_diff__file_token_t *file_token[2];
  char buffer[2][COMPARE_CHUNK_SIZE];
  char *bufp[2];
  apr_off_t offset[2];
  struct file_info *file[2];
  apr_off_t length[2];
  apr_off_t total_length;
  /* How much is left to read of each token from the file. */
  apr_off_t raw_length[2];
  int i;
  svn_diff__normalize_state_t state[2];

  file_token[0] = token1;
  file_token[1] = token2;
  if (file_token[0]->length < file_token[1]->length)
    {
      *compare = -1;
      return SVN_NO_ERROR;
    }

  if (file_token[0]->length > file_token[1]->length)
    {
      *compare = 1;
      return SVN_NO_ERROR;
    }

  total_length = file_token[0]->length;
  if (total_length == 0)
    {
      *compare = 0;
      return SVN_NO_ERROR;
    }

  for (i = 0; i < 2; ++i)
    {
      int idx = datasource_to_index(file_token[i]->datasource);

      file[i] = &file_baton->files[idx];
      offset[i] = file_token[i]->norm_offset;
      state[i] = svn_diff__normalize_state_normal;

      if (offset_to_chunk(offset[i]) == file[i]->chunk)
        {
          /* If the start of the token is in memory, the entire token is
           * in memory.
           */
          bufp[i] = file[i]->buffer;
          bufp[i] += offset_in_chunk(offset[i]);

          length[i] = total_length;
          raw_length[i] = 0;
        }
      else
        {
          length[i] = 0;
          raw_length[i] = file_token[i]->raw_length;
        }
    }

  do
    {
      apr_off_t len;
      for (i = 0; i < 2; i++)
        {
          if (length[i] == 0)
            {
              /* Error if raw_length is 0, that's an unexpected change
               * of the file that can happen when ingoring whitespace
               * and that can lead to an infinite loop. */
              if (raw_length[i] == 0)
                return svn_error_createf(SVN_ERR_DIFF_DATASOURCE_MODIFIED,
                                         NULL,
                                         _("The file '%s' changed unexpectedly"
                                           " during diff"),
                                         file[i]->path);

              /* Read a chunk from disk into a buffer */
              bufp[i] = buffer[i];
              length[i] = raw_length[i] > COMPARE_CHUNK_SIZE ?
                COMPARE_CHUNK_SIZE : raw_length[i];

              SVN_ERR(read_chunk(file[i]->file,
                                 file[i]->path,
                                 bufp[i], length[i], offset[i],
                                 file_baton->pool));
              offset[i] += length[i];
              raw_length[i] -= length[i];
              /* bufp[i] gets reset to buffer[i] before reading each chunk,
                 so, overwriting it isn't a problem */
              svn_diff__normalize_buffer(&bufp[i], &length[i], &state[i],
                                         bufp[i], file_baton->options);
            }
        }

      len = length[0] > length[1] ? length[1] : length[0];

      /* Compare two chunks (that could be entire tokens if they both reside
       * in memory).
       */
      *compare = memcmp(bufp[0], bufp[1], (size_t) len);
      if (*compare != 0)
        return SVN_NO_ERROR;

      total_length -= len;
      length[0] -= len;
      length[1] -= len;
      bufp[0] += len;
      bufp[1] += len;
    }
  while(total_length > 0);

  *compare = 0;
  return SVN_NO_ERROR;
}


/* Implements svn_diff_fns_t::token_discard */
static void
token_discard(void *baton, void *token)
{
  svn_diff__file_baton_t *file_baton = baton;
  svn_diff__file_token_t *file_token = token;

  file_token->next = file_baton->tokens;
  file_baton->tokens = file_token;
}


/* Implements svn_diff_fns_t::token_discard_all */
static void
token_discard_all(void *baton)
{
  svn_diff__file_baton_t *file_baton = baton;

  /* Discard all memory in use by the tokens, and close all open files. */
  svn_pool_clear(file_baton->pool);
}


static const svn_diff_fns_t svn_diff__file_vtable =
{
  datasource_open,
  datasources_open,
  datasource_close,
  datasource_get_next_token,
  token_compare,
  token_discard,
  token_discard_all
};

/* Id for the --ignore-eol-style option, which doesn't have a short name. */
#define SVN_DIFF__OPT_IGNORE_EOL_STYLE 256

/* Options supported by svn_diff_file_options_parse(). */
static const apr_getopt_option_t diff_options[] =
{
  { "ignore-space-change", 'b', 0, NULL },
  { "ignore-all-space", 'w', 0, NULL },
  { "ignore-eol-style", SVN_DIFF__OPT_IGNORE_EOL_STYLE, 0, NULL },
  { "show-c-function", 'p', 0, NULL },
  /* ### For compatibility; we don't support the argument to -u, because
   * ### we don't have optional argument support. */
  { "unified", 'u', 0, NULL },
  { NULL, 0, 0, NULL }
};

svn_diff_file_options_t *
svn_diff_file_options_create(apr_pool_t *pool)
{
  return apr_pcalloc(pool, sizeof(svn_diff_file_options_t));
}

/* A baton for use with opt_parsing_error_func(). */
struct opt_parsing_error_baton_t
{
  svn_error_t *err;
  apr_pool_t *pool;
};

/* Store an error message from apr_getopt_long().  Set BATON->err to a new
 * error with a message generated from FMT and the remaining arguments.
 * Implements apr_getopt_err_fn_t. */
static void
opt_parsing_error_func(void *baton,
                       const char *fmt, ...)
{
  struct opt_parsing_error_baton_t *b = baton;
  const char *message;
  va_list ap;

  va_start(ap, fmt);
  message = apr_pvsprintf(b->pool, fmt, ap);
  va_end(ap);

  /* Skip leading ": " (if present, which it always is in known cases). */
  if (strncmp(message, ": ", 2) == 0)
    message += 2;

  b->err = svn_error_create(SVN_ERR_INVALID_DIFF_OPTION, NULL, message);
}

svn_error_t *
svn_diff_file_options_parse(svn_diff_file_options_t *options,
                            const apr_array_header_t *args,
                            apr_pool_t *pool)
{
  apr_getopt_t *os;
  struct opt_parsing_error_baton_t opt_parsing_error_baton = { NULL, pool };
  /* Make room for each option (starting at index 1) plus trailing NULL. */
  const char **argv = apr_palloc(pool, sizeof(char*) * (args->nelts + 2));

  argv[0] = "";
  memcpy((void *) (argv + 1), args->elts, sizeof(char*) * args->nelts);
  argv[args->nelts + 1] = NULL;

  apr_getopt_init(&os, pool, args->nelts + 1, argv);

  /* Capture any error message from apr_getopt_long().  This will typically
   * say which option is wrong, which we would not otherwise know. */
  os->errfn = opt_parsing_error_func;
  os->errarg = &opt_parsing_error_baton;

  while (1)
    {
      const char *opt_arg;
      int opt_id;
      apr_status_t err = apr_getopt_long(os, diff_options, &opt_id, &opt_arg);

      if (APR_STATUS_IS_EOF(err))
        break;
      if (err)
        /* Wrap apr_getopt_long()'s error message.  Its doc string implies
         * it always will produce one, but never mind if it doesn't.  Avoid
         * using the message associated with the return code ERR, because
         * it refers to the "command line" which may be misleading here. */
        return svn_error_create(SVN_ERR_INVALID_DIFF_OPTION,
                                opt_parsing_error_baton.err,
                                _("Error in options to internal diff"));

      switch (opt_id)
        {
        case 'b':
          /* -w takes precedence over -b. */
          if (! options->ignore_space)
            options->ignore_space = svn_diff_file_ignore_space_change;
          break;
        case 'w':
          options->ignore_space = svn_diff_file_ignore_space_all;
          break;
        case SVN_DIFF__OPT_IGNORE_EOL_STYLE:
          options->ignore_eol_style = TRUE;
          break;
        case 'p':
          options->show_c_function = TRUE;
          break;
        default:
          break;
        }
    }

  /* Check for spurious arguments. */
  if (os->ind < os->argc)
    return svn_error_createf(SVN_ERR_INVALID_DIFF_OPTION, NULL,
                             _("Invalid argument '%s' in diff options"),
                             os->argv[os->ind]);

  return SVN_NO_ERROR;
}

svn_error_t *
svn_diff_file_diff_2(svn_diff_t **diff,
                     const char *original,
                     const char *modified,
                     const svn_diff_file_options_t *options,
                     apr_pool_t *pool)
{
  svn_diff__file_baton_t baton;

  memset(&baton, 0, sizeof(baton));
  baton.options = options;
  baton.files[0].path = original;
  baton.files[1].path = modified;
  baton.pool = svn_pool_create(pool);

  SVN_ERR(svn_diff_diff(diff, &baton, &svn_diff__file_vtable, pool));

  svn_pool_destroy(baton.pool);
  return SVN_NO_ERROR;
}

svn_error_t *
svn_diff_file_diff3_2(svn_diff_t **diff,
                      const char *original,
                      const char *modified,
                      const char *latest,
                      const svn_diff_file_options_t *options,
                      apr_pool_t *pool)
{
  svn_diff__file_baton_t baton;

  memset(&baton, 0, sizeof(baton));
  baton.options = options;
  baton.files[0].path = original;
  baton.files[1].path = modified;
  baton.files[2].path = latest;
  baton.pool = svn_pool_create(pool);

  SVN_ERR(svn_diff_diff3(diff, &baton, &svn_diff__file_vtable, pool));

  svn_pool_destroy(baton.pool);
  return SVN_NO_ERROR;
}

svn_error_t *
svn_diff_file_diff4_2(svn_diff_t **diff,
                      const char *original,
                      const char *modified,
                      const char *latest,
                      const char *ancestor,
                      const svn_diff_file_options_t *options,
                      apr_pool_t *pool)
{
  svn_diff__file_baton_t baton;

  memset(&baton, 0, sizeof(baton));
  baton.options = options;
  baton.files[0].path = original;
  baton.files[1].path = modified;
  baton.files[2].path = latest;
  baton.files[3].path = ancestor;
  baton.pool = svn_pool_create(pool);

  SVN_ERR(svn_diff_diff4(diff, &baton, &svn_diff__file_vtable, pool));

  svn_pool_destroy(baton.pool);
  return SVN_NO_ERROR;
}


/** Display unified context diffs **/

/* Maximum length of the extra context to show when show_c_function is set.
 * GNU diff uses 40, let's be brave and use 50 instead. */
#define SVN_DIFF__EXTRA_CONTEXT_LENGTH 50
typedef struct svn_diff__file_output_baton_t
{
  svn_stream_t *output_stream;
  const char *header_encoding;

  /* Cached markers, in header_encoding. */
  const char *context_str;
  const char *delete_str;
  const char *insert_str;

  const char *path[2];
  apr_file_t *file[2];

  apr_off_t   current_line[2];

  char        buffer[2][4096];
  apr_size_t  length[2];
  char       *curp[2];

  apr_off_t   hunk_start[2];
  apr_off_t   hunk_length[2];
  svn_stringbuf_t *hunk;

  /* Should we emit C functions in the unified diff header */
  svn_boolean_t show_c_function;
  /* Extra strings to skip over if we match. */
  apr_array_header_t *extra_skip_match;
  /* "Context" to append to the @@ line when the show_c_function option
   * is set. */
  svn_stringbuf_t *extra_context;
  /* Extra context for the current hunk. */
  char hunk_extra_context[SVN_DIFF__EXTRA_CONTEXT_LENGTH + 1];

  apr_pool_t *pool;
} svn_diff__file_output_baton_t;

typedef enum svn_diff__file_output_unified_type_e
{
  svn_diff__file_output_unified_skip,
  svn_diff__file_output_unified_context,
  svn_diff__file_output_unified_delete,
  svn_diff__file_output_unified_insert
} svn_diff__file_output_unified_type_e;


static svn_error_t *
output_unified_line(svn_diff__file_output_baton_t *baton,
                    svn_diff__file_output_unified_type_e type, int idx)
{
  char *curp;
  char *eol;
  apr_size_t length;
  svn_error_t *err;
  svn_boolean_t bytes_processed = FALSE;
  svn_boolean_t had_cr = FALSE;
  /* Are we collecting extra context? */
  svn_boolean_t collect_extra = FALSE;

  length = baton->length[idx];
  curp = baton->curp[idx];

  /* Lazily update the current line even if we're at EOF.
   * This way we fake output of context at EOF
   */
  baton->current_line[idx]++;

  if (length == 0 && apr_file_eof(baton->file[idx]))
    {
      return SVN_NO_ERROR;
    }

  do
    {
      if (length > 0)
        {
          if (!bytes_processed)
            {
              switch (type)
                {
                case svn_diff__file_output_unified_context:
                  svn_stringbuf_appendcstr(baton->hunk, baton->context_str);
                  baton->hunk_length[0]++;
                  baton->hunk_length[1]++;
                  break;
                case svn_diff__file_output_unified_delete:
                  svn_stringbuf_appendcstr(baton->hunk, baton->delete_str);
                  baton->hunk_length[0]++;
                  break;
                case svn_diff__file_output_unified_insert:
                  svn_stringbuf_appendcstr(baton->hunk, baton->insert_str);
                  baton->hunk_length[1]++;
                  break;
                default:
                  break;
                }

              if (baton->show_c_function
                  && (type == svn_diff__file_output_unified_skip
                      || type == svn_diff__file_output_unified_context)
                  && (svn_ctype_isalpha(*curp) || *curp == '$' || *curp == '_')
                  && !svn_cstring_match_glob_list(curp,
                                                  baton->extra_skip_match))
                {
                  svn_stringbuf_setempty(baton->extra_context);
                  collect_extra = TRUE;
                }
            }

          eol = svn_eol__find_eol_start(curp, length);

          if (eol != NULL)
            {
              apr_size_t len;

              had_cr = (*eol == '\r');
              eol++;
              len = (apr_size_t)(eol - curp);

              if (! had_cr || len < length)
                {
                  if (had_cr && *eol == '\n')
                    {
                      ++eol;
                      ++len;
                    }

                  length -= len;

                  if (type != svn_diff__file_output_unified_skip)
                    {
                      svn_stringbuf_appendbytes(baton->hunk, curp, len);
                    }
                  if (collect_extra)
                    {
                      svn_stringbuf_appendbytes(baton->extra_context,
                                                curp, len);
                    }

                  baton->curp[idx] = eol;
                  baton->length[idx] = length;

                  err = SVN_NO_ERROR;

                  break;
                }
            }

          if (type != svn_diff__file_output_unified_skip)
            {
              svn_stringbuf_appendbytes(baton->hunk, curp, length);
            }

          if (collect_extra)
            {
              svn_stringbuf_appendbytes(baton->extra_context, curp, length);
            }

          bytes_processed = TRUE;
        }

      curp = baton->buffer[idx];
      length = sizeof(baton->buffer[idx]);

      err = svn_io_file_read(baton->file[idx], curp, &length, baton->pool);

      /* If the last chunk ended with a CR, we look for an LF at the start
         of this chunk. */
      if (had_cr)
        {
          if (! err && length > 0 && *curp == '\n')
            {
              if (type != svn_diff__file_output_unified_skip)
                {
                  svn_stringbuf_appendbyte(baton->hunk, *curp);
                }
              /* We don't append the LF to extra_context, since it would
               * just be stripped anyway. */
              ++curp;
              --length;
            }

          baton->curp[idx] = curp;
          baton->length[idx] = length;

          break;
        }
    }
  while (! err);

  if (err && ! APR_STATUS_IS_EOF(err->apr_err))
    return err;

  if (err && APR_STATUS_IS_EOF(err->apr_err))
    {
      svn_error_clear(err);
      /* Special case if we reach the end of file AND the last line is in the
         changed range AND the file doesn't end with a newline */
      if (bytes_processed && (type != svn_diff__file_output_unified_skip)
          && ! had_cr)
        {
          const char *out_str;
          SVN_ERR(svn_utf_cstring_from_utf8_ex2
                  (&out_str,
                   apr_psprintf(baton->pool,
                                APR_EOL_STR "\\ %s" APR_EOL_STR,
                                _("No newline at end of file")),
                   baton->header_encoding, baton->pool));
          svn_stringbuf_appendcstr(baton->hunk, out_str);
        }

      baton->length[idx] = 0;
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
output_unified_flush_hunk(svn_diff__file_output_baton_t *baton)
{
  apr_off_t target_line;
  apr_size_t hunk_len;
  int i;

  if (svn_stringbuf_isempty(baton->hunk))
    {
      /* Nothing to flush */
      return SVN_NO_ERROR;
    }

  target_line = baton->hunk_start[0] + baton->hunk_length[0]
                + SVN_DIFF__UNIFIED_CONTEXT_SIZE;

  /* Add trailing context to the hunk */
  while (baton->current_line[0] < target_line)
    {
      SVN_ERR(output_unified_line
              (baton, svn_diff__file_output_unified_context, 0));
    }

  /* If the file is non-empty, convert the line indexes from
     zero based to one based */
  for (i = 0; i < 2; i++)
    {
      if (baton->hunk_length[i] > 0)
        baton->hunk_start[i]++;
    }

  /* Output the hunk header.  If the hunk length is 1, the file is a one line
     file.  In this case, surpress the number of lines in the hunk (it is
     1 implicitly)
   */
  SVN_ERR(svn_stream_printf_from_utf8(baton->output_stream,
                                      baton->header_encoding,
                                      baton->pool,
                                      "@@ -%" APR_OFF_T_FMT,
                                      baton->hunk_start[0]));
  if (baton->hunk_length[0] != 1)
    {
      SVN_ERR(svn_stream_printf_from_utf8(baton->output_stream,
                                          baton->header_encoding,
                                          baton->pool, ",%" APR_OFF_T_FMT,
                                          baton->hunk_length[0]));
    }

  SVN_ERR(svn_stream_printf_from_utf8(baton->output_stream,
                                      baton->header_encoding,
                                      baton->pool, " +%" APR_OFF_T_FMT,
                                      baton->hunk_start[1]));
  if (baton->hunk_length[1] != 1)
    {
      SVN_ERR(svn_stream_printf_from_utf8(baton->output_stream,
                                          baton->header_encoding,
                                          baton->pool, ",%" APR_OFF_T_FMT,
                                          baton->hunk_length[1]));
    }

  SVN_ERR(svn_stream_printf_from_utf8(baton->output_stream,
                                      baton->header_encoding,
                                      baton->pool, " @@%s%s" APR_EOL_STR,
                                      baton->hunk_extra_context[0]
                                      ? " " : "",
                                      baton->hunk_extra_context));

  /* Output the hunk content */
  hunk_len = baton->hunk->len;
  SVN_ERR(svn_stream_write(baton->output_stream, baton->hunk->data,
                           &hunk_len));

  /* Prepare for the next hunk */
  baton->hunk_length[0] = 0;
  baton->hunk_length[1] = 0;
  svn_stringbuf_setempty(baton->hunk);

  return SVN_NO_ERROR;
}

static svn_error_t *
output_unified_diff_modified(void *baton,
  apr_off_t original_start, apr_off_t original_length,
  apr_off_t modified_start, apr_off_t modified_length,
  apr_off_t latest_start, apr_off_t latest_length)
{
  svn_diff__file_output_baton_t *output_baton = baton;
  apr_off_t target_line[2];
  int i;

  target_line[0] = original_start >= SVN_DIFF__UNIFIED_CONTEXT_SIZE
                   ? original_start - SVN_DIFF__UNIFIED_CONTEXT_SIZE : 0;
  target_line[1] = modified_start;

  /* If the changed ranges are far enough apart (no overlapping or connecting
     context), flush the current hunk, initialize the next hunk and skip the
     lines not in context.  Also do this when this is the first hunk.
   */
  if (output_baton->current_line[0] < target_line[0]
      && (output_baton->hunk_start[0] + output_baton->hunk_length[0]
          + SVN_DIFF__UNIFIED_CONTEXT_SIZE < target_line[0]
          || output_baton->hunk_length[0] == 0))
    {
      SVN_ERR(output_unified_flush_hunk(output_baton));

      output_baton->hunk_start[0] = target_line[0];
      output_baton->hunk_start[1] = target_line[1] + target_line[0]
                                    - original_start;

      /* Skip lines until we are at the beginning of the context we want to
         display */
      while (output_baton->current_line[0] < target_line[0])
        {
          SVN_ERR(output_unified_line(output_baton,
                                      svn_diff__file_output_unified_skip, 0));
        }

      if (output_baton->show_c_function)
        {
          int p;
          const char *invalid_character;

          /* Save the extra context for later use.
           * Note that the last byte of the hunk_extra_context array is never
           * touched after it is zero-initialized, so the array is always
           * 0-terminated. */
          strncpy(output_baton->hunk_extra_context,
                  output_baton->extra_context->data,
                  SVN_DIFF__EXTRA_CONTEXT_LENGTH);
          /* Trim whitespace at the end, most notably to get rid of any
           * newline characters. */
          p = strlen(output_baton->hunk_extra_context);
          while (p > 0
                 && svn_ctype_isspace(output_baton->hunk_extra_context[p - 1]))
            {
              output_baton->hunk_extra_context[--p] = '\0';
            }
          invalid_character =
            svn_utf__last_valid(output_baton->hunk_extra_context,
                                SVN_DIFF__EXTRA_CONTEXT_LENGTH);
          for (p = invalid_character - output_baton->hunk_extra_context;
               p < SVN_DIFF__EXTRA_CONTEXT_LENGTH; p++)
            {
              output_baton->hunk_extra_context[p] = '\0';
            }
        }
    }

  /* Skip lines until we are at the start of the changed range */
  while (output_baton->current_line[1] < target_line[1])
    {
      SVN_ERR(output_unified_line(output_baton,
                                  svn_diff__file_output_unified_skip, 1));
    }

  /* Output the context preceding the changed range */
  while (output_baton->current_line[0] < original_start)
    {
      SVN_ERR(output_unified_line(output_baton,
                                  svn_diff__file_output_unified_context, 0));
    }

  target_line[0] = original_start + original_length;
  target_line[1] = modified_start + modified_length;

  /* Output the changed range */
  for (i = 0; i < 2; i++)
    {
      while (output_baton->current_line[i] < target_line[i])
        {
          SVN_ERR(output_unified_line
                          (output_baton,
                           i == 0 ? svn_diff__file_output_unified_delete
                                  : svn_diff__file_output_unified_insert, i));
        }
    }

  return SVN_NO_ERROR;
}

/* Set *HEADER to a new string consisting of PATH, a tab, and PATH's mtime. */
static svn_error_t *
output_unified_default_hdr(const char **header, const char *path,
                           apr_pool_t *pool)
{
  apr_finfo_t file_info;
  apr_time_exp_t exploded_time;
  char time_buffer[64];
  apr_size_t time_len;
  const char *utf8_timestr;

  SVN_ERR(svn_io_stat(&file_info, path, APR_FINFO_MTIME, pool));
  apr_time_exp_lt(&exploded_time, file_info.mtime);

  apr_strftime(time_buffer, &time_len, sizeof(time_buffer) - 1,
  /* Order of date components can be different in different languages */
               _("%a %b %e %H:%M:%S %Y"), &exploded_time);

  SVN_ERR(svn_utf_cstring_to_utf8(&utf8_timestr, time_buffer, pool));

  *header = apr_psprintf(pool, "%s\t%s", path, utf8_timestr);

  return SVN_NO_ERROR;
}

static const svn_diff_output_fns_t svn_diff__file_output_unified_vtable =
{
  NULL, /* output_common */
  output_unified_diff_modified,
  NULL, /* output_diff_latest */
  NULL, /* output_diff_common */
  NULL  /* output_conflict */
};

svn_error_t *
svn_diff_file_output_unified3(svn_stream_t *output_stream,
                              svn_diff_t *diff,
                              const char *original_path,
                              const char *modified_path,
                              const char *original_header,
                              const char *modified_header,
                              const char *header_encoding,
                              const char *relative_to_dir,
                              svn_boolean_t show_c_function,
                              apr_pool_t *pool)
{
  svn_diff__file_output_baton_t baton;

  if (svn_diff_contains_diffs(diff))
    {
      const char **c;
      int i;

      memset(&baton, 0, sizeof(baton));
      baton.output_stream = output_stream;
      baton.pool = pool;
      baton.header_encoding = header_encoding;
      baton.path[0] = original_path;
      baton.path[1] = modified_path;
      baton.hunk = svn_stringbuf_create("", pool);
      baton.show_c_function = show_c_function;
      baton.extra_context = svn_stringbuf_create("", pool);
      baton.extra_skip_match = apr_array_make(pool, 3, sizeof(char **));

      c = apr_array_push(baton.extra_skip_match);
      *c = "public:*";
      c = apr_array_push(baton.extra_skip_match);
      *c = "private:*";
      c = apr_array_push(baton.extra_skip_match);
      *c = "protected:*";

      SVN_ERR(svn_utf_cstring_from_utf8_ex2(&baton.context_str, " ",
                                            header_encoding, pool));
      SVN_ERR(svn_utf_cstring_from_utf8_ex2(&baton.delete_str, "-",
                                            header_encoding, pool));
      SVN_ERR(svn_utf_cstring_from_utf8_ex2(&baton.insert_str, "+",
                                            header_encoding, pool));

      if (relative_to_dir)
        {
          /* Possibly adjust the "original" and "modified" paths shown in
             the output (see issue #2723). */
          const char *child_path;

          if (! original_header)
            {
              child_path = svn_dirent_is_child(relative_to_dir,
                                               original_path, pool);
              if (child_path)
                original_path = child_path;
              else
                return svn_error_createf(
                                   SVN_ERR_BAD_RELATIVE_PATH, NULL,
                                   _("Path '%s' must be an immediate child of "
                                     "the directory '%s'"),
                                   svn_dirent_local_style(original_path, pool),
                                   svn_dirent_local_style(relative_to_dir,
                                                          pool));
            }

          if (! modified_header)
            {
              child_path = svn_dirent_is_child(relative_to_dir,
                                               modified_path, pool);
              if (child_path)
                modified_path = child_path;
              else
                return svn_error_createf(
                                   SVN_ERR_BAD_RELATIVE_PATH, NULL,
                                   _("Path '%s' must be an immediate child of "
                                     "the directory '%s'"),
                                   svn_dirent_local_style(modified_path, pool),
                                   svn_dirent_local_style(relative_to_dir,
                                                          pool));
            }
        }

      for (i = 0; i < 2; i++)
        {
          SVN_ERR(svn_io_file_open(&baton.file[i], baton.path[i],
                                   APR_READ, APR_OS_DEFAULT, pool));
        }

      if (original_header == NULL)
        {
          SVN_ERR(output_unified_default_hdr
                  (&original_header, original_path, pool));
        }

      if (modified_header == NULL)
        {
          SVN_ERR(output_unified_default_hdr
                  (&modified_header, modified_path, pool));
        }

      SVN_ERR(svn_stream_printf_from_utf8(output_stream, header_encoding, pool,
                                          "--- %s" APR_EOL_STR
                                          "+++ %s" APR_EOL_STR,
                                          original_header, modified_header));

      SVN_ERR(svn_diff_output(diff, &baton,
                              &svn_diff__file_output_unified_vtable));
      SVN_ERR(output_unified_flush_hunk(&baton));

      for (i = 0; i < 2; i++)
        {
          SVN_ERR(svn_io_file_close(baton.file[i], pool));
        }
    }

  return SVN_NO_ERROR;
}


/** Display diff3 **/

/* A stream to remember *leading* context.  Note that this stream does
   *not* copy the data that it is remembering; it just saves
   *pointers! */
typedef struct {
  svn_stream_t *stream;
  const char *data[SVN_DIFF__UNIFIED_CONTEXT_SIZE];
  apr_size_t len[SVN_DIFF__UNIFIED_CONTEXT_SIZE];
  apr_size_t next_slot;
  apr_size_t total_written;
} context_saver_t;


static svn_error_t *
context_saver_stream_write(void *baton,
                           const char *data,
                           apr_size_t *len)
{
  context_saver_t *cs = baton;
  cs->data[cs->next_slot] = data;
  cs->len[cs->next_slot] = *len;
  cs->next_slot = (cs->next_slot + 1) % SVN_DIFF__UNIFIED_CONTEXT_SIZE;
  cs->total_written++;
  return SVN_NO_ERROR;
}

typedef struct svn_diff3__file_output_baton_t
{
  svn_stream_t *output_stream;

  const char *path[3];

  apr_off_t   current_line[3];

  char       *buffer[3];
  char       *endp[3];
  char       *curp[3];

  /* The following four members are in the encoding used for the output. */
  const char *conflict_modified;
  const char *conflict_original;
  const char *conflict_separator;
  const char *conflict_latest;

  const char *marker_eol;

  svn_diff_conflict_display_style_t conflict_style;

  /* The rest of the fields are for
     svn_diff_conflict_display_only_conflicts only.  Note that for
     these batons, OUTPUT_STREAM is either CONTEXT_SAVER->STREAM or
     (soon after a conflict) a "trailing context stream", never the
     actual output stream.*/
  /* The actual output stream. */
  svn_stream_t *real_output_stream;
  context_saver_t *context_saver;
  /* Used to allocate context_saver and trailing context streams, and
     for some printfs. */
  apr_pool_t *pool;
} svn_diff3__file_output_baton_t;

static svn_error_t *
flush_context_saver(context_saver_t *cs,
                    svn_stream_t *output_stream)
{
  int i;
  for (i = 0; i < SVN_DIFF__UNIFIED_CONTEXT_SIZE; i++)
    {
      int slot = (i + cs->next_slot) % SVN_DIFF__UNIFIED_CONTEXT_SIZE;
      if (cs->data[slot])
        {
          apr_size_t len = cs->len[slot];
          SVN_ERR(svn_stream_write(output_stream, cs->data[slot], &len));
        }
    }
  return SVN_NO_ERROR;
}

static void
make_context_saver(svn_diff3__file_output_baton_t *fob)
{
  context_saver_t *cs;

  svn_pool_clear(fob->pool);
  cs = apr_pcalloc(fob->pool, sizeof(*cs));
  cs->stream = svn_stream_empty(fob->pool);
  svn_stream_set_baton(cs->stream, cs);
  svn_stream_set_write(cs->stream, context_saver_stream_write);
  fob->context_saver = cs;
  fob->output_stream = cs->stream;
}


/* A stream which prints SVN_DIFF__UNIFIED_CONTEXT_SIZE lines to
   BATON->REAL_OUTPUT_STREAM, and then changes BATON->OUTPUT_STREAM to
   a context_saver; used for *trailing* context. */

struct trailing_context_printer {
  apr_size_t lines_to_print;
  svn_diff3__file_output_baton_t *fob;
};



static svn_error_t *
trailing_context_printer_write(void *baton,
                               const char *data,
                               apr_size_t *len)
{
  struct trailing_context_printer *tcp = baton;
  SVN_ERR_ASSERT(tcp->lines_to_print > 0);
  SVN_ERR(svn_stream_write(tcp->fob->real_output_stream, data, len));
  tcp->lines_to_print--;
  if (tcp->lines_to_print == 0)
    make_context_saver(tcp->fob);
  return SVN_NO_ERROR;
}


static void
make_trailing_context_printer(svn_diff3__file_output_baton_t *btn)
{
  struct trailing_context_printer *tcp;
  svn_stream_t *s;

  svn_pool_clear(btn->pool);

  tcp = apr_pcalloc(btn->pool, sizeof(*tcp));
  tcp->lines_to_print = SVN_DIFF__UNIFIED_CONTEXT_SIZE;
  tcp->fob = btn;
  s = svn_stream_empty(btn->pool);
  svn_stream_set_baton(s, tcp);
  svn_stream_set_write(s, trailing_context_printer_write);
  btn->output_stream = s;
}



typedef enum svn_diff3__file_output_type_e
{
  svn_diff3__file_output_skip,
  svn_diff3__file_output_normal
} svn_diff3__file_output_type_e;


static svn_error_t *
output_line(svn_diff3__file_output_baton_t *baton,
            svn_diff3__file_output_type_e type, int idx)
{
  char *curp;
  char *endp;
  char *eol;
  apr_size_t len;

  curp = baton->curp[idx];
  endp = baton->endp[idx];

  /* Lazily update the current line even if we're at EOF.
   */
  baton->current_line[idx]++;

  if (curp == endp)
    return SVN_NO_ERROR;

  eol = svn_eol__find_eol_start(curp, endp - curp);
  if (!eol)
    eol = endp;
  else
    {
      svn_boolean_t had_cr = (*eol == '\r');
      eol++;
      if (had_cr && eol != endp && *eol == '\n')
        eol++;
    }

  if (type != svn_diff3__file_output_skip)
    {
      len = eol - curp;
      /* Note that the trailing context printer assumes that
         svn_stream_write is called exactly once per line. */
      SVN_ERR(svn_stream_write(baton->output_stream, curp, &len));
    }

  baton->curp[idx] = eol;

  return SVN_NO_ERROR;
}

static svn_error_t *
output_marker_eol(svn_diff3__file_output_baton_t *btn)
{
  apr_size_t len = strlen(btn->marker_eol);
  return svn_stream_write(btn->output_stream, btn->marker_eol, &len);
}

static svn_error_t *
output_hunk(void *baton, int idx, apr_off_t target_line,
            apr_off_t target_length)
{
  svn_diff3__file_output_baton_t *output_baton = baton;

  /* Skip lines until we are at the start of the changed range */
  while (output_baton->current_line[idx] < target_line)
    {
      SVN_ERR(output_line(output_baton, svn_diff3__file_output_skip, idx));
    }

  target_line += target_length;

  while (output_baton->current_line[idx] < target_line)
    {
      SVN_ERR(output_line(output_baton, svn_diff3__file_output_normal, idx));
    }

  return SVN_NO_ERROR;
}

static svn_error_t *
output_common(void *baton, apr_off_t original_start, apr_off_t original_length,
              apr_off_t modified_start, apr_off_t modified_length,
              apr_off_t latest_start, apr_off_t latest_length)
{
  return output_hunk(baton, 1, modified_start, modified_length);
}

static svn_error_t *
output_diff_modified(void *baton,
                     apr_off_t original_start, apr_off_t original_length,
                     apr_off_t modified_start, apr_off_t modified_length,
                     apr_off_t latest_start, apr_off_t latest_length)
{
  return output_hunk(baton, 1, modified_start, modified_length);
}

static svn_error_t *
output_diff_latest(void *baton,
                   apr_off_t original_start, apr_off_t original_length,
                   apr_off_t modified_start, apr_off_t modified_length,
                   apr_off_t latest_start, apr_off_t latest_length)
{
  return output_hunk(baton, 2, latest_start, latest_length);
}

static svn_error_t *
output_conflict(void *baton,
                apr_off_t original_start, apr_off_t original_length,
                apr_off_t modified_start, apr_off_t modified_length,
                apr_off_t latest_start, apr_off_t latest_length,
                svn_diff_t *diff);

static const svn_diff_output_fns_t svn_diff3__file_output_vtable =
{
  output_common,
  output_diff_modified,
  output_diff_latest,
  output_diff_modified, /* output_diff_common */
  output_conflict
};



static svn_error_t *
output_conflict_with_context(svn_diff3__file_output_baton_t *btn,
                             apr_off_t original_start,
                             apr_off_t original_length,
                             apr_off_t modified_start,
                             apr_off_t modified_length,
                             apr_off_t latest_start,
                             apr_off_t latest_length)
{
  /* Are we currently saving starting context (as opposed to printing
     trailing context)?  If so, flush it. */
  if (btn->output_stream == btn->context_saver->stream)
    {
      if (btn->context_saver->total_written > SVN_DIFF__UNIFIED_CONTEXT_SIZE)
        SVN_ERR(svn_stream_printf(btn->real_output_stream, btn->pool, "@@\n"));
      SVN_ERR(flush_context_saver(btn->context_saver, btn->real_output_stream));
    }

  /* Print to the real output stream. */
  btn->output_stream = btn->real_output_stream;

  /* Output the conflict itself. */
  SVN_ERR(svn_stream_printf(btn->output_stream, btn->pool,
                            (modified_length == 1
                             ? "%s (%" APR_OFF_T_FMT ")"
                             : "%s (%" APR_OFF_T_FMT ",%" APR_OFF_T_FMT ")"),
                            btn->conflict_modified,
                            modified_start + 1, modified_length));
  SVN_ERR(output_marker_eol(btn));
  SVN_ERR(output_hunk(btn, 1/*modified*/, modified_start, modified_length));

  SVN_ERR(svn_stream_printf(btn->output_stream, btn->pool,
                            (original_length == 1
                             ? "%s (%" APR_OFF_T_FMT ")"
                             : "%s (%" APR_OFF_T_FMT ",%" APR_OFF_T_FMT ")"),
                            btn->conflict_original,
                            original_start + 1, original_length));
  SVN_ERR(output_marker_eol(btn));
  SVN_ERR(output_hunk(btn, 0/*original*/, original_start, original_length));

  SVN_ERR(svn_stream_printf(btn->output_stream, btn->pool,
                            "%s%s", btn->conflict_separator, btn->marker_eol));
  SVN_ERR(output_hunk(btn, 2/*latest*/, latest_start, latest_length));
  SVN_ERR(svn_stream_printf(btn->output_stream, btn->pool,
                            (latest_length == 1
                             ? "%s (%" APR_OFF_T_FMT ")"
                             : "%s (%" APR_OFF_T_FMT ",%" APR_OFF_T_FMT ")"),
                            btn->conflict_latest,
                            latest_start + 1, latest_length));
  SVN_ERR(output_marker_eol(btn));

  /* Go into print-trailing-context mode instead. */
  make_trailing_context_printer(btn);

  return SVN_NO_ERROR;
}


static svn_error_t *
output_conflict(void *baton,
                apr_off_t original_start, apr_off_t original_length,
                apr_off_t modified_start, apr_off_t modified_length,
                apr_off_t latest_start, apr_off_t latest_length,
                svn_diff_t *diff)
{
  svn_diff3__file_output_baton_t *file_baton = baton;
  apr_size_t len;

  svn_diff_conflict_display_style_t style = file_baton->conflict_style;

  if (style == svn_diff_conflict_display_only_conflicts)
    return output_conflict_with_context(file_baton,
                                        original_start, original_length,
                                        modified_start, modified_length,
                                        latest_start, latest_length);

  if (style == svn_diff_conflict_display_resolved_modified_latest)
    {
      if (diff)
        return svn_diff_output(diff, baton,
                               &svn_diff3__file_output_vtable);
      else
        style = svn_diff_conflict_display_modified_latest;
    }

  if (style == svn_diff_conflict_display_modified_latest ||
      style == svn_diff_conflict_display_modified_original_latest)
    {
      len = strlen(file_baton->conflict_modified);
      SVN_ERR(svn_stream_write(file_baton->output_stream,
                               file_baton->conflict_modified,
                               &len));
      SVN_ERR(output_marker_eol(file_baton));

      SVN_ERR(output_hunk(baton, 1, modified_start, modified_length));

      if (style == svn_diff_conflict_display_modified_original_latest)
        {
          len = strlen(file_baton->conflict_original);
          SVN_ERR(svn_stream_write(file_baton->output_stream,
                                   file_baton->conflict_original, &len));
          SVN_ERR(output_marker_eol(file_baton));
          SVN_ERR(output_hunk(baton, 0, original_start, original_length));
        }

      len = strlen(file_baton->conflict_separator);
      SVN_ERR(svn_stream_write(file_baton->output_stream,
                               file_baton->conflict_separator, &len));
      SVN_ERR(output_marker_eol(file_baton));

      SVN_ERR(output_hunk(baton, 2, latest_start, latest_length));

      len = strlen(file_baton->conflict_latest);
      SVN_ERR(svn_stream_write(file_baton->output_stream,
                               file_baton->conflict_latest, &len));
      SVN_ERR(output_marker_eol(file_baton));
    }
  else if (style == svn_diff_conflict_display_modified)
    SVN_ERR(output_hunk(baton, 1, modified_start, modified_length));
  else if (style == svn_diff_conflict_display_latest)
    SVN_ERR(output_hunk(baton, 2, latest_start, latest_length));
  else /* unknown style */
    SVN_ERR_MALFUNCTION();

  return SVN_NO_ERROR;
}

svn_error_t *
svn_diff_file_output_merge2(svn_stream_t *output_stream,
                            svn_diff_t *diff,
                            const char *original_path,
                            const char *modified_path,
                            const char *latest_path,
                            const char *conflict_original,
                            const char *conflict_modified,
                            const char *conflict_latest,
                            const char *conflict_separator,
                            svn_diff_conflict_display_style_t style,
                            apr_pool_t *pool)
{
  svn_diff3__file_output_baton_t baton;
  apr_file_t *file[3];
  apr_off_t size;
  int idx;
#if APR_HAS_MMAP
  apr_mmap_t *mm[3] = { 0 };
#endif /* APR_HAS_MMAP */
  const char *eol;
  svn_boolean_t conflicts_only =
    (style == svn_diff_conflict_display_only_conflicts);

  memset(&baton, 0, sizeof(baton));
  if (conflicts_only)
    {
      baton.pool = svn_pool_create(pool);
      make_context_saver(&baton);
      baton.real_output_stream = output_stream;
    }
  else
    baton.output_stream = output_stream;
  baton.path[0] = original_path;
  baton.path[1] = modified_path;
  baton.path[2] = latest_path;
  SVN_ERR(svn_utf_cstring_from_utf8(&baton.conflict_modified,
                                    conflict_modified ? conflict_modified
                                    : apr_psprintf(pool, "<<<<<<< %s",
                                                   modified_path),
                                    pool));
  SVN_ERR(svn_utf_cstring_from_utf8(&baton.conflict_original,
                                    conflict_original ? conflict_original
                                    : apr_psprintf(pool, "||||||| %s",
                                                   original_path),
                                    pool));
  SVN_ERR(svn_utf_cstring_from_utf8(&baton.conflict_separator,
                                    conflict_separator ? conflict_separator
                                    : "=======", pool));
  SVN_ERR(svn_utf_cstring_from_utf8(&baton.conflict_latest,
                                    conflict_latest ? conflict_latest
                                    : apr_psprintf(pool, ">>>>>>> %s",
                                                   latest_path),
                                    pool));

  baton.conflict_style = style;

  for (idx = 0; idx < 3; idx++)
    {
      SVN_ERR(map_or_read_file(&file[idx],
                               MMAP_T_ARG(mm[idx])
                               &baton.buffer[idx], &size,
                               baton.path[idx], pool));

      baton.curp[idx] = baton.buffer[idx];
      baton.endp[idx] = baton.buffer[idx];

      if (baton.endp[idx])
        baton.endp[idx] += size;
    }

  /* Check what eol marker we should use for conflict markers.
     We use the eol marker of the modified file and fall back on the
     platform's eol marker if that file doesn't contain any newlines. */
  eol = svn_eol__detect_eol(baton.buffer[1], baton.endp[1]);
  if (! eol)
    eol = APR_EOL_STR;
  baton.marker_eol = eol;

  SVN_ERR(svn_diff_output(diff, &baton,
                          &svn_diff3__file_output_vtable));

  for (idx = 0; idx < 3; idx++)
    {
#if APR_HAS_MMAP
      if (mm[idx])
        {
          apr_status_t rv = apr_mmap_delete(mm[idx]);
          if (rv != APR_SUCCESS)
            {
              return svn_error_wrap_apr(rv, _("Failed to delete mmap '%s'"),
                                        baton.path[idx]);
            }
        }
#endif /* APR_HAS_MMAP */

      if (file[idx])
        {
          SVN_ERR(svn_io_file_close(file[idx], pool));
        }
    }

  if (conflicts_only)
    svn_pool_destroy(baton.pool);

  return SVN_NO_ERROR;
}

